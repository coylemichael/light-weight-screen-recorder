/*
 * kill_feed_sampler.c - Template-only kill detection (NCC match on calibrated kill-feed region) + auto-clip trigger
 *
 * Scans a calibrated screen region for "RUNNER DOWN" / "RUNNER DOWN [ASSIST]"
 * banner templates using multi-scale NCC (Normalized Cross-Correlation).
 *
 * On match: triggers WM_AUTOCLIP_SAVE to save replay as "Marathon_Kill".
 * Foreground check: only triggers when Marathon is in front.
 *
 * Scan cadence: every 2 seconds. Replay trigger cooldown: 30 seconds.
 *
 * USES: gdiplus_api (PNG loading), capture (readback)
 */

#include "kill_feed_sampler.h"
#include "gdiplus_api.h"
#include "capture.h"
#include "config.h"
#include "logger.h"
#include "debug_console.h"
#include "constants.h"
#include "mem_utils.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* How often to scan the detection region */
#define SCAN_INTERVAL_MS        2000
/* Minimum time between replay save triggers */
#define TRIGGER_COOLDOWN_MS     30000
/* NCC threshold for template match */
#define MATCH_THRESHOLD         0.80f
/* Diagnostic heartbeat / throttle intervals (gated on DebugConsole_IsOpen) */
#define SAMPLER_HEARTBEAT_MS        60000
#define SAMPLER_READBACK_LOG_MS     30000
/* Worker wait quantum so heartbeat can tick even when no work arrives */
#define SAMPLER_WORKER_WAIT_MS      10000
/* Template scale factors to try */
static const float TEMPLATE_SCALES[] = { 1.0f, 1.5f, 2.0f, 0.75f };
#define NUM_TEMPLATE_SCALES     (sizeof(TEMPLATE_SCALES) / sizeof(TEMPLATE_SCALES[0]))
/* Number of templates */
#define MAX_TEMPLATES           2

/* ─── GDI+ bitmap types (for PNG loading) ─── */

typedef void* GpBitmap;

typedef struct {
    INT X, Y, Width, Height;
} GpRect;

typedef struct {
    UINT Width;
    UINT Height;
    INT Stride;
    INT PixelFormat;
    void* Scan0;
    UINT_PTR Reserved;
} BitmapData;

#define PixelFormat32bppARGB  0x26200A
#define ImageLockModeRead     1

typedef GpStatus (WINAPI *fn_GdipCreateBitmapFromFile)(const WCHAR*, GpBitmap**);
typedef GpStatus (WINAPI *fn_GdipGetImageWidth)(void*, UINT*);
typedef GpStatus (WINAPI *fn_GdipGetImageHeight)(void*, UINT*);
typedef GpStatus (WINAPI *fn_GdipBitmapLockBits)(GpBitmap*, const GpRect*, UINT, INT, BitmapData*);
typedef GpStatus (WINAPI *fn_GdipBitmapUnlockBits)(GpBitmap*, BitmapData*);
typedef GpStatus (WINAPI *fn_GdipDisposeImage)(void*);

/* ─── Template data ─── */

typedef struct {
    BYTE* gray;         /* Grayscale pixel data */
    int w, h;           /* Dimensions */
    float mean;         /* Pre-computed mean for NCC */
    float stdDev;       /* Pre-computed stddev for NCC */
    BOOL loaded;
    char name[32];      /* Display name for logging */
} Template;

/* ─── Module state ─── */

/* Pending work item for the worker thread */
typedef struct {
    BYTE* gray;         /* Grayscale image (worker owns, frees after use) */
    BYTE* bgra;         /* BGRA copy for BMP snapshot (worker owns) */
    int w, h;
    int bgraStride;
    ULONGLONG timestamp; /* GetTickCount64 at capture time */
} ScanWork;

struct KillFeedSampler {
    /* Detection region in capture-texture coordinates */
    int kfX, kfY, kfW, kfH;

    /* Templates */
    Template templates[MAX_TEMPLATES];
    int templateCount;

    /* Timing (capture thread only) */
    ULONGLONG lastScanMs;
    DWORD captureThreadId;  /* Enforces FeedFrame single-thread precondition */

    /* Worker thread */
    HANDLE workerThread;
    HANDLE hWorkReady;      /* Auto-reset event: new work available */
    HANDLE hStopEvent;      /* Manual-reset event: shutdown requested */
    CRITICAL_SECTION workLock;
    ScanWork pendingWork;   /* Protected by workLock */
    BOOL hasPendingWork;    /* Protected by workLock */

    /* Trigger state (worker thread writes, main thread reads via WriteTriggerContext) */
    ULONGLONG lastTriggerMs;  /* Worker thread only */

    /* Overlay window for WM_AUTOCLIP_SAVE */
    HWND overlayWnd;

    /* Last trigger context (worker writes, main reads after save) */
    CRITICAL_SECTION triggerLock;
    char triggerReason[512];
    BYTE* triggerBmp;
    int triggerBmpW, triggerBmpH, triggerBmpStride;
    BOOL triggerPending;

    /* ─── Diagnostic counters (gated on DebugConsole_IsOpen for file-log emission) ─── */
    /* All counters guarded by workLock. Window resets each heartbeat. */
    ULONGLONG lastHeartbeatMs;
    ULONGLONG lastReadbackFailLogMs;
    unsigned int scansWindow;
    unsigned int feedFrameCallsWindow;   /* raw FeedFrame invocations (producer rate) */
    unsigned int feedFrameQueuedWindow;  /* invocations that passed throttle + readback and queued work */
    unsigned int readbackFails;       /* producer writes, worker reads/resets */
    unsigned int readbackFailsTotal;  /* lifetime, never reset */
    float bestScoreWindow;
    unsigned int cooldownRejects;
    unsigned int foregroundRejects;
    unsigned int belowThresholdCount;
};

/* ─── Helpers ─── */

/* Save BGRA buffer as a 32-bit BMP file */
static BOOL SaveBMP(const char* path, const BYTE* bgra, int width, int height, int stride)
{
    FILE* f = NULL;
    fopen_s(&f, path, "wb");
    if (!f) return FALSE;

    int rowBytes = width * 4;
    int imageSize = rowBytes * height;

    BYTE fileHeader[14] = {0};
    int fileSize = 14 + 40 + imageSize;
    fileHeader[0] = 'B'; fileHeader[1] = 'M';
    fileHeader[2] = (BYTE)(fileSize);
    fileHeader[3] = (BYTE)(fileSize >> 8);
    fileHeader[4] = (BYTE)(fileSize >> 16);
    fileHeader[5] = (BYTE)(fileSize >> 24);
    fileHeader[10] = 54;
    fwrite(fileHeader, 1, 14, f);

    BYTE dibHeader[40] = {0};
    dibHeader[0] = 40;
    *(int*)(dibHeader + 4) = width;
    *(int*)(dibHeader + 8) = height;
    *(short*)(dibHeader + 12) = 1;
    *(short*)(dibHeader + 14) = 32;
    *(int*)(dibHeader + 20) = imageSize;
    fwrite(dibHeader, 1, 40, f);

    for (int y = height - 1; y >= 0; y--) {
        const BYTE* row = bgra + (size_t)y * stride;
        fwrite(row, 1, rowBytes, f);
    }

    fclose(f);
    return TRUE;
}

/* Convert BGRA buffer to grayscale */
static BYTE* BgraToGray(const BYTE* bgra, int width, int height, int stride)
{
    BYTE* gray = (BYTE*)malloc((size_t)width * height);
    if (!gray) return NULL;

    for (int y = 0; y < height; y++) {
        const BYTE* src = bgra + (size_t)y * stride;
        BYTE* dst = gray + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            BYTE b = src[x * 4 + 0];
            BYTE g = src[x * 4 + 1];
            BYTE r = src[x * 4 + 2];
            dst[x] = (BYTE)((r * 299 + g * 587 + b * 114) / 1000);
        }
    }
    return gray;
}

/* Scale a grayscale buffer by a factor (nearest neighbor) */
static BYTE* ScaleGray(const BYTE* src, int srcW, int srcH, float scale, int* outW, int* outH)
{
    *outW = (int)(srcW * scale + 0.5f);
    *outH = (int)(srcH * scale + 0.5f);
    if (*outW <= 0 || *outH <= 0) return NULL;

    BYTE* dst = (BYTE*)malloc((size_t)(*outW) * (*outH));
    if (!dst) return NULL;

    for (int y = 0; y < *outH; y++) {
        int srcY = (int)(y / scale);
        if (srcY >= srcH) srcY = srcH - 1;
        for (int x = 0; x < *outW; x++) {
            int srcX = (int)(x / scale);
            if (srcX >= srcW) srcX = srcW - 1;
            dst[y * (*outW) + x] = src[srcY * srcW + srcX];
        }
    }
    return dst;
}

/* ─── Template matching (NCC) ─── */

static float TemplateMatchNCC(const BYTE* image, int imgW, int imgH,
                              const BYTE* tmpl, int tmplW, int tmplH,
                              float tmplMean, float tmplStdDev,
                              int* bestX, int* bestY)
{
    if (tmplW > imgW || tmplH > imgH) return -1.0f;
    if (tmplStdDev < 1.0f) return -1.0f;

    int tmplN = tmplW * tmplH;
    float bestScore = -1.0f;
    *bestX = 0;
    *bestY = 0;

    for (int y = 0; y <= imgH - tmplH; y++) {
        for (int x = 0; x <= imgW - tmplW; x++) {
            float imgMean = 0;
            for (int ty = 0; ty < tmplH; ty++) {
                const BYTE* row = image + (size_t)(y + ty) * imgW + x;
                for (int tx = 0; tx < tmplW; tx++)
                    imgMean += row[tx];
            }
            imgMean /= tmplN;

            float imgVar = 0;
            float crossCorr = 0;
            for (int ty = 0; ty < tmplH; ty++) {
                const BYTE* row = image + (size_t)(y + ty) * imgW + x;
                for (int tx = 0; tx < tmplW; tx++) {
                    float di = row[tx] - imgMean;
                    float dt = tmpl[ty * tmplW + tx] - tmplMean;
                    imgVar += di * di;
                    crossCorr += di * dt;
                }
            }
            float imgStd = sqrtf(imgVar);
            float ncc = (imgStd > 1.0f) ? (crossCorr / (imgStd * tmplStdDev)) : 0.0f;
            if (ncc > bestScore) {
                bestScore = ncc;
                *bestX = x;
                *bestY = y;
            }
        }
    }
    return bestScore;
}

static float TemplateMatchMultiScale(const BYTE* imageGray, int imgW, int imgH,
                                     const Template* tmpl,
                                     int* bestX, int* bestY)
{
    float overallBest = -1.0f;
    *bestX = 0;
    *bestY = 0;

    for (int s = 0; s < (int)NUM_TEMPLATE_SCALES; s++) {
        float scale = TEMPLATE_SCALES[s];
        int scaledW, scaledH;
        BYTE* scaledTmpl = NULL;
        const BYTE* useTmpl = tmpl->gray;
        int useW = tmpl->w, useH = tmpl->h;
        float useMean = tmpl->mean, useStd = tmpl->stdDev;

        if (scale != 1.0f) {
            scaledTmpl = ScaleGray(tmpl->gray, tmpl->w, tmpl->h, scale, &scaledW, &scaledH);
            if (!scaledTmpl) continue;
            useTmpl = scaledTmpl;
            useW = scaledW;
            useH = scaledH;

            int n = useW * useH;
            useMean = 0;
            for (int i = 0; i < n; i++) useMean += useTmpl[i];
            useMean /= n;
            useStd = 0;
            for (int i = 0; i < n; i++) {
                float d = useTmpl[i] - useMean;
                useStd += d * d;
            }
            useStd = sqrtf(useStd);
        }

        int mx, my;
        float score = TemplateMatchNCC(imageGray, imgW, imgH,
                                       useTmpl, useW, useH,
                                       useMean, useStd, &mx, &my);
        if (score > overallBest) {
            overallBest = score;
            *bestX = mx + useW / 2;
            *bestY = my + useH / 2;
        }
        free(scaledTmpl);
    }
    return overallBest;
}

/* ─── PNG template loading via GDI+ ─── */

static BOOL LoadTemplatePNG(Template* t, const char* pngPath, const char* name)
{
    HMODULE mod = g_gdip.module;
    if (!mod || !g_gdip.initialized) return FALSE;

    fn_GdipCreateBitmapFromFile pCreate =
        (fn_GdipCreateBitmapFromFile)GetProcAddress(mod, "GdipCreateBitmapFromFile");
    fn_GdipGetImageWidth pGetW =
        (fn_GdipGetImageWidth)GetProcAddress(mod, "GdipGetImageWidth");
    fn_GdipGetImageHeight pGetH =
        (fn_GdipGetImageHeight)GetProcAddress(mod, "GdipGetImageHeight");
    fn_GdipBitmapLockBits pLock =
        (fn_GdipBitmapLockBits)GetProcAddress(mod, "GdipBitmapLockBits");
    fn_GdipBitmapUnlockBits pUnlock =
        (fn_GdipBitmapUnlockBits)GetProcAddress(mod, "GdipBitmapUnlockBits");
    fn_GdipDisposeImage pDispose =
        (fn_GdipDisposeImage)GetProcAddress(mod, "GdipDisposeImage");

    if (!pCreate || !pGetW || !pGetH || !pLock || !pUnlock || !pDispose) return FALSE;

    wchar_t wPath[MAX_PATH];
    int wConv = MultiByteToWideChar(CP_ACP, 0, pngPath, -1, wPath, MAX_PATH);
    if (wConv <= 0) {
        Logger_Log("KillFeedSampler: MultiByteToWideChar failed for '%s' (err=%lu)\n",
                   pngPath, GetLastError());
        return FALSE;
    }

    GpBitmap* bitmap = NULL;
    if (pCreate(wPath, &bitmap) != GdipOk || !bitmap) return FALSE;

    UINT w = 0, h = 0;
    GpStatus sw = pGetW(bitmap, &w);
    GpStatus sh = pGetH(bitmap, &h);
    if (sw != GdipOk || sh != GdipOk) {
        Logger_Log("KillFeedSampler: GdipGetImageWidth/Height failed (sw=%d sh=%d)\n", sw, sh);
        pDispose(bitmap);
        return FALSE;
    }

    if (w == 0 || h == 0 || w > 1024 || h > 1024) {
        pDispose(bitmap);
        return FALSE;
    }

    GpRect rect = { 0, 0, (INT)w, (INT)h };
    BitmapData data = {0};
    if (pLock(bitmap, &rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != GdipOk) {
        pDispose(bitmap);
        return FALSE;
    }

    t->w = (int)w;
    t->h = (int)h;
    t->gray = (BYTE*)malloc((size_t)w * h);
    if (!t->gray) {
        pUnlock(bitmap, &data);
        pDispose(bitmap);
        return FALSE;
    }

    for (int y = 0; y < (int)h; y++) {
        BYTE* row = (BYTE*)data.Scan0 + (size_t)y * data.Stride;
        for (int x = 0; x < (int)w; x++) {
            BYTE b = row[x * 4 + 0];
            BYTE g = row[x * 4 + 1];
            BYTE r = row[x * 4 + 2];
            t->gray[y * (int)w + x] = (BYTE)((r * 299 + g * 587 + b * 114) / 1000);
        }
    }

    pUnlock(bitmap, &data);
    pDispose(bitmap);

    /* Pre-compute stats */
    int n = t->w * t->h;
    t->mean = 0;
    for (int i = 0; i < n; i++) t->mean += t->gray[i];
    t->mean /= n;

    float var = 0;
    for (int i = 0; i < n; i++) {
        float d = t->gray[i] - t->mean;
        var += d * d;
    }
    t->stdDev = sqrtf(var);
    t->loaded = TRUE;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';

    Logger_Log("KillFeedSampler: Template '%s' loaded: %dx%d (mean=%.1f, std=%.1f)\n",
               name, t->w, t->h, t->mean, t->stdDev);
    return TRUE;
}

/* ─── Foreground check ─── */

static BOOL IsMarathonInForeground(void)
{
    HWND fgWnd = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fgWnd) GetWindowThreadProcessId(fgWnd, &fgPid);
    if (!fgPid) return FALSE;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid);
    if (!hProc) return FALSE;

    BOOL result = FALSE;
    char exePath[MAX_PATH];
    DWORD pathSize = MAX_PATH;
    if (QueryFullProcessImageNameA(hProc, 0, exePath, &pathSize)) {
        char* slash = strrchr(exePath, '\\');
        char* base = slash ? slash + 1 : exePath;
        if (_strnicmp(base, "marathon", 8) == 0)
            result = TRUE;
    }
    CloseHandle(hProc);
    return result;
}

/* ─── Worker thread ─── */

/* Emit a throttled diagnostic heartbeat to the main log (gated on debug console).
 * Caller must NOT hold workLock. Resets windowed counters. */
static void EmitHeartbeatIfDue(KillFeedSampler* s, ULONGLONG now)
{
    EnterCriticalSection(&s->workLock);
    if (s->lastHeartbeatMs == 0) s->lastHeartbeatMs = now;
    if ((now - s->lastHeartbeatMs) < SAMPLER_HEARTBEAT_MS) {
        LeaveCriticalSection(&s->workLock);
        return;
    }

    unsigned int scans   = s->scansWindow;
    unsigned int feedCalls  = s->feedFrameCallsWindow;
    unsigned int feedQueued = s->feedFrameQueuedWindow;
    unsigned int rbFail  = s->readbackFails;
    unsigned int rbTotal = s->readbackFailsTotal;
    float bestScore      = s->bestScoreWindow;
    unsigned int cdRej   = s->cooldownRejects;
    unsigned int fgRej   = s->foregroundRejects;
    unsigned int loCnt   = s->belowThresholdCount;
    ULONGLONG lastTrig   = s->lastTriggerMs;

    s->scansWindow = 0;
    s->feedFrameCallsWindow = 0;
    s->feedFrameQueuedWindow = 0;
    s->readbackFails = 0;
    s->bestScoreWindow = -1.0f;
    s->cooldownRejects = 0;
    s->foregroundRejects = 0;
    s->belowThresholdCount = 0;
    s->lastHeartbeatMs = now;
    LeaveCriticalSection(&s->workLock);

    if (!DebugConsole_IsOpen()) return;

    long long ageMs = (lastTrig == 0) ? -1 : (long long)(now - lastTrig);
    float displayScore = (bestScore < 0.0f) ? 0.0f : bestScore;
    Logger_Log("KillFeedSampler: heartbeat feed_calls=%u feed_queued=%u scans=%u readback_fails=%u (total=%u) best_score=%.3f "
               "last_match_age_ms=%lld rejects[cd=%u fg=%u lo=%u]\n",
               feedCalls, feedQueued, scans, rbFail, rbTotal, displayScore,
               ageMs, cdRej, fgRej, loCnt);
    DebugConsole_Print("HEARTBEAT: feed=%u queued=%u scans=%u/min best=%.3f rb_fails=%u (total=%u) "
                       "last_match=%llds ago rejects[cd=%u fg=%u lo=%u]\n",
                       feedCalls, feedQueued, scans, displayScore, rbFail, rbTotal,
                       (ageMs < 0) ? -1LL : (ageMs / 1000),
                       cdRej, fgRej, loCnt);
}

static DWORD WINAPI ScanWorkerProc(LPVOID param)
{
    KillFeedSampler* s = (KillFeedSampler*)param;
    HANDLE waitHandles[2] = { s->hStopEvent, s->hWorkReady };

    while (TRUE) {
        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, SAMPLER_WORKER_WAIT_MS);
        if (wait == WAIT_OBJECT_0) break;  /* Stop event */

        /* Heartbeat tick — fires whether work arrived or wait timed out.
         * If work has dried up entirely, scans=0 in the heartbeat is itself a signal. */
        EmitHeartbeatIfDue(s, GetTickCount64());

        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0 + 1) break;  /* Error */

        /* Grab the pending work item */
        ScanWork work = {0};
        EnterCriticalSection(&s->workLock);
        if (s->hasPendingWork) {
            work = s->pendingWork;
            memset(&s->pendingWork, 0, sizeof(s->pendingWork));
            s->hasPendingWork = FALSE;
        }
        s->scansWindow++;
        LeaveCriticalSection(&s->workLock);

        if (!work.gray) continue;

        /* Try each template */
        float bestScore = -1.0f;
        int bestIdx = -1;
        int bestMx = 0, bestMy = 0;

        for (int i = 0; i < s->templateCount; i++) {
            /* Bail out mid-scan if shutdown was requested — template matching
             * is multi-ms per template and would otherwise block Shutdown. */
            if (WaitForSingleObject(s->hStopEvent, 0) == WAIT_OBJECT_0) {
                free(work.gray);
                work.gray = NULL;
                goto worker_exit;
            }
            if (!s->templates[i].loaded) continue;
            int mx, my;
            float score = TemplateMatchMultiScale(work.gray, work.w, work.h,
                                                  &s->templates[i], &mx, &my);
            if (score > 0.60f)
                DebugConsole_Print("SCAN: %s score=%.3f\n", s->templates[i].name, score);
            if (score > bestScore) {
                bestScore = score;
                bestIdx = i;
                bestMx = mx;
                bestMy = my;
            }
        }

        free(work.gray);

        /* Track windowed best score for heartbeat */
        EnterCriticalSection(&s->workLock);
        if (bestScore > s->bestScoreWindow) s->bestScoreWindow = bestScore;
        LeaveCriticalSection(&s->workLock);

        /* Check threshold */
        if (bestScore < MATCH_THRESHOLD || bestIdx < 0) {
            EnterCriticalSection(&s->workLock);
            s->belowThresholdCount++;
            LeaveCriticalSection(&s->workLock);
            if (bestScore > 0.60f)
                DebugConsole_Print("SCAN: no match (best=%.3f, need %.2f)\n",
                                  bestScore, MATCH_THRESHOLD);
            free(work.bgra);
            continue;
        }

        const char* matchedName = s->templates[bestIdx].name;
        DebugConsole_Print("MATCH: %s at (%d,%d) score=%.3f\n",
                          matchedName, bestMx, bestMy, bestScore);

        /* Check cooldown */
        ULONGLONG now = work.timestamp;
        if ((now - s->lastTriggerMs) < TRIGGER_COOLDOWN_MS) {
            EnterCriticalSection(&s->workLock);
            s->cooldownRejects++;
            LeaveCriticalSection(&s->workLock);
            DebugConsole_Print("MATCH: cooldown active (%llums left)\n",
                              TRIGGER_COOLDOWN_MS - (now - s->lastTriggerMs));
            free(work.bgra);
            continue;
        }

        /* Foreground check */
        if (!IsMarathonInForeground()) {
            EnterCriticalSection(&s->workLock);
            s->foregroundRejects++;
            LeaveCriticalSection(&s->workLock);
            DebugConsole_Print("MATCH: game not in foreground, skipping\n");
            free(work.bgra);
            continue;
        }

        /* ─── Trigger! ─── */
        EnterCriticalSection(&s->workLock);
        s->lastTriggerMs = now;
        LeaveCriticalSection(&s->workLock);

        Logger_Log("KillFeedSampler: Kill detected! (%s, score=%.3f) Triggering replay save.\n",
                   matchedName, bestScore);
        DebugConsole_Print("TRIGGER: Marathon_Kill (%s, score=%.3f) -> saving replay\n",
                          matchedName, bestScore);

        /* Store trigger context (protected by triggerLock) */
        EnterCriticalSection(&s->triggerLock);
        snprintf(s->triggerReason, sizeof(s->triggerReason),
                 "Auto-Clip Trigger Report\n"
                 "========================\n"
                 "Type: Kill\n"
                 "Template: %s\n"
                 "Match Score: %.4f (threshold: %.2f)\n"
                 "Match Position: (%d, %d)\n"
                 "Detection Region: (%d,%d) %dx%d\n",
                 matchedName, bestScore, MATCH_THRESHOLD,
                 bestMx, bestMy,
                 s->kfX, s->kfY, s->kfW, s->kfH);

        free(s->triggerBmp);
        s->triggerBmp = work.bgra;  /* Take ownership */
        work.bgra = NULL;
        s->triggerBmpW = work.w;
        s->triggerBmpH = work.h;
        s->triggerBmpStride = work.bgraStride;
        s->triggerPending = TRUE;
        LeaveCriticalSection(&s->triggerLock);

        /* PostMessage with heap-allocated game name */
        if (s->overlayWnd) {
            const char* gameName = "Marathon_Kill";
            size_t len = strlen(gameName) + 1;
            char* nameCopy = (char*)malloc(len);
            if (nameCopy) {
                memcpy(nameCopy, gameName, len);
                PostMessage(s->overlayWnd, WM_AUTOCLIP_SAVE, 0, (LPARAM)nameCopy);
            }
        }

        free(work.bgra);
    }

worker_exit:
    return 0;
}

/* ─── Public API ─── */

KillFeedSampler* KillFeedSampler_Init(const AppConfig* config, const CaptureState* capture,
                                      HWND overlayWnd)
{
    if (!config || !capture) return NULL;

    /* Detection region must be calibrated */
    if (config->killfeedWPct <= 0.0f || config->killfeedHPct <= 0.0f) {
        Logger_Log("KillFeedSampler: Detection region not calibrated - disabled\n");
        return NULL;
    }

    int captureWidth = capture->captureWidth;
    int captureHeight = capture->captureHeight;
    if (captureWidth <= 0 || captureHeight <= 0) return NULL;

    KillFeedSampler* s = (KillFeedSampler*)calloc(1, sizeof(KillFeedSampler));
    if (!s) return NULL;

    s->bestScoreWindow = -1.0f;

    /* Resolve detection region */
    int monW = capture->monitorWidth;
    int monH = capture->monitorHeight;
    int cropX = capture->captureRect.left;
    int cropY = capture->captureRect.top;

    if (monW <= 0 || monH <= 0) {
        monW = captureWidth;
        monH = captureHeight;
        cropX = 0;
        cropY = 0;
    }

    s->kfX = (int)(config->killfeedXPct * monW) - cropX;
    s->kfY = (int)(config->killfeedYPct * monH) - cropY;
    s->kfW = (int)(config->killfeedWPct * monW);
    s->kfH = (int)(config->killfeedHPct * monH);

    /* Clamp to capture bounds */
    if (s->kfX < 0) { s->kfW += s->kfX; s->kfX = 0; }
    if (s->kfY < 0) { s->kfH += s->kfY; s->kfY = 0; }
    if (s->kfX + s->kfW > captureWidth)  s->kfW = captureWidth - s->kfX;
    if (s->kfY + s->kfH > captureHeight) s->kfH = captureHeight - s->kfY;

    if (s->kfW <= 0 || s->kfH <= 0) {
        Logger_Log("KillFeedSampler: Detection region outside capture area\n");
        free(s);
        return NULL;
    }

    /* Load templates */
    {
        char exePath[MAX_PATH];
        DWORD gmfn = GetModuleFileNameA(NULL, exePath, MAX_PATH);
        if (gmfn == 0 || gmfn >= MAX_PATH) {
            Logger_Log("KillFeedSampler: GetModuleFileNameA failed/truncated (ret=%lu err=%lu)\n",
                       gmfn, GetLastError());
            free(s);
            return NULL;
        }
        char* slash = strrchr(exePath, '\\');
        if (slash) *(slash + 1) = '\0';

        char pngPath[MAX_PATH];

        /* Template 1: runner_down.png */
        snprintf(pngPath, MAX_PATH, "%sstatic\\runner_down.png", exePath);
        if (LoadTemplatePNG(&s->templates[s->templateCount], pngPath, "runner_down"))
            s->templateCount++;
        else
            Logger_Log("KillFeedSampler: WARNING - runner_down.png not found: %s\n", pngPath);
    }

    if (s->templateCount == 0) {
        Logger_Log("KillFeedSampler: No templates loaded - disabled\n");
        free(s);
        return NULL;
    }

    s->overlayWnd = overlayWnd;
    s->lastScanMs = 0;
    s->lastTriggerMs = 0;
    s->captureThreadId = 0;  /* Captured on first FeedFrame call */

    /*
     * MULTI-RESOURCE FUNCTION: KillFeedSampler_Init (sync setup)
     * Resources: 5 - 2 CRITICAL_SECTIONs, 2 events, 1 worker thread
     * Pattern: goto-cleanup with SAFE_*
     */
    BOOL workLockInit = FALSE;
    BOOL triggerLockInit = FALSE;

    InitializeCriticalSection(&s->workLock);
    workLockInit = TRUE;
    InitializeCriticalSection(&s->triggerLock);
    triggerLockInit = TRUE;

    s->hWorkReady = CreateEvent(NULL, FALSE, FALSE, NULL);   /* Auto-reset */
    s->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);    /* Manual-reset */
    if (!s->hWorkReady || !s->hStopEvent) {
        Logger_Log("KillFeedSampler: Failed to create events\n");
        goto init_fail;
    }

    s->workerThread = CreateThread(NULL, 0, ScanWorkerProc, s, 0, NULL);
    if (!s->workerThread) {
        Logger_Log("KillFeedSampler: Failed to create worker thread\n");
        goto init_fail;
    }

    Logger_Log("KillFeedSampler: Initialized, region (%d,%d) %dx%d, %d templates loaded\n",
               s->kfX, s->kfY, s->kfW, s->kfH, s->templateCount);

    return s;

init_fail:
    SAFE_CLOSE_HANDLE(s->hWorkReady);
    SAFE_CLOSE_HANDLE(s->hStopEvent);
    if (workLockInit) DeleteCriticalSection(&s->workLock);
    if (triggerLockInit) DeleteCriticalSection(&s->triggerLock);
    for (int i = 0; i < MAX_TEMPLATES; i++) SAFE_FREE(s->templates[i].gray);
    free(s);
    return NULL;
}

void KillFeedSampler_FeedFrame(KillFeedSampler* s, CaptureState* capture,
                               ID3D11Texture2D* bgraTexture)
{
    if (!s || !capture || !bgraTexture) return;

    /* Defend the "capture thread only" precondition: latch first caller, reject others. */
    DWORD tid = GetCurrentThreadId();
    if (s->captureThreadId == 0) {
        s->captureThreadId = tid;
    } else if (s->captureThreadId != tid) {
        LWSR_ASSERT_MSG(FALSE, "KillFeedSampler_FeedFrame called from non-capture thread");
        return;
    }

    /* Count raw producer invocations (before throttle) so the heartbeat can
     * discriminate worker-bottleneck vs producer-starvation. */
    EnterCriticalSection(&s->workLock);
    s->feedFrameCallsWindow++;
    LeaveCriticalSection(&s->workLock);

    /* Throttle: only scan every SCAN_INTERVAL_MS */
    ULONGLONG now = GetTickCount64();
    if ((now - s->lastScanMs) < SCAN_INTERVAL_MS) return;
    s->lastScanMs = now;

    /* Read back detection region from GPU (must happen on D3D11 thread) */
    BYTE* buf = (BYTE*)malloc((size_t)s->kfW * s->kfH * 4);
    if (!buf) return;

    int stride = 0;
    BOOL ok = Capture_ReadbackRegion(capture, bgraTexture,
                                     s->kfX, s->kfY, s->kfW, s->kfH,
                                     buf, &stride);
    if (!ok) {
        EnterCriticalSection(&s->workLock);
        s->readbackFails++;
        s->readbackFailsTotal++;
        BOOL shouldLog = DebugConsole_IsOpen() &&
                         ((now - s->lastReadbackFailLogMs) >= SAMPLER_READBACK_LOG_MS);
        if (shouldLog) s->lastReadbackFailLogMs = now;
        unsigned int total = s->readbackFailsTotal;
        LeaveCriticalSection(&s->workLock);
        if (shouldLog)
            Logger_Log("KillFeedSampler: Capture_ReadbackRegion failed (total=%u)\n", total);
        free(buf);
        return;
    }

    /* Convert to grayscale on capture thread (fast, O(n)) */
    BYTE* grayBuf = BgraToGray(buf, s->kfW, s->kfH, stride);
    if (!grayBuf) {
        free(buf);
        return;
    }

    /* Compact BGRA copy for worker (remove stride padding) */
    BYTE* bgraCopy = (BYTE*)malloc((size_t)s->kfW * s->kfH * 4);
    if (bgraCopy) {
        for (int y = 0; y < s->kfH; y++) {
            memcpy(bgraCopy + (size_t)y * s->kfW * 4,
                   buf + (size_t)y * stride, (size_t)s->kfW * 4);
        }
    }
    free(buf);

    /* Queue work for the worker thread (replaces any stale pending work) */
    EnterCriticalSection(&s->workLock);
    /* Free any unconsumed previous work */
    SAFE_FREE(s->pendingWork.gray);
    SAFE_FREE(s->pendingWork.bgra);
    s->pendingWork.gray = grayBuf;
    s->pendingWork.bgra = bgraCopy;
    s->pendingWork.w = s->kfW;
    s->pendingWork.h = s->kfH;
    s->pendingWork.bgraStride = s->kfW * 4;
    s->pendingWork.timestamp = now;
    s->hasPendingWork = TRUE;
    s->feedFrameQueuedWindow++;
    LeaveCriticalSection(&s->workLock);

    SetEvent(s->hWorkReady);
}

void KillFeedSampler_Shutdown(KillFeedSampler* s)
{
    if (!s) return;

    /* Signal worker to stop and wait */
    if (s->hStopEvent) SetEvent(s->hStopEvent);
    if (s->workerThread) {
        WaitForSingleObject(s->workerThread, 1000);
        SAFE_CLOSE_HANDLE(s->workerThread);
    }
    SAFE_CLOSE_HANDLE(s->hWorkReady);
    SAFE_CLOSE_HANDLE(s->hStopEvent);

    /* Free any unconsumed work */
    SAFE_FREE(s->pendingWork.gray);
    SAFE_FREE(s->pendingWork.bgra);

    DeleteCriticalSection(&s->workLock);
    DeleteCriticalSection(&s->triggerLock);

    for (int i = 0; i < MAX_TEMPLATES; i++)
        SAFE_FREE(s->templates[i].gray);
    SAFE_FREE(s->triggerBmp);
    free(s);
}

void KillFeedSampler_WriteTriggerContext(KillFeedSampler* s, const char* clipPath, BOOL debugMode)
{
    if (!s || !clipPath || !clipPath[0]) return;

    EnterCriticalSection(&s->triggerLock);
    if (!s->triggerPending) {
        LeaveCriticalSection(&s->triggerLock);
        return;
    }

    /* Build .txt path from clip path */
    char txtPath[MAX_PATH];
    strncpy(txtPath, clipPath, MAX_PATH - 1);
    txtPath[MAX_PATH - 1] = '\0';
    char* ext = strrchr(txtPath, '.');
    if (ext) {
        size_t remain = (size_t)(MAX_PATH - (ext - txtPath));
        strncpy_s(ext, remain, ".txt", _TRUNCATE);
    } else {
        strncat(txtPath, ".txt", MAX_PATH - strlen(txtPath) - 1);
    }

    FILE* f = NULL;
    fopen_s(&f, txtPath, "w");
    if (f) {
        fprintf(f, "%s", s->triggerReason);
        fprintf(f, "\nClip: %s\n", clipPath);
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
        fclose(f);
    }

    /* Save BMP alongside clip when debug mode is on */
    if (debugMode && s->triggerBmp && s->triggerBmpW > 0 && s->triggerBmpH > 0) {
        char bmpPath[MAX_PATH];
        strncpy(bmpPath, clipPath, MAX_PATH - 1);
        bmpPath[MAX_PATH - 1] = '\0';
        ext = strrchr(bmpPath, '.');
        if (ext) {
            size_t remain = (size_t)(MAX_PATH - (ext - bmpPath));
            strncpy_s(ext, remain, "_region.bmp", _TRUNCATE);
        } else {
            strncat(bmpPath, "_region.bmp", MAX_PATH - strlen(bmpPath) - 1);
        }

        SaveBMP(bmpPath, s->triggerBmp, s->triggerBmpW, s->triggerBmpH, s->triggerBmpStride);
    }

    s->triggerPending = FALSE;
    SAFE_FREE(s->triggerBmp);
    LeaveCriticalSection(&s->triggerLock);
}

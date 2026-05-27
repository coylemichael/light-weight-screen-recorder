/*
 * Kill Detector - Player name auto-detection from screen region
 *
 * Architecture:
 *   Capture thread (replay_buffer.c) calls KillDetector_FeedFrame()
 *   every frame. On a 3-second cadence, it reads back the player name
 *   region from the GPU texture, runs OCR with pre-processing, and
 *   validates the NAME#NNNN pattern.
 *
 * Once the player name is detected, it's stored for future use by
 * kill feed matching (to be implemented as a separate module).
 *
 * USES: ocr_engine, ocr_preview, capture (readback), config, logger
 */

#include "kill_detector.h"
#include "ocr_engine.h"
#include "ocr_preview.h"
#include "constants.h"
#include "logger.h"
#include "debug_console.h"
#include "mem_utils.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* How many player name OCR attempts before giving up auto-detect */
#define AUTODETECT_MAX_ATTEMPTS  30
/* Interval between auto-detect attempts (ms) */
#define AUTODETECT_INTERVAL_MS   3000
/* Minimum similarity % to consider a detection consistent with candidate */
#define CONFIDENCE_THRESHOLD     90
/* How many consistent detections required to lock the name */
#define REQUIRED_CONSISTENT_HITS 2

struct KillDetector {
    /* OCR engine */
    OcrEngine* ocr;

    /* Player name (protected by cs) */
    CRITICAL_SECTION cs;
    char playerName[64];     /* just the name part, e.g. "BLACKHAND" */
    char playerTag[64];      /* full tag, e.g. "BLACKHAND#3221" (for diagnostics) */

    /* Candidate tag (not yet confirmed — needs consistent detections) */
    char candidateTag[64];   /* e.g. "BLACKHAND#3221" */
    char candidateName[64];  /* e.g. "BLACKHAND" */
    int candidateHits;       /* how many times we've seen this candidate consistently */

    /* Player name region (pixels, for name auto-detect) */
    int bdX, bdY, bdW, bdH;

    /* Auto-detect state */
    volatile LONG nameDetected;      /* 1 once name is confirmed */
    int detectAttemptsLeft;          /* countdown of name OCR attempts */
    ULONGLONG lastDetectAttemptMs;

    /* Overlay window for notifications */
    HWND overlayWnd;

    /* Debug: show OCR pre-processing stages */
    volatile LONG showProcessing;
};

/* ─── Helper: case-insensitive similarity % between two tags ─── */
static int TagSimilarityPct(const char* a, const char* b)
{
    size_t lenA = strlen(a);
    size_t lenB = strlen(b);
    if (lenA == 0 || lenB == 0) return 0;
    size_t maxLen = (lenA > lenB) ? lenA : lenB;
    size_t minLen = (lenA < lenB) ? lenA : lenB;
    int matches = 0;
    for (size_t i = 0; i < minLen; i++) {
        if (toupper((unsigned char)a[i]) == toupper((unsigned char)b[i]))
            matches++;
    }
    return (int)((matches * 100) / maxLen);
}

/* ─── Public API ─── */

KillDetector* KillDetector_Init(const AppConfig* config, const CaptureState* capture,
                                HWND overlayWnd)
{
    if (!config || !capture || !config->autoClipEnabled)
        return NULL;

    int captureWidth = capture->captureWidth;
    int captureHeight = capture->captureHeight;
    if (captureWidth <= 0 || captureHeight <= 0)
        return NULL;

    /* Check that player name region is calibrated */
    if (config->badgeWPct <= 0.0f || config->badgeHPct <= 0.0f) {
        Logger_Log("KillDetector: Player name region not calibrated - disabled\n");
        return NULL;
    }

    KillDetector* det = (KillDetector*)calloc(1, sizeof(KillDetector));
    if (!det) return NULL;

    InitializeCriticalSection(&det->cs);

    /*
     * Region percentages are calibrated against the FULL MONITOR.
     * The capture texture may be a crop (e.g. 16:9 from a 32:9 monitor).
     * Resolve against monitor dims, then subtract capture rect origin
     * to get coordinates within the capture texture.
     */
    int monW = capture->monitorWidth;
    int monH = capture->monitorHeight;
    int cropX = capture->captureRect.left;
    int cropY = capture->captureRect.top;

    /* If monitor dims unavailable, fall back to capture dims (no crop) */
    if (monW <= 0 || monH <= 0) {
        monW = captureWidth;
        monH = captureHeight;
        cropX = 0;
        cropY = 0;
    }

    /* Resolve player name region to absolute monitor pixel coordinates */
    int bdAbsX = (int)(config->badgeXPct * monW);
    int bdAbsY = (int)(config->badgeYPct * monH);
    int bdAbsW = (int)(config->badgeWPct * monW);
    int bdAbsH = (int)(config->badgeHPct * monH);

    /* Convert to capture-texture-relative coordinates */
    det->bdX = bdAbsX - cropX;
    det->bdY = bdAbsY - cropY;
    det->bdW = bdAbsW;
    det->bdH = bdAbsH;

    /* Clamp to capture bounds */
    if (det->bdX < 0) { det->bdW += det->bdX; det->bdX = 0; }
    if (det->bdY < 0) { det->bdH += det->bdY; det->bdY = 0; }
    if (det->bdX + det->bdW > captureWidth)  det->bdW = captureWidth - det->bdX;
    if (det->bdY + det->bdH > captureHeight) det->bdH = captureHeight - det->bdY;

    /* Warn if player name region is outside crop */
    if (det->bdW <= 0 || det->bdH <= 0) {
        Logger_Log("KillDetector: WARNING - Player name region outside capture area\n");
        Logger_Log("  Name region on monitor: (%d,%d) %dx%d, crop starts at (%d,%d)\n",
                   bdAbsX, bdAbsY, bdAbsW, bdAbsH, cropX, cropY);
    }

    Logger_Log("KillDetector: Monitor %dx%d, Capture crop (%d,%d) %dx%d\n",
               monW, monH, cropX, cropY, captureWidth, captureHeight);
    Logger_Log("KillDetector: Player name region: (%d,%d) %dx%d (from monitor abs %d,%d)\n",
               det->bdX, det->bdY, det->bdW, det->bdH, bdAbsX, bdAbsY);

    /* Initialize OCR engine (looks for tessdata next to the exe) */
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (slash) *(slash + 1) = '\0';

    det->ocr = OcrEngine_Init(exePath);
    if (!det->ocr) {
        Logger_Log("KillDetector: OCR init failed - auto-clip disabled\n");
        goto fail;
    }

    /* Initialize state */
    det->playerName[0] = '\0';
    det->playerTag[0] = '\0';
    det->candidateTag[0] = '\0';
    det->candidateName[0] = '\0';
    det->candidateHits = 0;
    det->overlayWnd = overlayWnd;
    det->showProcessing = config->autoClipShowProcessing ? 1 : 0;

    det->nameDetected = 0;
    det->detectAttemptsLeft = AUTODETECT_MAX_ATTEMPTS;
    det->lastDetectAttemptMs = 0;
    Logger_Log("KillDetector: Will auto-detect player name (max %d attempts, every %dms)\n",
               AUTODETECT_MAX_ATTEMPTS, AUTODETECT_INTERVAL_MS);

    return det;

fail:
    KillDetector_Shutdown(det);
    return NULL;
}

void KillDetector_FeedFrame(KillDetector* det, CaptureState* capture,
                            ID3D11Texture2D* bgraTexture)
{
    if (!det || !capture || !bgraTexture) return;

    ULONGLONG now = GetTickCount64();

    /*
     * Auto-detect player name from name region.
     * Runs every AUTODETECT_INTERVAL_MS. Gives up after max attempts
     * unless showProcessing is active (keeps running for diagnostics).
     */
    BOOL showProc = InterlockedCompareExchange(&det->showProcessing, 0, 0) != 0;
    BOOL nameKnown = InterlockedCompareExchange(&det->nameDetected, 0, 0) != 0;
    BOOL shouldDetect = (!nameKnown && det->detectAttemptsLeft > 0) || showProc;

    if (shouldDetect &&
        (now - det->lastDetectAttemptMs) >= AUTODETECT_INTERVAL_MS) {
        det->lastDetectAttemptMs = now;
        if (!nameKnown) det->detectAttemptsLeft--;
        if (KillDetector_DetectPlayerName(det, capture, bgraTexture)) {
            if (!nameKnown) {
                InterlockedExchange(&det->nameDetected, 1);
                Logger_Log("KillDetector: Name auto-detected, %d attempts remaining (unused)\n",
                           det->detectAttemptsLeft);
            }
        } else if (!nameKnown && det->detectAttemptsLeft == 0) {
            Logger_Log("KillDetector: Auto-detect exhausted %d attempts\n",
                       AUTODETECT_MAX_ATTEMPTS);
        }
    }
}

void KillDetector_SetPlayerName(KillDetector* det, const char* name)
{
    if (!det) return;
    EnterCriticalSection(&det->cs);
    if (name) {
        strncpy(det->playerName, name, sizeof(det->playerName) - 1);
        det->playerName[sizeof(det->playerName) - 1] = '\0';
    } else {
        det->playerName[0] = '\0';
    }
    LeaveCriticalSection(&det->cs);
}

void KillDetector_GetPlayerName(KillDetector* det, char* out, int outSize)
{
    if (!det || !out || outSize <= 0) return;
    EnterCriticalSection(&det->cs);
    strncpy(out, det->playerName, outSize - 1);
    out[outSize - 1] = '\0';
    LeaveCriticalSection(&det->cs);
}

BOOL KillDetector_DetectPlayerName(KillDetector* det, CaptureState* capture,
                                   ID3D11Texture2D* bgraTexture)
{
    if (!det || !capture || !bgraTexture || !det->ocr) return FALSE;
    if (det->bdW <= 0 || det->bdH <= 0) return FALSE;

    /* Read back player name region from GPU */
    int nameBufSize = det->bdW * det->bdH * 4;
    BYTE* nameBuf = (BYTE*)malloc(nameBufSize);
    if (!nameBuf) return FALSE;

    int stride = 0;
    BOOL ok = Capture_ReadbackRegion(capture, bgraTexture,
                                     det->bdX, det->bdY, det->bdW, det->bdH,
                                     nameBuf, &stride);
    if (!ok) {
        free(nameBuf);
        return FALSE;
    }

    BOOL showProc = InterlockedCompareExchange(&det->showProcessing, 0, 0) != 0;

    /* Pass player tag for stage scoring: locked tag first, candidate tag as fallback */
    char scoreTag[64] = {0};
    EnterCriticalSection(&det->cs);
    if (det->playerTag[0])
        strncpy(scoreTag, det->playerTag, sizeof(scoreTag) - 1);
    else if (det->candidateTag[0])
        strncpy(scoreTag, det->candidateTag, sizeof(scoreTag) - 1);
    LeaveCriticalSection(&det->cs);

    char* text = OcrEngine_Recognize(det->ocr, nameBuf, det->bdW, det->bdH, 4, stride,
                                     showProc, scoreTag[0] ? scoreTag : NULL);
    free(nameBuf);

    if (!text) return FALSE;

    DebugConsole_Print("NAME OCR: '%s'\n", text);

    /* Post raw OCR text to overlay for debug display */
    if (det->overlayWnd) {
        size_t rawLen = strlen(text) + 1;
        char* rawCopy = (char*)malloc(rawLen);
        if (rawCopy) {
            memcpy(rawCopy, text, rawLen);
            PostMessage(det->overlayWnd, WM_AUTOCLIP_RAW_OCR, 0, (LPARAM)rawCopy);
        }
    }

    /*
     * Parse player name format: "[prefix] NAME#DIGITS"
     * Examples: "A1 BLACKHAND#3221", "B2 SHERPA MIND#4639"
     *
     * Strategy: Replace newlines with spaces, find '#', verify
     * exactly 4 digits after it, then take the last space-delimited
     * word before '#' as the player name.
     */
    char detectedName[64] = {0};
    char detectedDigits[5] = {0};

    /* Normalize: replace newlines/tabs with spaces */
    for (char* p = text; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }

    char* hash = strchr(text, '#');
    if (hash && hash > text) {
        /* Verify exactly 4 digits after # */
        char* digits = hash + 1;
        int digitCount = 0;
        while (digits[digitCount] >= '0' && digits[digitCount] <= '9') digitCount++;

        if (digitCount == 4) {
            /* Find the last space before '#' to isolate the name */
            char* nameStart = text;
            char* lastSpace = NULL;
            for (char* p = text; p < hash; p++) {
                if (*p == ' ') lastSpace = p;
            }
            if (lastSpace && lastSpace > text && lastSpace < hash - 1) {
                nameStart = lastSpace + 1;
            }

            /* Trim trailing spaces */
            size_t nameLen = hash - nameStart;
            while (nameLen > 0 && nameStart[nameLen - 1] == ' ') nameLen--;
            if (nameLen > 0 && nameLen < sizeof(detectedName)) {
                memcpy(detectedName, nameStart, nameLen);
                detectedName[nameLen] = '\0';
                memcpy(detectedDigits, hash + 1, 4);
                detectedDigits[4] = '\0';
            }
        }
    }

    free(text);

    /* If name is already locked, skip candidate matching (showProcessing keeps
     * OCR running for diagnostics only — we must not re-lock or mutate state) */
    BOOL alreadyLocked = InterlockedCompareExchange(&det->nameDetected, 0, 0) != 0;
    if (alreadyLocked) return FALSE;

    if (detectedName[0] != '\0') {
        /* Build candidate tag string */
        char newTag[64];
        snprintf(newTag, sizeof(newTag), "%s#%s", detectedName, detectedDigits);

        /* Compare against current candidate */
        int sim = TagSimilarityPct(newTag, det->candidateTag);

        if (sim >= CONFIDENCE_THRESHOLD && det->candidateTag[0] != '\0') {
            /* Consistent with existing candidate */
            det->candidateHits++;
            DebugConsole_Print("NAME CANDIDATE: '%s' hit %d/%d (%d%% match)\n",
                              newTag, det->candidateHits, REQUIRED_CONSISTENT_HITS, sim);

            if (det->candidateHits >= REQUIRED_CONSISTENT_HITS) {
                /* Lock it in! */
                KillDetector_SetPlayerName(det, det->candidateName);

                EnterCriticalSection(&det->cs);
                strncpy(det->playerTag, det->candidateTag, sizeof(det->playerTag) - 1);
                LeaveCriticalSection(&det->cs);

                Logger_Log("KillDetector: LOCKED player name: '%s' (tag: %s, %d consistent hits)\n",
                           det->candidateName, det->candidateTag, det->candidateHits);
                DebugConsole_Print("NAME LOCKED: '%s' -> CONFIRMED\n", det->candidateTag);

                /* Notify overlay */
                if (det->overlayWnd) {
                    size_t len = strlen(det->candidateName) + 1;
                    char* nameCopy = (char*)malloc(len);
                    if (nameCopy) {
                        memcpy(nameCopy, det->candidateName, len);
                        PostMessage(det->overlayWnd, WM_AUTOCLIP_NAME_DETECTED, 0, (LPARAM)nameCopy);
                    }
                }
                return TRUE;
            }
        } else {
            /* New or different candidate — reset */
            strncpy(det->candidateTag, newTag, sizeof(det->candidateTag) - 1);
            det->candidateTag[sizeof(det->candidateTag) - 1] = '\0';
            strncpy(det->candidateName, detectedName, sizeof(det->candidateName) - 1);
            det->candidateName[sizeof(det->candidateName) - 1] = '\0';
            det->candidateHits = 1;
            DebugConsole_Print("NAME CANDIDATE: '%s' (new, %d%% vs prev)\n", newTag, sim);
        }
    }

    return FALSE;
}

void KillDetector_SetShowProcessing(KillDetector* det, BOOL show)
{
    if (!det) return;
    InterlockedExchange(&det->showProcessing, show ? 1 : 0);
    if (!show) {
        OcrPreview_CloseAll();
    }
}

void KillDetector_Shutdown(KillDetector* det)
{
    if (!det) return;

    OcrPreview_DestroyAll();
    OcrEngine_Shutdown(det->ocr);
    DeleteCriticalSection(&det->cs);
    free(det);
}

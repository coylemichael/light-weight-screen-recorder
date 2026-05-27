/*
 * OCR Engine - Tesseract C API wrapper with dynamic loading
 *
 * Loads tesseract53.dll (or tesseract.dll) and leptonica at runtime.
 * If the DLLs are not present, OcrEngine_Init returns NULL and the
 * auto-clip feature is silently disabled.
 *
 * Tesseract C API used:
 *   TessBaseAPICreate, TessBaseAPIInit3, TessBaseAPISetPageSegMode,
 *   TessBaseAPISetImage, TessBaseAPIGetUTF8Text, TessBaseAPIDelete,
 *   TessDeleteText, TessBaseAPISetVariable
 */

#include "ocr_engine.h"
#include "ocr_preview.h"
#include "debug_console.h"
#include "logger.h"
#include "mem_utils.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Tesseract C API types */
typedef void* TessBaseAPI;

/* Page segmentation modes we care about */
#define PSM_AUTO  3
#define PSM_SINGLE_LINE 7

/* Function pointer types for Tesseract C API */
typedef TessBaseAPI (*fn_TessBaseAPICreate)(void);
typedef int         (*fn_TessBaseAPIInit3)(TessBaseAPI, const char*, const char*);
typedef void        (*fn_TessBaseAPISetPageSegMode)(TessBaseAPI, int);
typedef void        (*fn_TessBaseAPISetImage)(TessBaseAPI, const unsigned char*, int, int, int, int);
typedef char*       (*fn_TessBaseAPIGetUTF8Text)(TessBaseAPI);
typedef void        (*fn_TessBaseAPIDelete)(TessBaseAPI);
typedef void        (*fn_TessDeleteText)(char*);
typedef int         (*fn_TessBaseAPISetVariable)(TessBaseAPI, const char*, const char*);

struct OcrEngine {
    HMODULE hTesseract;
    TessBaseAPI api;
    
    /* Function pointers */
    fn_TessBaseAPICreate        Create;
    fn_TessBaseAPIInit3         Init3;
    fn_TessBaseAPISetPageSegMode SetPageSegMode;
    fn_TessBaseAPISetImage      SetImage;
    fn_TessBaseAPIGetUTF8Text   GetUTF8Text;
    fn_TessBaseAPIDelete        Delete;
    fn_TessDeleteText           DeleteText;
    fn_TessBaseAPISetVariable   SetVariable;
};

/* DLL names to try (MinGW and MSVC naming conventions) */
static const char* TESS_DLL_NAMES[] = {
    "libtesseract-5.dll",  /* MSYS2/MinGW builds (UB-Mannheim, winget) */
    "tesseract55.dll",     /* MSVC/vcpkg builds */
    "tesseract53.dll",
    "tesseract52.dll",
    "tesseract.dll",
    NULL
};

/*
 * Try loading the Tesseract DLL. Search order:
 *   1. <exe_dir>\ocr\ subdirectory (self-contained bundle)
 *   2. <exe_dir> itself (DLLs next to exe)
 *
 * SetDllDirectoryA is used so the DLL's own dependencies (leptonica,
 * MinGW runtime, image libs) resolve from the same directory.
 *
 * ocrDir is filled with the directory the DLL was loaded from.
 */
static HMODULE LoadTesseractDll(char* ocrDir, int ocrDirSize)
{
    ocrDir[0] = '\0';
    
    /* Build <exe_dir>\ocr\ path */
    char exeDir[MAX_PATH];
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) *(slash + 1) = '\0';
    
    /* Search locations in priority order */
    char searchDirs[2][MAX_PATH];
    snprintf(searchDirs[0], MAX_PATH, "%socr", exeDir);  /* <exe>\ocr\ */
    strncpy(searchDirs[1], exeDir, MAX_PATH - 1);        /* <exe>\ */
    
    for (int d = 0; d < 2; d++) {
        SetDllDirectoryA(searchDirs[d]);
        for (int i = 0; TESS_DLL_NAMES[i]; i++) {
            char fullPath[MAX_PATH];
            snprintf(fullPath, MAX_PATH, "%s\\%s", searchDirs[d], TESS_DLL_NAMES[i]);
            HMODULE h = LoadLibraryA(fullPath);
            if (h) {
                Logger_Log("OCR: Loaded %s from %s\n", TESS_DLL_NAMES[i], searchDirs[d]);
                strncpy(ocrDir, searchDirs[d], ocrDirSize - 1);
                ocrDir[ocrDirSize - 1] = '\0';
                SetDllDirectoryA(NULL);
                return h;
            }
        }
    }
    SetDllDirectoryA(NULL);
    return NULL;
}

OcrEngine* OcrEngine_Init(const char* dataPath)
{
    (void)dataPath;  /* Now unused - DLL dir is used as tessdata path */
    OcrEngine* engine = (OcrEngine*)calloc(1, sizeof(OcrEngine));
    if (!engine) return NULL;

    char installDir[MAX_PATH] = {0};
    engine->hTesseract = LoadTesseractDll(installDir, sizeof(installDir));
    if (!engine->hTesseract) {
        Logger_Log("OCR: Tesseract DLL not found - auto-clip disabled\n");
        Logger_Log("OCR: Place Tesseract DLLs + tessdata in <exe>\\ocr\\\n");
        free(engine);
        return NULL;
    }

    /*
     * Use the directory the DLL was loaded from as the tessdata path.
     * Tesseract's Init3 expects the PARENT of tessdata/.
     */
    const char* effectiveDataPath = installDir;

    /* Resolve all function pointers */
    engine->Create         = (fn_TessBaseAPICreate)GetProcAddress(engine->hTesseract, "TessBaseAPICreate");
    engine->Init3          = (fn_TessBaseAPIInit3)GetProcAddress(engine->hTesseract, "TessBaseAPIInit3");
    engine->SetPageSegMode = (fn_TessBaseAPISetPageSegMode)GetProcAddress(engine->hTesseract, "TessBaseAPISetPageSegMode");
    engine->SetImage       = (fn_TessBaseAPISetImage)GetProcAddress(engine->hTesseract, "TessBaseAPISetImage");
    engine->GetUTF8Text    = (fn_TessBaseAPIGetUTF8Text)GetProcAddress(engine->hTesseract, "TessBaseAPIGetUTF8Text");
    engine->Delete         = (fn_TessBaseAPIDelete)GetProcAddress(engine->hTesseract, "TessBaseAPIDelete");
    engine->DeleteText     = (fn_TessDeleteText)GetProcAddress(engine->hTesseract, "TessDeleteText");
    engine->SetVariable    = (fn_TessBaseAPISetVariable)GetProcAddress(engine->hTesseract, "TessBaseAPISetVariable");

    if (!engine->Create || !engine->Init3 || !engine->SetImage ||
        !engine->GetUTF8Text || !engine->Delete || !engine->DeleteText) {
        Logger_Log("OCR: Failed to resolve Tesseract API functions\n");
        FreeLibrary(engine->hTesseract);
        free(engine);
        return NULL;
    }

    /* Create and initialize the API */
    engine->api = engine->Create();
    if (!engine->api) {
        Logger_Log("OCR: TessBaseAPICreate failed\n");
        FreeLibrary(engine->hTesseract);
        free(engine);
        return NULL;
    }

    int rc = engine->Init3(engine->api, effectiveDataPath, "eng");
    if (rc != 0) {
        /*
         * MSYS2/MinGW builds may need TESSDATA_PREFIX pointing at the
         * tessdata directory itself (not its parent), or the path with
         * a trailing separator. Try setting the env var and retrying.
         */
        char tessdataDir[MAX_PATH];
        snprintf(tessdataDir, MAX_PATH, "%s\\tessdata", effectiveDataPath);
        SetEnvironmentVariableA("TESSDATA_PREFIX", tessdataDir);
        Logger_Log("OCR: Init3 failed with '%s', retrying with TESSDATA_PREFIX=%s\n",
                   effectiveDataPath, tessdataDir);
        
        /* Must recreate the API object for a clean retry */
        engine->Delete(engine->api);
        engine->api = engine->Create();
        if (engine->api) {
            rc = engine->Init3(engine->api, effectiveDataPath, "eng");
        }
        if (rc != 0 && engine->api) {
            /* Last resort: pass tessdata dir directly as datapath */
            engine->Delete(engine->api);
            engine->api = engine->Create();
            if (engine->api) {
                rc = engine->Init3(engine->api, tessdataDir, "eng");
                if (rc == 0) {
                    Logger_Log("OCR: Init3 succeeded with tessdata dir as datapath\n");
                }
            }
        }
    }
    if (rc != 0) {
        Logger_Log("OCR: TessBaseAPIInit3 failed (rc=%d). Check tessdata path: %s\n",
                   rc, effectiveDataPath ? effectiveDataPath : "(null)");
        if (engine->api) engine->Delete(engine->api);
        FreeLibrary(engine->hTesseract);
        free(engine);
        return NULL;
    }

    /* Configure for speed: auto page segmentation, limit character set */
    if (engine->SetPageSegMode)
        engine->SetPageSegMode(engine->api, PSM_AUTO);
    
    if (engine->SetVariable) {
        /* Whitelist characters that appear in kill feeds */
        engine->SetVariable(engine->api, "tessedit_char_whitelist",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 #_-+.");
    }

    Logger_Log("OCR: Tesseract initialized (dataPath=%s)\n",
               effectiveDataPath ? effectiveDataPath : "(exe dir)");
    return engine;
}

/* ─── Pre-processing helpers ─── */

/* Compute Otsu threshold on a grayscale histogram */
static int ComputeOtsuThreshold(const BYTE* gray, int count)
{
    int hist[256] = {0};
    for (int i = 0; i < count; i++)
        hist[gray[i]]++;
    
    float sum = 0;
    for (int i = 0; i < 256; i++)
        sum += (float)i * hist[i];
    
    float sumB = 0;
    int wB = 0, wF = 0;
    float maxVar = 0;
    int threshold = 128;
    
    for (int t = 0; t < 256; t++) {
        wB += hist[t];
        if (wB == 0) continue;
        wF = count - wB;
        if (wF == 0) break;
        
        sumB += (float)t * hist[t];
        float mB = sumB / wB;
        float mF = (sum - sumB) / wF;
        float var = (float)wB * wF * (mB - mF) * (mB - mF);
        if (var > maxVar) {
            maxVar = var;
            threshold = t;
        }
    }
    return threshold;
}

/* ─── Stage scoring helpers (for showProcessing diagnostic) ─── */

/* Run Tesseract on a grayscale (1-channel) buffer and return allocated text.
 * Applies 2x nearest-neighbor upscale if height < 50px (same as main pipeline). */
static char* RunTesseractGray(OcrEngine* engine, const BYTE* gray, int w, int h, int grayStride)
{
    const BYTE* buf = gray;
    int useW = w, useH = h, useStride = grayStride;
    BYTE* scaled = NULL;

    if (h < 50) {
        int scale = 2;
        int newW = w * scale;
        int newH = h * scale;
        scaled = (BYTE*)malloc((size_t)newW * newH);
        if (scaled) {
            for (int y = 0; y < newH; y++) {
                const BYTE* srcRow = gray + (size_t)(y / scale) * grayStride;
                BYTE* dstRow = scaled + (size_t)y * newW;
                for (int x = 0; x < newW; x++)
                    dstRow[x] = srcRow[x / scale];
            }
            buf = scaled;
            useW = newW;
            useH = newH;
            useStride = newW;
        }
    }

    engine->SetImage(engine->api, buf, useW, useH, 1, useStride);
    char* tessText = engine->GetUTF8Text(engine->api);
    free(scaled);
    if (!tessText) return NULL;
    size_t len = strlen(tessText);
    char* result = (char*)malloc(len + 1);
    if (result) memcpy(result, tessText, len + 1);
    engine->DeleteText(tessText);
    return result;
}

/* Compute best-match similarity % of 'expected' within 'ocrOutput' (case-insensitive).
 * Uses sliding window: finds the alignment in ocrOutput that maximizes character
 * matches against expected. Returns best match as percentage of expected length. */
static int ComputeSimilarityPct(const char* ocrOutput, const char* expected)
{
    if (!ocrOutput || !expected) return 0;

    /* Strip trailing whitespace/newlines */
    size_t lenOcr = strlen(ocrOutput);
    size_t lenExp = strlen(expected);
    while (lenOcr > 0 && (ocrOutput[lenOcr-1] == '\n' || ocrOutput[lenOcr-1] == '\r' || ocrOutput[lenOcr-1] == ' '))
        lenOcr--;
    while (lenExp > 0 && (expected[lenExp-1] == '\n' || expected[lenExp-1] == '\r' || expected[lenExp-1] == ' '))
        lenExp--;

    if (lenExp == 0) return 100;
    if (lenOcr == 0) return 0;

    /* Slide expected-length window across OCR output, find best match */
    int bestMatches = 0;
    size_t windowEnd = (lenOcr >= lenExp) ? (lenOcr - lenExp) : 0;

    for (size_t i = 0; i <= windowEnd; i++) {
        int matches = 0;
        for (size_t j = 0; j < lenExp && (i + j) < lenOcr; j++) {
            if (toupper((unsigned char)ocrOutput[i + j]) == toupper((unsigned char)expected[j]))
                matches++;
        }
        if (matches > bestMatches) bestMatches = matches;
    }

    return (int)((bestMatches * 100) / lenExp);
}

char* OcrEngine_Recognize(OcrEngine* engine, const BYTE* bgraPixels,
                          int width, int height, int bytesPerPixel, int stride,
                          BOOL showProcessing, const char* expectedName)
{
    if (!engine || !engine->api || !bgraPixels || width <= 0 || height <= 0)
        return NULL;

    /* ─── Stage scoring (showProcessing diagnostic) ───
     * When showProcessing is active and expectedName is provided, we run
     * Tesseract at each stage and compute a similarity % against the
     * known player name. This shows which preprocessing steps help.
     */

    int pixelCount = width * height;
    int grayStride = width;

    /* Buffers to save each stage for scoring (only when showProcessing) */
    BYTE* stageGray = NULL;      /* after grayscale */
    BYTE* stageContrast = NULL;  /* after contrast stretch */
    BYTE* stageBinary = NULL;    /* after Otsu binarization */

    /* Stage 0: Show raw capture */
    if (showProcessing) {
        OcrPreview_Update(OCR_STAGE_RAW, bgraPixels, width, height, bytesPerPixel);
    }

    /* Stage 1: Convert BGRA to grayscale (luminance) */
    BYTE* grayBuf = (BYTE*)malloc((size_t)grayStride * height);
    if (!grayBuf) return NULL;

    for (int y = 0; y < height; y++) {
        const BYTE* src = bgraPixels + (size_t)y * stride;
        BYTE* dst = grayBuf + (size_t)y * grayStride;
        for (int x = 0; x < width; x++) {
            BYTE b = src[x * bytesPerPixel + 0];
            BYTE g = src[x * bytesPerPixel + 1];
            BYTE r = src[x * bytesPerPixel + 2];
            dst[x] = (BYTE)((r * 299 + g * 587 + b * 114) / 1000);
        }
    }

    if (showProcessing) {
        OcrPreview_Update(OCR_STAGE_GRAYSCALE, grayBuf, width, height, 1);
        /* Save grayscale stage for scoring */
        stageGray = (BYTE*)malloc((size_t)grayStride * height);
        if (stageGray) memcpy(stageGray, grayBuf, (size_t)grayStride * height);
    }

    /* Stage 2: Contrast stretch (min-max normalization) */
    BYTE minVal = 255, maxVal = 0;
    for (int i = 0; i < pixelCount; i++) {
        if (grayBuf[i] < minVal) minVal = grayBuf[i];
        if (grayBuf[i] > maxVal) maxVal = grayBuf[i];
    }
    if (maxVal > minVal) {
        int range = maxVal - minVal;
        for (int i = 0; i < pixelCount; i++) {
            grayBuf[i] = (BYTE)(((int)(grayBuf[i] - minVal) * 255) / range);
        }
    }

    if (showProcessing) {
        OcrPreview_Update(OCR_STAGE_CONTRAST, grayBuf, width, height, 1);
        /* Save contrast stage for scoring */
        stageContrast = (BYTE*)malloc((size_t)grayStride * height);
        if (stageContrast) memcpy(stageContrast, grayBuf, (size_t)grayStride * height);
    }

    /* Stage 3: Otsu binarization (invert: bright text becomes black on white) */
    int otsuThresh = ComputeOtsuThreshold(grayBuf, pixelCount);
    for (int i = 0; i < pixelCount; i++) {
        grayBuf[i] = (grayBuf[i] > otsuThresh) ? 0 : 255;
    }

    if (showProcessing) {
        OcrPreview_Update(OCR_STAGE_BINARY, grayBuf, width, height, 1);
        /* Save binary stage for scoring */
        stageBinary = (BYTE*)malloc((size_t)grayStride * height);
        if (stageBinary) memcpy(stageBinary, grayBuf, (size_t)grayStride * height);
    }

    /* Stage 4: Scale up small text (2x if height < 50px) */
    BYTE* ocrBuf = grayBuf;
    int ocrW = width;
    int ocrH = height;
    int ocrStride = grayStride;
    BYTE* scaledBuf = NULL;

    if (height < 50) {
        int scale = 2;
        int newW = width * scale;
        int newH = height * scale;
        scaledBuf = (BYTE*)malloc((size_t)newW * newH);
        if (scaledBuf) {
            /* Nearest-neighbor upscale */
            for (int y = 0; y < newH; y++) {
                int srcY = y / scale;
                const BYTE* srcRow = grayBuf + (size_t)srcY * grayStride;
                BYTE* dstRow = scaledBuf + (size_t)y * newW;
                for (int x = 0; x < newW; x++) {
                    dstRow[x] = srcRow[x / scale];
                }
            }
            ocrBuf = scaledBuf;
            ocrW = newW;
            ocrH = newH;
            ocrStride = newW;

            if (showProcessing) {
                OcrPreview_Update(OCR_STAGE_SCALED, scaledBuf, newW, newH, 1);
            }
        }
    }

    /* Run Tesseract on the processed buffer */
    engine->SetImage(engine->api, ocrBuf, ocrW, ocrH, 1, ocrStride);
    
    char* tessText = engine->GetUTF8Text(engine->api);

    /* ─── Stage scoring: run Tesseract on each saved stage and compare ─── */
    if (showProcessing && tessText && expectedName && expectedName[0]) {
        /* Score grayscale stage */
        if (stageGray) {
            char* t = RunTesseractGray(engine, stageGray, width, height, grayStride);
            int pct = ComputeSimilarityPct(t, expectedName);
            DebugConsole_Print("STAGE SCORE: Grayscale = %3d%% | '%s'\n", pct, t ? t : "");
            free(t);
        }
        /* Score contrast stage */
        if (stageContrast) {
            char* t = RunTesseractGray(engine, stageContrast, width, height, grayStride);
            int pct = ComputeSimilarityPct(t, expectedName);
            DebugConsole_Print("STAGE SCORE: Contrast  = %3d%% | '%s'\n", pct, t ? t : "");
            free(t);
        }
        /* Score binary stage (no scale) */
        if (stageBinary) {
            char* t = RunTesseractGray(engine, stageBinary, width, height, grayStride);
            int pct = ComputeSimilarityPct(t, expectedName);
            DebugConsole_Print("STAGE SCORE: Binary    = %3d%% | '%s'\n", pct, t ? t : "");
            free(t);
        }
        /* Final (binary+scaled or binary if no scale) */
        int finalPct = ComputeSimilarityPct(tessText, expectedName);
        DebugConsole_Print("STAGE SCORE: Final     = %3d%% | '%s'\n", finalPct, tessText);
        DebugConsole_Print("  (vs expected: '%s')\n", expectedName);
        DebugConsole_Print("=========================================\n");
    }

    free(stageGray);
    free(stageContrast);
    free(stageBinary);
    free(grayBuf);
    free(scaledBuf);

    /* Pump messages for preview windows (they live on this thread).
     * Must pump even when showProcessing is OFF to process pending WM_CLOSE. */
    OcrPreview_PumpMessages();

    if (!tessText) return NULL;

    /* Copy to a standard malloc'd buffer so caller can use free() */
    size_t len = strlen(tessText);
    char* result = (char*)malloc(len + 1);
    if (result) {
        memcpy(result, tessText, len + 1);
    }

    engine->DeleteText(tessText);
    return result;
}

void OcrEngine_Shutdown(OcrEngine* engine)
{
    if (!engine) return;
    
    if (engine->api && engine->Delete) {
        engine->Delete(engine->api);
    }
    if (engine->hTesseract) {
        FreeLibrary(engine->hTesseract);
    }
    free(engine);
}

BOOL OcrEngine_IsReady(const OcrEngine* engine)
{
    return engine && engine->api != NULL;
}

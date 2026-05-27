/*
 * OCR Preview - Debug windows showing OCR pre-processing stages
 *
 * Creates small Win32 popup windows that display the pixel buffer
 * at each pre-processing stage. Windows are arranged horizontally
 * and update every OCR scan cycle when enabled.
 *
 * Rendering uses SetDIBitsToDevice with a BITMAPINFO header to
 * blit pixel data directly — no GDI+ or extra textures needed.
 */

#include "ocr_preview.h"
#include <stdio.h>
#include <string.h>

/* Preview window class name */
#define OCR_PREVIEW_CLASS "LWSROcrPreview"

/* Maximum display size for preview windows */
#define PREVIEW_MAX_W  400
#define PREVIEW_MAX_H  300

/* Stage names for window titles */
static const char* STAGE_NAMES[OCR_STAGE_COUNT] = {
    "1: Raw Capture",
    "2: Grayscale",
    "3: Contrast Stretch",
    "4: Binarize (Otsu)",
    "5: Scaled Up"
};

/* Per-stage state */
typedef struct {
    HWND hwnd;
    BYTE* dibBits;       /* 32-bit BGRA DIB for display */
    int srcW, srcH;      /* Source dimensions */
    int dispW, dispH;    /* Display dimensions (scaled to fit) */
    BITMAPINFO bmi;
} StageWindow;

static StageWindow s_stages[OCR_STAGE_COUNT];
static BOOL s_registered = FALSE;

/* ─── Window procedure ─── */

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            /* Find which stage this window belongs to */
            for (int i = 0; i < OCR_STAGE_COUNT; i++) {
                if (s_stages[i].hwnd == hwnd && s_stages[i].dibBits) {
                    /* Blit the DIB to the window */
                    StretchDIBits(hdc,
                        0, 0, s_stages[i].dispW, s_stages[i].dispH,
                        0, 0, s_stages[i].srcW, s_stages[i].srcH,
                        s_stages[i].dibBits, &s_stages[i].bmi,
                        DIB_RGB_COLORS, SRCCOPY);
                    break;
                }
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_CLOSE:
            /* Don't destroy — just hide. User can re-enable via checkbox. */
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ─── Public API ─── */

BOOL OcrPreview_Register(HINSTANCE hInstance)
{
    if (s_registered) return TRUE;
    
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = OCR_PREVIEW_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    if (!RegisterClassA(&wc)) return FALSE;
    s_registered = TRUE;
    return TRUE;
}

void OcrPreview_Update(OcrPreviewStage stage, const BYTE* pixels,
                       int width, int height, int bytesPerPixel)
{
    if (stage < 0 || stage >= OCR_STAGE_COUNT) return;
    if (!pixels || width <= 0 || height <= 0) return;
    
    StageWindow* sw = &s_stages[stage];
    
    /* Calculate display size preserving aspect ratio */
    int dispW = width;
    int dispH = height;
    if (dispW > PREVIEW_MAX_W) {
        dispH = dispH * PREVIEW_MAX_W / dispW;
        dispW = PREVIEW_MAX_W;
    }
    if (dispH > PREVIEW_MAX_H) {
        dispW = dispW * PREVIEW_MAX_H / dispH;
        dispH = PREVIEW_MAX_H;
    }
    if (dispW < 1) dispW = 1;
    if (dispH < 1) dispH = 1;
    
    /* Create window if needed */
    if (!sw->hwnd || !IsWindow(sw->hwnd)) {
        /* Position windows side by side, offset from top-left */
        int xPos = 10 + stage * (PREVIEW_MAX_W + 10);
        int yPos = 10;
        
        /* Account for window chrome */
        RECT wr = {0, 0, dispW, dispH};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        
        char title[64];
        snprintf(title, sizeof(title), "OCR: %s", STAGE_NAMES[stage]);
        
        sw->hwnd = CreateWindowA(
            OCR_PREVIEW_CLASS, title,
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            xPos, yPos,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, GetModuleHandle(NULL), NULL);
    }
    
    /* Resize if source dimensions changed */
    if (sw->srcW != width || sw->srcH != height || sw->dispW != dispW || sw->dispH != dispH) {
        sw->srcW = width;
        sw->srcH = height;
        sw->dispW = dispW;
        sw->dispH = dispH;
        
        /* Resize the window */
        RECT wr = {0, 0, dispW, dispH};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(sw->hwnd, NULL, 0, 0,
                     wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        
        /* Reallocate DIB buffer */
        free(sw->dibBits);
        sw->dibBits = (BYTE*)malloc((size_t)width * height * 4);
    }
    
    if (!sw->dibBits) return;
    
    /* Convert source pixels to 32-bit BGRA for DIB display */
    if (bytesPerPixel == 4) {
        /* Already BGRA — direct copy */
        for (int y = 0; y < height; y++) {
            memcpy(sw->dibBits + (size_t)y * width * 4,
                   pixels + (size_t)(height - 1 - y) * width * 4,
                   (size_t)width * 4);
        }
    } else if (bytesPerPixel == 1) {
        /* Grayscale → BGRA (bottom-up for DIB) */
        for (int y = 0; y < height; y++) {
            const BYTE* src = pixels + (size_t)(height - 1 - y) * width;
            BYTE* dst = sw->dibBits + (size_t)y * width * 4;
            for (int x = 0; x < width; x++) {
                BYTE v = src[x];
                dst[x * 4 + 0] = v;  /* B */
                dst[x * 4 + 1] = v;  /* G */
                dst[x * 4 + 2] = v;  /* R */
                dst[x * 4 + 3] = 255; /* A */
            }
        }
    }
    
    /* Set up BITMAPINFO for StretchDIBits */
    memset(&sw->bmi, 0, sizeof(sw->bmi));
    sw->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    sw->bmi.bmiHeader.biWidth = width;
    sw->bmi.bmiHeader.biHeight = height;  /* positive = bottom-up */
    sw->bmi.bmiHeader.biPlanes = 1;
    sw->bmi.bmiHeader.biBitCount = 32;
    sw->bmi.bmiHeader.biCompression = BI_RGB;
    
    /* Trigger repaint */
    InvalidateRect(sw->hwnd, NULL, FALSE);
    
    /* Ensure window is visible */
    if (!IsWindowVisible(sw->hwnd)) {
        ShowWindow(sw->hwnd, SW_SHOWNOACTIVATE);
    }
}

void OcrPreview_CloseAll(void)
{
    for (int i = 0; i < OCR_STAGE_COUNT; i++) {
        if (s_stages[i].hwnd && IsWindow(s_stages[i].hwnd)) {
            /* Post WM_CLOSE instead of calling ShowWindow cross-thread.
             * ShowWindow does a synchronous SendMessage to the owning thread,
             * which deadlocks if that thread is busy (e.g., in OCR).
             * PostMessage is async — the buffer thread will process it
             * on its next PumpMessages call and hide the window. */
            PostMessage(s_stages[i].hwnd, WM_CLOSE, 0, 0);
        }
    }
}

void OcrPreview_DestroyAll(void)
{
    for (int i = 0; i < OCR_STAGE_COUNT; i++) {
        if (s_stages[i].hwnd && IsWindow(s_stages[i].hwnd)) {
            DestroyWindow(s_stages[i].hwnd);
        }
        s_stages[i].hwnd = NULL;
        free(s_stages[i].dibBits);
        s_stages[i].dibBits = NULL;
        s_stages[i].srcW = 0;
        s_stages[i].srcH = 0;
    }
}

void OcrPreview_PumpMessages(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

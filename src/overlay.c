/*
 * Overlay Implementation
 * Selection UI, recording controls, and main logic
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include <stdio.h>

#include "action_toolbar.h"
#include "border.h"

// GDI+ Flat API for anti-aliased drawing
#include <objbase.h>

// GDI+ types and functions (flat API)
typedef struct GdiplusStartupInput {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef void* GpGraphics;
typedef void* GpBrush;
typedef void* GpSolidFill;
typedef void* GpPen;
typedef int GpStatus;

// GDI+ function pointers
typedef GpStatus (WINAPI *GdiplusStartupFunc)(ULONG_PTR*, const GdiplusStartupInput*, void*);
typedef void (WINAPI *GdiplusShutdownFunc)(ULONG_PTR);
typedef GpStatus (WINAPI *GdipCreateFromHDCFunc)(HDC, GpGraphics**);
typedef GpStatus (WINAPI *GdipDeleteGraphicsFunc)(GpGraphics*);
typedef GpStatus (WINAPI *GdipSetSmoothingModeFunc)(GpGraphics*, int);
typedef GpStatus (WINAPI *GdipCreateSolidFillFunc)(DWORD, GpSolidFill**);
typedef GpStatus (WINAPI *GdipDeleteBrushFunc)(GpBrush*);
typedef GpStatus (WINAPI *GdipCreatePenFunc)(DWORD, float, int, GpPen**);
typedef GpStatus (WINAPI *GdipDeletePenFunc)(GpPen*);
typedef GpStatus (WINAPI *GdipFillRectangleFunc)(GpGraphics*, GpBrush*, float, float, float, float);
typedef GpStatus (WINAPI *GdipFillEllipseFunc)(GpGraphics*, GpBrush*, float, float, float, float);
typedef GpStatus (WINAPI *GdipDrawRectangleFunc)(GpGraphics*, GpPen*, float, float, float, float);
typedef GpStatus (WINAPI *GdipFillPathFunc)(GpGraphics*, GpBrush*, void*);
typedef GpStatus (WINAPI *GdipDrawPathFunc)(GpGraphics*, GpPen*, void*);
typedef GpStatus (WINAPI *GdipCreatePathFunc)(int, void**);
typedef GpStatus (WINAPI *GdipDeletePathFunc)(void*);
typedef GpStatus (WINAPI *GdipAddPathArcFunc)(void*, float, float, float, float, float, float);
typedef GpStatus (WINAPI *GdipAddPathLineFunc)(void*, float, float, float, float);
typedef GpStatus (WINAPI *GdipClosePathFigureFunc)(void*);
typedef GpStatus (WINAPI *GdipStartPathFigureFunc)(void*);

static HMODULE g_gdiplus = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static GdipCreateFromHDCFunc GdipCreateFromHDC = NULL;
static GdipDeleteGraphicsFunc GdipDeleteGraphics = NULL;
static GdipSetSmoothingModeFunc GdipSetSmoothingMode = NULL;
static GdipCreateSolidFillFunc GdipCreateSolidFill = NULL;
static GdipDeleteBrushFunc GdipDeleteBrush = NULL;
static GdipCreatePenFunc GdipCreatePen1 = NULL;
static GdipDeletePenFunc GdipDeletePen = NULL;
static GdipFillRectangleFunc GdipFillRectangle = NULL;
static GdipFillEllipseFunc GdipFillEllipse = NULL;
static GdipFillPathFunc GdipFillPath = NULL;
static GdipDrawPathFunc GdipDrawPath = NULL;
static GdipCreatePathFunc GdipCreatePath = NULL;
static GdipDeletePathFunc GdipDeletePath = NULL;
static GdipAddPathArcFunc GdipAddPathArc = NULL;
static GdipAddPathLineFunc GdipAddPathLine = NULL;
static GdipClosePathFigureFunc GdipClosePathFigure = NULL;
static GdipStartPathFigureFunc GdipStartPathFigure = NULL;

// Smoothing mode constants
#define SmoothingModeAntiAlias 4
#define UnitPixel 2
#define FillModeAlternate 0

// DWM window corner preference (Windows 11+)
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
typedef enum {
    DWMWCP_DEFAULT = 0,
    DWMWCP_DONOTROUND = 1,
    DWMWCP_ROUND = 2,
    DWMWCP_ROUNDSMALL = 3
} DWM_WINDOW_CORNER_PREFERENCE;

// OCR_NORMAL not defined in some Windows headers
#ifndef OCR_NORMAL
#define OCR_NORMAL 32512
#endif

#include "overlay.h"
#include "capture.h"
#include "encoder.h"
#include "config.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

// External globals from main.c
extern AppConfig g_config;
extern CaptureState g_capture;
extern BOOL g_isRecording;
extern BOOL g_isSelecting;
extern HWND g_overlayWnd;
extern HWND g_controlWnd;

// Control IDs
#define ID_MODE_AREA       1001
#define ID_MODE_WINDOW     1002
#define ID_MODE_MONITOR    1003
#define ID_MODE_ALL        1004
#define ID_BTN_CLOSE       1005
#define ID_BTN_STOP        1006
#define ID_CHK_MOUSE       1007
#define ID_CHK_BORDER      1008
#define ID_CMB_FORMAT      1009
#define ID_CMB_QUALITY     1010
#define ID_EDT_PATH        1011
#define ID_BTN_BROWSE      1012
#define ID_BTN_SETTINGS    1013
#define ID_EDT_TIMELIMIT   1014
#define ID_BTN_RECORD      1015
#define ID_CMB_HOURS       1016
#define ID_CMB_MINUTES     1017
#define ID_CMB_SECONDS     1018
#define ID_TIMER_RECORD    2001
#define ID_TIMER_LIMIT     2002
#define ID_TIMER_DISPLAY   2003

// Action toolbar button IDs
#define ID_ACTION_RECORD   3001
#define ID_ACTION_COPY     3002
#define ID_ACTION_SAVE     3003
#define ID_ACTION_MARKUP   3004

// Selection states
typedef enum {
    SEL_NONE = 0,
    SEL_DRAWING,      // Currently drawing selection
    SEL_COMPLETE,     // Selection complete, showing handles
    SEL_MOVING,       // Moving the entire selection
    SEL_RESIZING      // Resizing via handle
} SelectionState;

// Resize handle positions
typedef enum {
    HANDLE_NONE = 0,
    HANDLE_TL, HANDLE_T, HANDLE_TR,
    HANDLE_L,           HANDLE_R,
    HANDLE_BL, HANDLE_B, HANDLE_BR
} HandlePosition;

// Window state
static HINSTANCE g_hInstance;
static CaptureMode g_currentMode = MODE_NONE;
static SelectionState g_selState = SEL_NONE;
static HandlePosition g_activeHandle = HANDLE_NONE;
static BOOL g_isDragging = FALSE;
static POINT g_dragStart;
static POINT g_dragEnd;
static POINT g_moveStart;      // For moving selection
static RECT g_selectedRect;
static RECT g_originalRect;    // Original rect before resize/move
static HWND g_settingsWnd = NULL;
static HWND g_crosshairWnd = NULL;
static HWND g_timerWnd = NULL;     // Recording timer display
static EncoderState g_encoder;
static HANDLE g_recordThread = NULL;
static volatile BOOL g_stopRecording = FALSE;
static DWORD g_recordStartTime = 0;

// Handle size
#define HANDLE_SIZE 10

// Recording thread
static DWORD WINAPI RecordingThread(LPVOID param);

// Window procedures
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK TimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper functions
static HandlePosition HitTestHandle(POINT pt);
static void UpdateActionToolbar(void);
static void UpdateTimerDisplay(void);
static void ShowActionToolbar(BOOL show);
static void CaptureToClipboard(void);
static void CaptureToFile(void);

// Initialize GDI+ for anti-aliased drawing
static BOOL InitGdiPlus(void) {
    g_gdiplus = LoadLibraryW(L"gdiplus.dll");
    if (!g_gdiplus) return FALSE;
    
    GdiplusStartupFunc GdiplusStartup = (GdiplusStartupFunc)GetProcAddress(g_gdiplus, "GdiplusStartup");
    if (!GdiplusStartup) return FALSE;
    
    GdiplusStartupInput input = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&g_gdiplusToken, &input, NULL) != 0) return FALSE;
    
    // Load all needed functions
    GdipCreateFromHDC = (GdipCreateFromHDCFunc)GetProcAddress(g_gdiplus, "GdipCreateFromHDC");
    GdipDeleteGraphics = (GdipDeleteGraphicsFunc)GetProcAddress(g_gdiplus, "GdipDeleteGraphics");
    GdipSetSmoothingMode = (GdipSetSmoothingModeFunc)GetProcAddress(g_gdiplus, "GdipSetSmoothingMode");
    GdipCreateSolidFill = (GdipCreateSolidFillFunc)GetProcAddress(g_gdiplus, "GdipCreateSolidFill");
    GdipDeleteBrush = (GdipDeleteBrushFunc)GetProcAddress(g_gdiplus, "GdipDeleteBrush");
    GdipCreatePen1 = (GdipCreatePenFunc)GetProcAddress(g_gdiplus, "GdipCreatePen1");
    GdipDeletePen = (GdipDeletePenFunc)GetProcAddress(g_gdiplus, "GdipDeletePen");
    GdipFillRectangle = (GdipFillRectangleFunc)GetProcAddress(g_gdiplus, "GdipFillRectangle");
    GdipFillEllipse = (GdipFillEllipseFunc)GetProcAddress(g_gdiplus, "GdipFillEllipse");
    GdipFillPath = (GdipFillPathFunc)GetProcAddress(g_gdiplus, "GdipFillPath");
    GdipDrawPath = (GdipDrawPathFunc)GetProcAddress(g_gdiplus, "GdipDrawPath");
    GdipCreatePath = (GdipCreatePathFunc)GetProcAddress(g_gdiplus, "GdipCreatePath");
    GdipDeletePath = (GdipDeletePathFunc)GetProcAddress(g_gdiplus, "GdipDeletePath");
    GdipAddPathArc = (GdipAddPathArcFunc)GetProcAddress(g_gdiplus, "GdipAddPathArc");
    GdipAddPathLine = (GdipAddPathLineFunc)GetProcAddress(g_gdiplus, "GdipAddPathLine");
    GdipClosePathFigure = (GdipClosePathFigureFunc)GetProcAddress(g_gdiplus, "GdipClosePathFigure");
    GdipStartPathFigure = (GdipStartPathFigureFunc)GetProcAddress(g_gdiplus, "GdipStartPathFigure");
    
    return TRUE;
}

static void ShutdownGdiPlus(void) {
    if (g_gdiplusToken && g_gdiplus) {
        GdiplusShutdownFunc GdiplusShutdown = (GdiplusShutdownFunc)GetProcAddress(g_gdiplus, "GdiplusShutdown");
        if (GdiplusShutdown) GdiplusShutdown(g_gdiplusToken);
    }
    if (g_gdiplus) FreeLibrary(g_gdiplus);
    g_gdiplus = NULL;
    g_gdiplusToken = 0;
}

// Convert COLORREF to ARGB for GDI+
static DWORD ColorRefToARGB(COLORREF cr, BYTE alpha) {
    return ((DWORD)alpha << 24) | 
           ((DWORD)GetRValue(cr) << 16) | 
           ((DWORD)GetGValue(cr) << 8) | 
           (DWORD)GetBValue(cr);
}

// Draw anti-aliased filled rounded rectangle
static void DrawRoundedRectAA(HDC hdc, RECT* rect, int radius, COLORREF fillColor, COLORREF borderColor) {
    if (!GdipCreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (GdipCreateFromHDC(hdc, &graphics) != 0) return;
    
    GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    // Inset by 0.5 to ensure border is fully visible (GDI+ draws strokes centered on path)
    float x = (float)rect->left + 0.5f;
    float y = (float)rect->top + 0.5f;
    float w = (float)(rect->right - rect->left) - 1.0f;
    float h = (float)(rect->bottom - rect->top) - 1.0f;
    float r = (float)radius;
    float d = r * 2.0f;
    
    // Create rounded rectangle path
    void* path = NULL;
    GdipCreatePath(FillModeAlternate, &path);
    
    // Top-left arc
    GdipAddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    // Top-right arc
    GdipAddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    // Bottom-right arc
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    // Bottom-left arc
    GdipAddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(path);
    
    // Fill
    GpSolidFill* brush = NULL;
    GdipCreateSolidFill(ColorRefToARGB(fillColor, 255), &brush);
    GdipFillPath(graphics, brush, path);
    GdipDeleteBrush(brush);
    
    // Border
    GpPen* pen = NULL;
    GdipCreatePen1(ColorRefToARGB(borderColor, 255), 1.0f, UnitPixel, &pen);
    GdipDrawPath(graphics, pen, path);
    GdipDeletePen(pen);
    
    GdipDeletePath(path);
    GdipDeleteGraphics(graphics);
}

// Draw anti-aliased filled circle
static void DrawCircleAA(HDC hdc, int cx, int cy, int radius, COLORREF color) {
    if (!GdipCreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (GdipCreateFromHDC(hdc, &graphics) != 0) return;
    
    GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    GpSolidFill* brush = NULL;
    GdipCreateSolidFill(ColorRefToARGB(color, 255), &brush);
    GdipFillEllipse(graphics, brush, 
                    (float)(cx - radius), (float)(cy - radius), 
                    (float)(radius * 2), (float)(radius * 2));
    GdipDeleteBrush(brush);
    GdipDeleteGraphics(graphics);
}

// Apply smooth rounded corners using DWM (Windows 11+)
static void ApplyRoundedCorners(HWND hwnd) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

// Helper to get primary monitor center position
static void GetPrimaryMonitorCenter(POINT* pt) {
    HMONITOR hMon = MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    pt->x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    pt->y = mi.rcMonitor.top + 80; // Near top
}

// Draw dotted selection rectangle on a DC
static void DrawSelectionBorder(HDC hdc, RECT* rect) {
    // White dotted line
    HPEN whitePen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, whitePen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    
    Rectangle(hdc, rect->left, rect->top, rect->right, rect->bottom);
    
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(whitePen);
}

// Update layered window with dark overlay and clear selection hole
static void UpdateOverlayBitmap(void) {
    if (!g_overlayWnd) return;
    
    RECT wndRect;
    GetWindowRect(g_overlayWnd, &wndRect);
    int width = wndRect.right - wndRect.left;
    int height = wndRect.bottom - wndRect.top;
    
    // Create 32-bit DIB for per-pixel alpha
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    BYTE* pBits = NULL;
    HBITMAP hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    // Fill entire overlay with semi-transparent dark (alpha ~100 out of 255)
    int overlayAlpha = 100;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            BYTE* pixel = pBits + (y * width + x) * 4;
            pixel[0] = 0;   // B
            pixel[1] = 0;   // G  
            pixel[2] = 0;   // R
            pixel[3] = overlayAlpha; // A
        }
    }
    
    // If we have a selection (drawing or complete), punch a clear hole
    BOOL hasSelection = !IsRectEmpty(&g_selectedRect) && 
                        (g_selState == SEL_DRAWING || g_selState == SEL_COMPLETE || 
                         g_selState == SEL_MOVING || g_selState == SEL_RESIZING);
    
    if (hasSelection) {
        // Convert screen coords to window coords
        int selLeft = g_selectedRect.left - wndRect.left;
        int selTop = g_selectedRect.top - wndRect.top;
        int selRight = g_selectedRect.right - wndRect.left;
        int selBottom = g_selectedRect.bottom - wndRect.top;
        
        // Clamp to window bounds
        if (selLeft < 0) selLeft = 0;
        if (selTop < 0) selTop = 0;
        if (selRight > width) selRight = width;
        if (selBottom > height) selBottom = height;
        
        // Clear the selection area (fully transparent)
        for (int y = selTop; y < selBottom; y++) {
            for (int x = selLeft; x < selRight; x++) {
                BYTE* pixel = pBits + (y * width + x) * 4;
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 0; // Fully transparent
            }
        }
        
        // Draw white dotted border around selection
        RECT borderRect = { selLeft, selTop, selRight, selBottom };
        DrawSelectionBorder(memDC, &borderRect);
        
        // Draw resize handles when selection is complete
        if (g_selState == SEL_COMPLETE || g_selState == SEL_MOVING || g_selState == SEL_RESIZING) {
            HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, whiteBrush);
            HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
            HPEN oldPen = (HPEN)SelectObject(memDC, whitePen);
            
            int cx = (selLeft + selRight) / 2;
            int cy = (selTop + selBottom) / 2;
            int hs = HANDLE_SIZE / 2;
            
            // Corner handles
            Ellipse(memDC, selLeft - hs, selTop - hs, selLeft + hs, selTop + hs);           // TL
            Ellipse(memDC, selRight - hs, selTop - hs, selRight + hs, selTop + hs);         // TR
            Ellipse(memDC, selLeft - hs, selBottom - hs, selLeft + hs, selBottom + hs);     // BL
            Ellipse(memDC, selRight - hs, selBottom - hs, selRight + hs, selBottom + hs);   // BR
            
            // Edge handles
            Ellipse(memDC, cx - hs, selTop - hs, cx + hs, selTop + hs);                     // T
            Ellipse(memDC, cx - hs, selBottom - hs, cx + hs, selBottom + hs);               // B
            Ellipse(memDC, selLeft - hs, cy - hs, selLeft + hs, cy + hs);                   // L
            Ellipse(memDC, selRight - hs, cy - hs, selRight + hs, cy + hs);                 // R
            
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(whiteBrush);
            DeleteObject(whitePen);
        }
    }
    
    // Apply to layered window
    POINT ptSrc = {0, 0};
    POINT ptDst = { wndRect.left, wndRect.top };
    SIZE sizeWnd = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    
    UpdateLayeredWindow(g_overlayWnd, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
    SelectObject(memDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
}

// Update crosshair position - positions the size indicator near cursor
static void UpdateCrosshair(int x, int y) {
    if (!g_crosshairWnd) return;
    if (!IsWindowVisible(g_crosshairWnd)) return;
    
    // Get screen bounds to determine corner placement
    RECT screenRect;
    Capture_GetAllMonitorsBounds(&screenRect);
    
    int crossSize = 80;
    int offset = 20;
    int posX, posY;
    
    // Position near cursor but offset so it doesn't obscure selection
    if (x > (screenRect.right - 150)) {
        posX = x - crossSize - offset;
    } else {
        posX = x + offset;
    }
    
    if (y > (screenRect.bottom - 150)) {
        posY = y - crossSize - offset;
    } else {
        posY = y + offset;
    }
    
    SetWindowPos(g_crosshairWnd, HWND_TOPMOST, posX, posY, 
                 crossSize, crossSize, SWP_NOACTIVATE);
    InvalidateRect(g_crosshairWnd, NULL, FALSE);
}

// Hit test for resize handles - returns which handle is under the point
static HandlePosition HitTestHandle(POINT pt) {
    if (IsRectEmpty(&g_selectedRect)) return HANDLE_NONE;
    
    int hs = HANDLE_SIZE;
    int cx = (g_selectedRect.left + g_selectedRect.right) / 2;
    int cy = (g_selectedRect.top + g_selectedRect.bottom) / 2;
    
    // Check corner handles first (higher priority)
    RECT handleRect;
    
    // Top-left
    SetRect(&handleRect, g_selectedRect.left - hs, g_selectedRect.top - hs, 
            g_selectedRect.left + hs, g_selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_TL;
    
    // Top-right
    SetRect(&handleRect, g_selectedRect.right - hs, g_selectedRect.top - hs,
            g_selectedRect.right + hs, g_selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_TR;
    
    // Bottom-left
    SetRect(&handleRect, g_selectedRect.left - hs, g_selectedRect.bottom - hs,
            g_selectedRect.left + hs, g_selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_BL;
    
    // Bottom-right
    SetRect(&handleRect, g_selectedRect.right - hs, g_selectedRect.bottom - hs,
            g_selectedRect.right + hs, g_selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_BR;
    
    // Edge handles
    // Top
    SetRect(&handleRect, cx - hs, g_selectedRect.top - hs, cx + hs, g_selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_T;
    
    // Bottom
    SetRect(&handleRect, cx - hs, g_selectedRect.bottom - hs, cx + hs, g_selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_B;
    
    // Left
    SetRect(&handleRect, g_selectedRect.left - hs, cy - hs, g_selectedRect.left + hs, cy + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_L;
    
    // Right
    SetRect(&handleRect, g_selectedRect.right - hs, cy - hs, g_selectedRect.right + hs, cy + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_R;
    
    return HANDLE_NONE;
}

// Check if point is inside the selection (for moving)
static BOOL PtInSelection(POINT pt) {
    return PtInRect(&g_selectedRect, pt);
}

// Check if point is on the selection border (for moving with border hover)
static BOOL PtOnSelectionBorder(POINT pt) {
    if (IsRectEmpty(&g_selectedRect)) return FALSE;
    
    int borderWidth = 8; // Width of the border hit zone
    
    // Create outer and inner rects
    RECT outer = g_selectedRect;
    InflateRect(&outer, borderWidth, borderWidth);
    
    RECT inner = g_selectedRect;
    InflateRect(&inner, -borderWidth, -borderWidth);
    
    // Point must be in outer but not in inner
    return PtInRect(&outer, pt) && !PtInRect(&inner, pt);
}

// Get cursor for current handle
static HCURSOR GetHandleCursor(HandlePosition handle) {
    switch (handle) {
        case HANDLE_TL: case HANDLE_BR: return LoadCursor(NULL, IDC_SIZENWSE);
        case HANDLE_TR: case HANDLE_BL: return LoadCursor(NULL, IDC_SIZENESW);
        case HANDLE_T: case HANDLE_B: return LoadCursor(NULL, IDC_SIZENS);
        case HANDLE_L: case HANDLE_R: return LoadCursor(NULL, IDC_SIZEWE);
        default: return LoadCursor(NULL, IDC_ARROW);
    }
}

// Show/hide the action toolbar (uses new action_toolbar module)
static void ShowActionToolbar(BOOL show) {
    if (show && !IsRectEmpty(&g_selectedRect)) {
        int cx = (g_selectedRect.left + g_selectedRect.right) / 2;
        int posY = g_selectedRect.bottom + 10;
        
        // Check if it would go off screen
        RECT screenRect;
        Capture_GetAllMonitorsBounds(&screenRect);
        if (posY + 40 > screenRect.bottom - 20) {
            posY = g_selectedRect.top - 40 - 10;
        }
        
        ActionToolbar_Show(cx, posY);
    } else {
        ActionToolbar_Hide();
    }
}

// Update the action toolbar position
static void UpdateActionToolbar(void) {
    ShowActionToolbar(g_selState == SEL_COMPLETE);
}

// Capture screen region to clipboard
static void CaptureToClipboard(void) {
    if (IsRectEmpty(&g_selectedRect)) return;
    
    int w = g_selectedRect.right - g_selectedRect.left;
    int h = g_selectedRect.bottom - g_selectedRect.top;
    
    // Hide overlay temporarily
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    Sleep(50); // Let windows redraw
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    BitBlt(memDC, 0, 0, w, h, screenDC, g_selectedRect.left, g_selectedRect.top, SRCCOPY);
    
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    
    // Copy to clipboard
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hBitmap);
        CloseClipboard();
    } else {
        DeleteObject(hBitmap);
    }
    
    // Clear selection state - overlay stays hidden, show control panel
    g_selState = SEL_NONE;
    SetRectEmpty(&g_selectedRect);
    g_isSelecting = FALSE;
    ShowWindow(g_controlWnd, SW_SHOW);
}

// Capture screen region to file (Save As dialog)
static void CaptureToFile(void) {
    if (IsRectEmpty(&g_selectedRect)) return;
    
    int w = g_selectedRect.right - g_selectedRect.left;
    int h = g_selectedRect.bottom - g_selectedRect.top;
    
    // Hide overlay temporarily
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    Sleep(50);
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    BitBlt(memDC, 0, 0, w, h, screenDC, g_selectedRect.left, g_selectedRect.top, SRCCOPY);
    
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    
    // Show Save As dialog
    char filename[MAX_PATH] = "capture.png";
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "PNG Image\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "png";
    
    if (GetSaveFileNameA(&ofn)) {
        // TODO: Save as PNG (requires GDI+ or other library)
        // For now, just show success message
        MessageBoxA(NULL, "Save functionality requires PNG encoder.\nBitmap captured to clipboard instead.", 
                    "Save", MB_OK | MB_ICONINFORMATION);
        
        // Copy to clipboard as fallback
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            SetClipboardData(CF_BITMAP, hBitmap);
            CloseClipboard();
        }
    }
    
    DeleteObject(hBitmap);
    
    // Clear selection state - overlay stays hidden, show control panel
    g_selState = SEL_NONE;
    SetRectEmpty(&g_selectedRect);
    g_isSelecting = FALSE;
    ShowWindow(g_controlWnd, SW_SHOW);
}

// Update timer display position and text
static void UpdateTimerDisplay(void) {
    if (!g_timerWnd || !g_isRecording) return;
    
    // Update timer text
    DWORD elapsed = GetTickCount() - g_recordStartTime;
    int secs = (elapsed / 1000) % 60;
    int mins = (elapsed / 60000) % 60;
    int hours = elapsed / 3600000;
    
    char timeText[32];
    if (hours > 0) {
        sprintf(timeText, "%d:%02d:%02d", hours, mins, secs);
    } else {
        sprintf(timeText, "%02d:%02d", mins, secs);  // MM:SS with leading zero
    }
    SetWindowTextA(g_timerWnd, timeText);
    
    // Position in bottom-right corner of selection
    int timerW = 175;
    int timerH = 28;
    RECT screenRect;
    Capture_GetAllMonitorsBounds(&screenRect);
    
    // Position inside the bottom-right of selection
    int posX = g_selectedRect.right - timerW - 8;
    int posY = g_selectedRect.bottom - timerH - 8;
    
    // If too close to edge, move to opposite corner
    if (posX < screenRect.left + 20) posX = g_selectedRect.left + 8;
    if (posY < screenRect.top + 20) posY = g_selectedRect.top + 8;
    
    SetWindowPos(g_timerWnd, HWND_TOPMOST, posX, posY, timerW, timerH, SWP_NOACTIVATE);
    InvalidateRect(g_timerWnd, NULL, TRUE);
}

BOOL Overlay_Create(HINSTANCE hInstance) {
    g_hInstance = hInstance;
    
    // Initialize GDI+ for anti-aliased drawing
    InitGdiPlus();
    
    // Initialize common controls
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);
    
    // Register overlay window class
    WNDCLASSEXA wcOverlay = {0};
    wcOverlay.cbSize = sizeof(wcOverlay);
    wcOverlay.style = CS_HREDRAW | CS_VREDRAW;
    wcOverlay.lpfnWndProc = OverlayWndProc;
    wcOverlay.hInstance = hInstance;
    wcOverlay.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcOverlay.hbrBackground = NULL; // Transparent
    wcOverlay.lpszClassName = "LWSROverlay";
    RegisterClassExA(&wcOverlay);
    
    // Register control panel class
    WNDCLASSEXA wcControl = {0};
    wcControl.cbSize = sizeof(wcControl);
    wcControl.style = CS_HREDRAW | CS_VREDRAW;
    wcControl.lpfnWndProc = ControlWndProc;
    wcControl.hInstance = hInstance;
    wcControl.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcControl.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wcControl.lpszClassName = "LWSRControl";
    RegisterClassExA(&wcControl);
    
    // Register settings window class
    WNDCLASSEXA wcSettings = {0};
    wcSettings.cbSize = sizeof(wcSettings);
    wcSettings.style = CS_HREDRAW | CS_VREDRAW;
    wcSettings.lpfnWndProc = SettingsWndProc;
    wcSettings.hInstance = hInstance;
    wcSettings.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcSettings.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wcSettings.lpszClassName = "LWSRSettings";
    RegisterClassExA(&wcSettings);
    
    // Register crosshair window class
    WNDCLASSEXA wcCross = {0};
    wcCross.cbSize = sizeof(wcCross);
    wcCross.style = CS_HREDRAW | CS_VREDRAW;
    wcCross.lpfnWndProc = CrosshairWndProc;
    wcCross.hInstance = hInstance;
    wcCross.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcCross.hbrBackground = NULL;
    wcCross.lpszClassName = "LWSRCrosshair";
    RegisterClassExA(&wcCross);
    
    // Initialize new action toolbar module
    ActionToolbar_Init(hInstance);
    ActionToolbar_SetCallbacks(Recording_Start, CaptureToClipboard, CaptureToFile, NULL);
    
    // Register timer display window class
    WNDCLASSEXA wcTimer = {0};
    wcTimer.cbSize = sizeof(wcTimer);
    wcTimer.style = CS_HREDRAW | CS_VREDRAW;
    wcTimer.lpfnWndProc = TimerWndProc;
    wcTimer.hInstance = hInstance;
    wcTimer.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcTimer.hbrBackground = NULL;
    wcTimer.lpszClassName = "LWSRTimer";
    RegisterClassExA(&wcTimer);
    
    // Initialize border module
    Border_Init(hInstance);
    
    // Get virtual screen bounds (all monitors)
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    // Create overlay window (fullscreen, layered for per-pixel alpha)
    g_overlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        "LWSROverlay",
        NULL,
        WS_POPUP,
        vsX, vsY, vsW, vsH,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_overlayWnd) return FALSE;
    
    // Initial overlay bitmap will be set by UpdateOverlayBitmap when mode is selected
    
    // Create control panel (top center) - Windows 11 Snipping Tool style
    POINT center;
    GetPrimaryMonitorCenter(&center);
    
    int ctrlWidth = 680;
    int ctrlHeight = 44;
    
    g_controlWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "LWSRControl",
        NULL,
        WS_POPUP | WS_VISIBLE,
        center.x - ctrlWidth / 2, center.y - ctrlHeight / 2,
        ctrlWidth, ctrlHeight,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_controlWnd) {
        DestroyWindow(g_overlayWnd);
        return FALSE;
    }
    
    // Apply smooth rounded corners using DWM (Windows 11+)
    ApplyRoundedCorners(g_controlWnd);
    
    // Create crosshair indicator window
    g_crosshairWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "LWSRCrosshair",
        NULL,
        WS_POPUP,
        -9999, -9999, 80, 80,  // Start off-screen
        NULL, NULL, hInstance, NULL
    );
    
    SetLayeredWindowAttributes(g_crosshairWnd, RGB(0, 0, 0), 200, LWA_ALPHA);
    
    // Action toolbar is now managed by action_toolbar module
    
    // Create stop recording button (hidden initially) - click to stop
    g_timerWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "LWSRTimer",
        NULL,
        WS_POPUP,
        -9999, -9999, 175, 28,  // Start off-screen to avoid capture issues
        NULL, NULL, hInstance, NULL
    );
    
    // Exclude timer window from screen capture (Windows 10 2004+)
    // WDA_EXCLUDEFROMCAPTURE = 0x00000011
    SetWindowDisplayAffinity(g_timerWnd, 0x00000011);
    
    // Don't show overlay or crosshair initially - only when mode is selected
    g_isSelecting = FALSE;
    g_selState = SEL_NONE;
    
    // Only show the control panel at startup
    UpdateWindow(g_controlWnd);
    
    return TRUE;
}

void Overlay_Destroy(void) {
    if (g_isRecording) {
        Recording_Stop();
    }
    
    // Shutdown GDI+
    ShutdownGdiPlus();
    
    if (g_crosshairWnd) {
        DestroyWindow(g_crosshairWnd);
        g_crosshairWnd = NULL;
    }
    
    if (g_settingsWnd) {
        DestroyWindow(g_settingsWnd);
        g_settingsWnd = NULL;
    }
    
    // Shutdown action toolbar module
    ActionToolbar_Shutdown();
    
    if (g_timerWnd) {
        DestroyWindow(g_timerWnd);
        g_timerWnd = NULL;
    }
    
    // Shutdown border module
    Border_Shutdown();
    
    if (g_controlWnd) {
        DestroyWindow(g_controlWnd);
        g_controlWnd = NULL;
    }
    
    if (g_overlayWnd) {
        DestroyWindow(g_overlayWnd);
        g_overlayWnd = NULL;
    }
}

void Overlay_SetMode(CaptureMode mode) {
    g_currentMode = mode;
    g_isSelecting = TRUE;
    g_selState = SEL_NONE;
    SetRectEmpty(&g_selectedRect);
    ShowActionToolbar(FALSE);
    
    // Update overlay based on mode
    if (mode == MODE_AREA || mode == MODE_WINDOW || mode == MODE_MONITOR || mode == MODE_ALL_MONITORS) {
        // Show overlay with dark tint
        UpdateOverlayBitmap();
        ShowWindow(g_overlayWnd, SW_SHOW);
        
        // Make sure control panel stays on top of overlay
        SetWindowPos(g_overlayWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(g_controlWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(g_overlayWnd);
    } else if (mode == MODE_NONE) {
        // Hide overlay
        ShowWindow(g_overlayWnd, SW_HIDE);
    }
    
    InvalidateRect(g_overlayWnd, NULL, TRUE);
}

BOOL Overlay_GetSelectedRegion(RECT* region) {
    if (IsRectEmpty(&g_selectedRect)) return FALSE;
    *region = g_selectedRect;
    return TRUE;
}

HWND Overlay_GetWindow(void) {
    return g_overlayWnd;
}

void Recording_Start(void) {
    if (g_isRecording) return;
    if (IsRectEmpty(&g_selectedRect)) return;
    
    // Set capture region
    if (!Capture_SetRegion(&g_capture, g_selectedRect)) {
        MessageBoxA(NULL, "Failed to set capture region", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Validate dimensions
    if (g_capture.captureWidth < 16 || g_capture.captureHeight < 16) {
        MessageBoxA(NULL, "Capture area too small", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Generate output filename
    char outputPath[MAX_PATH];
    Encoder_GenerateFilename(outputPath, MAX_PATH, 
                             g_config.savePath, g_config.outputFormat);
    
    // Initialize encoder
    int fps = Capture_GetRefreshRate(&g_capture);
    if (fps > 60) fps = 60; // Cap at 60 FPS for encoder compatibility
    if (!Encoder_Init(&g_encoder, outputPath, 
                      g_capture.captureWidth, g_capture.captureHeight,
                      fps, g_config.outputFormat, g_config.quality)) {
        char errMsg[512];
        snprintf(errMsg, sizeof(errMsg), 
            "Failed to initialize encoder.\nPath: %s\nSize: %dx%d\nFPS: %d",
            outputPath, g_capture.captureWidth, g_capture.captureHeight, fps);
        MessageBoxA(NULL, errMsg, "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Hide selection UI
    ShowWindow(g_overlayWnd, SW_HIDE);
    ShowWindow(g_crosshairWnd, SW_HIDE);
    ActionToolbar_Hide();
    ShowWindow(g_controlWnd, SW_HIDE);
    
    // Start recording
    g_isRecording = TRUE;
    g_isSelecting = FALSE;
    g_stopRecording = FALSE;
    g_recordStartTime = GetTickCount();
    
    // Show recording border if enabled
    if (g_config.showRecordingBorder) {
        Border_Show(g_selectedRect);
    }
    
    // Show timer display
    SetWindowTextA(g_timerWnd, "0");
    UpdateTimerDisplay();
    ShowWindow(g_timerWnd, SW_SHOW);
    SetTimer(g_timerWnd, ID_TIMER_DISPLAY, 1000, NULL); // Update every second
    
    // Update control panel
    Overlay_SetRecordingState(TRUE);
    
    // Start recording thread
    g_recordThread = CreateThread(NULL, 0, RecordingThread, NULL, 0, NULL);
    
    // Start time limit timer if configured
    if (g_config.maxRecordingSeconds > 0) {
        SetTimer(g_controlWnd, ID_TIMER_LIMIT, 
                 g_config.maxRecordingSeconds * 1000, NULL);
    }
}

void Recording_Stop(void) {
    if (!g_isRecording) return;
    
    g_stopRecording = TRUE;
    
    // Wait for recording thread
    if (g_recordThread) {
        WaitForSingleObject(g_recordThread, 5000);
        CloseHandle(g_recordThread);
        g_recordThread = NULL;
    }
    
    // Finalize encoder
    Encoder_Finalize(&g_encoder);
    
    g_isRecording = FALSE;
    
    // Hide recording border
    Border_Hide();
    
    // Hide timer
    KillTimer(g_timerWnd, ID_TIMER_DISPLAY);
    ShowWindow(g_timerWnd, SW_HIDE);
    
    KillTimer(g_controlWnd, ID_TIMER_LIMIT);
    
    // Save config with last capture rect
    g_config.lastCaptureRect = g_selectedRect;
    g_config.lastMode = g_currentMode;
    Config_Save(&g_config);
    
    // Exit application
    PostQuitMessage(0);
}

static DWORD WINAPI RecordingThread(LPVOID param) {
    (void)param;
    
    // Request high-resolution timer (1ms instead of 15.6ms)
    timeBeginPeriod(1);
    
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    
    int fps = Capture_GetRefreshRate(&g_capture);
    if (fps > 60) fps = 60;
    
    // Use frame count for consistent timing
    UINT64 frameCount = 0;
    UINT64 frameDuration100ns = 10000000ULL / fps; // 100-nanosecond units for MF
    double frameIntervalSec = 1.0 / fps;
    
    while (!g_stopRecording) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        double targetTime = frameCount * frameIntervalSec;
        
        if (elapsed >= targetTime) {
            // Use frame-based timestamp for smooth playback
            UINT64 timestamp = frameCount * frameDuration100ns;
            BYTE* frame = Capture_GetFrame(&g_capture, NULL); // Ignore DXGI timestamp
            
            if (frame) {
                Encoder_WriteFrame(&g_encoder, frame, timestamp);
            }
            
            frameCount++;
            
            // Skip frames if we're falling behind (drop frames rather than stutter)
            double newElapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
            while ((frameCount * frameIntervalSec) < newElapsed - frameIntervalSec) {
                frameCount++; // Skip this frame
            }
        } else {
            // Sleep until next frame time - use high precision sleep
            double sleepTime = (targetTime - elapsed) * 1000.0;
            if (sleepTime > 2.0) {
                Sleep((DWORD)(sleepTime - 1.5));
            } else if (sleepTime > 0.5) {
                Sleep(1);
            }
            // Busy-wait for sub-millisecond precision
        }
    }
    
    timeEndPeriod(1);
    return 0;
}

void Overlay_SetRecordingState(BOOL isRecording) {
    HWND btnStop = GetDlgItem(g_controlWnd, ID_BTN_STOP);
    
    if (isRecording) {
        // Hide mode buttons, show stop button
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_AREA), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_WINDOW), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_MONITOR), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_ALL), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_BTN_SETTINGS), SW_HIDE);
        
        if (btnStop) {
            ShowWindow(btnStop, SW_SHOW);
            SetWindowTextA(btnStop, "Stop Recording");
        }
        
        // Show recording border if enabled
        if (g_config.showRecordingBorder) {
            // Position control near capture area
            SetWindowPos(g_controlWnd, HWND_TOPMOST,
                         g_selectedRect.left, g_selectedRect.top - 60,
                         200, 40, SWP_SHOWWINDOW);
        }
    } else {
        // Show mode buttons, hide stop button
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_AREA), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_WINDOW), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_MONITOR), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_ALL), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_BTN_SETTINGS), SW_SHOW);
        
        if (btnStop) {
            ShowWindow(btnStop, SW_HIDE);
        }
    }
}

// Overlay window procedure - handles selection
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_USER + 1: // Stop recording signal from second instance
            if (g_isRecording) {
                Recording_Stop();
            } else {
                PostQuitMessage(0);
            }
            return 0;
        
        case WM_SETCURSOR: {
            // Cursor depends on state and what's under the mouse
            POINT pt;
            GetCursorPos(&pt);
            
            if (g_selState == SEL_COMPLETE) {
                HandlePosition handle = HitTestHandle(pt);
                if (handle != HANDLE_NONE) {
                    SetCursor(GetHandleCursor(handle));
                    return TRUE;
                }
                // Show move cursor on border OR inside selection
                if (PtOnSelectionBorder(pt) || PtInSelection(pt)) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEALL));
                    return TRUE;
                }
            }
            
            // Default cursor based on mode
            if (g_currentMode == MODE_AREA) {
                SetCursor(LoadCursor(NULL, IDC_CROSS));
            } else if (g_currentMode == MODE_WINDOW || g_currentMode == MODE_MONITOR || g_currentMode == MODE_ALL_MONITORS) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
            } else {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            }
            return TRUE;
        }
            
        case WM_LBUTTONDOWN: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_currentMode == MODE_AREA) {
                if (g_selState == SEL_COMPLETE) {
                    // Check if clicking on a handle
                    HandlePosition handle = HitTestHandle(pt);
                    if (handle != HANDLE_NONE) {
                        g_selState = SEL_RESIZING;
                        g_activeHandle = handle;
                        g_originalRect = g_selectedRect;
                        g_moveStart = pt;
                        SetCapture(hwnd);
                        ShowActionToolbar(FALSE);
                        return 0;
                    }
                    
                    // Check if clicking inside selection OR on border (move)
                    if (PtOnSelectionBorder(pt) || PtInSelection(pt)) {
                        g_selState = SEL_MOVING;
                        g_originalRect = g_selectedRect;
                        g_moveStart = pt;
                        SetCapture(hwnd);
                        ShowActionToolbar(FALSE);
                        return 0;
                    }
                    
                    // Clicking outside - start new selection
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    ShowActionToolbar(FALSE);
                }
                
                // Start drawing new selection
                g_selState = SEL_DRAWING;
                g_dragStart = pt;
                g_dragEnd = pt;
                SetCapture(hwnd);
            }
            return 0;
        }
            
        case WM_MOUSEMOVE: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_selState == SEL_DRAWING) {
                g_dragEnd = pt;
                
                // Update selection rect
                g_selectedRect.left = min(g_dragStart.x, g_dragEnd.x);
                g_selectedRect.top = min(g_dragStart.y, g_dragEnd.y);
                g_selectedRect.right = max(g_dragStart.x, g_dragEnd.x);
                g_selectedRect.bottom = max(g_dragStart.y, g_dragEnd.y);
                
                UpdateOverlayBitmap();
            } else if (g_selState == SEL_MOVING) {
                int dx = pt.x - g_moveStart.x;
                int dy = pt.y - g_moveStart.y;
                
                g_selectedRect.left = g_originalRect.left + dx;
                g_selectedRect.top = g_originalRect.top + dy;
                g_selectedRect.right = g_originalRect.right + dx;
                g_selectedRect.bottom = g_originalRect.bottom + dy;
                
                UpdateOverlayBitmap();
            } else if (g_selState == SEL_RESIZING) {
                int dx = pt.x - g_moveStart.x;
                int dy = pt.y - g_moveStart.y;
                
                g_selectedRect = g_originalRect;
                
                // Apply resize based on active handle
                switch (g_activeHandle) {
                    case HANDLE_TL:
                        g_selectedRect.left += dx;
                        g_selectedRect.top += dy;
                        break;
                    case HANDLE_T:
                        g_selectedRect.top += dy;
                        break;
                    case HANDLE_TR:
                        g_selectedRect.right += dx;
                        g_selectedRect.top += dy;
                        break;
                    case HANDLE_L:
                        g_selectedRect.left += dx;
                        break;
                    case HANDLE_R:
                        g_selectedRect.right += dx;
                        break;
                    case HANDLE_BL:
                        g_selectedRect.left += dx;
                        g_selectedRect.bottom += dy;
                        break;
                    case HANDLE_B:
                        g_selectedRect.bottom += dy;
                        break;
                    case HANDLE_BR:
                        g_selectedRect.right += dx;
                        g_selectedRect.bottom += dy;
                        break;
                    default:
                        break;
                }
                
                // Normalize rect (ensure left < right, top < bottom)
                if (g_selectedRect.left > g_selectedRect.right) {
                    int tmp = g_selectedRect.left;
                    g_selectedRect.left = g_selectedRect.right;
                    g_selectedRect.right = tmp;
                }
                if (g_selectedRect.top > g_selectedRect.bottom) {
                    int tmp = g_selectedRect.top;
                    g_selectedRect.top = g_selectedRect.bottom;
                    g_selectedRect.bottom = tmp;
                }
                
                UpdateOverlayBitmap();
            } else if (g_isSelecting && g_selState == SEL_NONE) {
                // Just moving mouse over overlay, cursor handles itself
            }
            return 0;
        }
            
        case WM_LBUTTONUP: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_selState == SEL_DRAWING) {
                ReleaseCapture();
                
                int width = g_selectedRect.right - g_selectedRect.left;
                int height = g_selectedRect.bottom - g_selectedRect.top;
                
                if (width >= 10 && height >= 10) {
                    // Selection complete - show handles and action toolbar
                    g_selState = SEL_COMPLETE;
                    UpdateOverlayBitmap();
                    ShowActionToolbar(TRUE);
                } else {
                    // Too small - reset
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    UpdateOverlayBitmap();
                }
            } else if (g_selState == SEL_MOVING || g_selState == SEL_RESIZING) {
                ReleaseCapture();
                g_selState = SEL_COMPLETE;
                g_activeHandle = HANDLE_NONE;
                UpdateOverlayBitmap();
                ShowActionToolbar(TRUE);
            } else if (g_isSelecting && g_currentMode != MODE_AREA) {
                // Window/Monitor click mode
                if (g_currentMode == MODE_WINDOW) {
                    HWND targetWnd = WindowFromPoint(pt);
                    if (targetWnd) {
                        HWND topLevel = GetAncestor(targetWnd, GA_ROOT);
                        if (topLevel) {
                            Capture_GetWindowRect(topLevel, &g_selectedRect);
                            g_selState = SEL_COMPLETE;
                            UpdateOverlayBitmap();
                            ShowActionToolbar(TRUE);
                        }
                    }
                } else if (g_currentMode == MODE_MONITOR) {
                    RECT monRect;
                    int monIndex;
                    if (Capture_GetMonitorFromPoint(pt, &monRect, &monIndex)) {
                        g_selectedRect = monRect;
                        g_selState = SEL_COMPLETE;
                        UpdateOverlayBitmap();
                        ShowActionToolbar(TRUE);
                    }
                } else if (g_currentMode == MODE_ALL_MONITORS) {
                    Capture_GetAllMonitorsBounds(&g_selectedRect);
                    g_selState = SEL_COMPLETE;
                    UpdateOverlayBitmap();
                    ShowActionToolbar(TRUE);
                }
            }
            return 0;
        }
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (g_selState == SEL_DRAWING || g_selState == SEL_MOVING || g_selState == SEL_RESIZING) {
                    ReleaseCapture();
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    UpdateOverlayBitmap();
                    ShowActionToolbar(FALSE);
                } else if (g_selState == SEL_COMPLETE) {
                    // Cancel selection
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    UpdateOverlayBitmap();
                    ShowActionToolbar(FALSE);
                } else {
                    PostQuitMessage(0);
                }
            } else if (wParam == VK_RETURN && g_selState == SEL_COMPLETE) {
                // Enter key starts recording
                Recording_Start();
            }
            return 0;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Track which mode button is hovered/selected
static int g_hoveredButton = 0;
static HFONT g_uiFont = NULL;
static HFONT g_iconFont = NULL;

// Control panel window procedure
static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Create UI fonts
            g_uiFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_iconFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Symbol");
            
            // Create mode buttons (owner-drawn for Snipping Tool style)
            CreateWindowW(L"BUTTON", L"Capture Area",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                8, 7, 120, 30, hwnd, (HMENU)ID_MODE_AREA, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture Window",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                132, 7, 130, 30, hwnd, (HMENU)ID_MODE_WINDOW, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture Monitor",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                266, 7, 135, 30, hwnd, (HMENU)ID_MODE_MONITOR, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture All Monitors",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                405, 7, 160, 30, hwnd, (HMENU)ID_MODE_ALL, g_hInstance, NULL);
            
            // Small buttons on right side (square 28x28, vertically centered)
            int btnSize = 28;
            int btnY = (44 - btnSize) / 2;  // Center in 44px tall window
            
            // Close button (right side)
            CreateWindowW(L"BUTTON", L"\\u2715",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                644, btnY, btnSize, btnSize, hwnd, (HMENU)ID_BTN_CLOSE, g_hInstance, NULL);
            
            // Settings button (gear icon area)
            CreateWindowW(L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                574, btnY, btnSize, btnSize, hwnd, (HMENU)ID_BTN_SETTINGS, g_hInstance, NULL);
            
            // Record button
            CreateWindowW(L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                609, btnY, btnSize, btnSize, hwnd, (HMENU)ID_BTN_RECORD, g_hInstance, NULL);
            
            // No mode selected by default
            g_currentMode = MODE_NONE;
            
            return 0;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_MODE_AREA:
                    Overlay_SetMode(MODE_AREA);
                    // Invalidate all mode buttons
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_MODE_WINDOW:
                    Overlay_SetMode(MODE_WINDOW);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_MODE_MONITOR:
                    Overlay_SetMode(MODE_MONITOR);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_MODE_ALL:
                    Overlay_SetMode(MODE_ALL_MONITORS);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_BTN_SETTINGS:
                    // Toggle settings window
                    if (g_settingsWnd) {
                        // Close settings if already open
                        DestroyWindow(g_settingsWnd);
                        g_settingsWnd = NULL;
                    } else {
                        // Open settings below control panel, centered
                        RECT ctrlRect;
                        GetWindowRect(hwnd, &ctrlRect);
                        int settingsW = 620;
                        int settingsH = 240;
                        int ctrlCenterX = (ctrlRect.left + ctrlRect.right) / 2;
                        
                        g_settingsWnd = CreateWindowExA(
                            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                            "LWSRSettings",
                            NULL,
                            WS_POPUP | WS_VISIBLE | WS_BORDER,
                            ctrlCenterX - settingsW / 2, ctrlRect.bottom + 5,
                            settingsW, settingsH,
                            hwnd, NULL, g_hInstance, NULL
                        );
                    }
                    break;
                case ID_BTN_RECORD:
                    // Toggle recording
                    if (g_isRecording) {
                        Recording_Stop();
                    } else {
                        // If no selection, use full primary monitor
                        if (IsRectEmpty(&g_selectedRect)) {
                            HMONITOR hMon = MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY);
                            MONITORINFO mi = { sizeof(mi) };
                            GetMonitorInfo(hMon, &mi);
                            g_selectedRect = mi.rcMonitor;
                        }
                        Recording_Start();
                    }
                    // Redraw button to show state change
                    InvalidateRect(GetDlgItem(hwnd, ID_BTN_RECORD), NULL, TRUE);
                    break;
                case ID_BTN_CLOSE:
                    PostQuitMessage(0);
                    break;
                case ID_BTN_STOP:
                    Recording_Stop();
                    break;
            }
            return 0;
            
        case WM_TIMER:
            if (wParam == ID_TIMER_LIMIT) {
                // Time limit reached
                Recording_Stop();
            }
            return 0;
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Dark background (Windows 11 dark theme)
            RECT rect;
            GetClientRect(hwnd, &rect);
            HBRUSH bgBrush = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);
            
            // Draw subtle border
            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HPEN oldPen = SelectObject(hdc, borderPen);
            HBRUSH oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(borderPen);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            UINT ctlId = dis->CtlID;
            BOOL isSelected = FALSE;
            BOOL isHovered = (dis->itemState & ODS_HOTLIGHT) || (dis->itemState & ODS_FOCUS);
            
            // Check if this is a mode button
            BOOL isModeButton = (ctlId >= ID_MODE_AREA && ctlId <= ID_MODE_ALL);
            
            if (isModeButton) {
                // Check if this mode is selected
                isSelected = (ctlId == ID_MODE_AREA && g_currentMode == MODE_AREA) ||
                            (ctlId == ID_MODE_WINDOW && g_currentMode == MODE_WINDOW) ||
                            (ctlId == ID_MODE_MONITOR && g_currentMode == MODE_MONITOR) ||
                            (ctlId == ID_MODE_ALL && g_currentMode == MODE_ALL_MONITORS);
            }
            
            // Background color
            COLORREF bgColor;
            COLORREF borderColor;
            if (isSelected) {
                bgColor = RGB(0, 95, 184); // Windows blue for selected
                borderColor = RGB(0, 120, 215);
            } else if (isHovered || (dis->itemState & ODS_SELECTED)) {
                bgColor = RGB(55, 55, 55); // Hover color
                borderColor = RGB(80, 80, 80);
            } else {
                bgColor = RGB(32, 32, 32); // Normal background
                borderColor = RGB(80, 80, 80);
            }
            
            // Draw anti-aliased rounded button background
            DrawRoundedRectAA(dis->hDC, &dis->rcItem, 6, bgColor, borderColor);
            
            // Draw text for mode buttons (no icon, just centered text)
            if (isModeButton) {
                SelectObject(dis->hDC, g_uiFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(255, 255, 255));
                
                // Get button text
                WCHAR text[64];
                GetWindowTextW(dis->hwndItem, text, 64);
                
                // Draw centered text
                RECT textRect = dis->rcItem;
                DrawTextW(dis->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                
                return TRUE;
            }
            
            // Record button
            if (ctlId == ID_BTN_RECORD) {
                int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                
                if (g_isRecording) {
                    // White square (stop icon)
                    HBRUSH iconBrush = CreateSolidBrush(RGB(255, 255, 255));
                    RECT stopRect = { cx - 4, cy - 4, cx + 4, cy + 4 };
                    FillRect(dis->hDC, &stopRect, iconBrush);
                    DeleteObject(iconBrush);
                } else {
                    // Red filled circle (record icon) - use anti-aliased circle
                    DrawCircleAA(dis->hDC, cx, cy, 6, RGB(220, 50, 50));
                }
                return TRUE;
            }
            
            // Settings button (three horizontal dots)
            if (ctlId == ID_BTN_SETTINGS) {
                SelectObject(dis->hDC, g_uiFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(200, 200, 200));
                RECT textRect = dis->rcItem;
                textRect.left += 1;  // Shift 1 pixel right
                textRect.right += 1;
                // Use horizontal ellipsis or three dots
                DrawTextW(dis->hDC, L"\u22EF", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            
            // Close button
            if (ctlId == ID_BTN_CLOSE) {
                SelectObject(dis->hDC, g_iconFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(200, 200, 200));
                RECT textRect = dis->rcItem;
                textRect.left += 1;  // Shift 1 pixel right
                textRect.right += 1;
                DrawTextW(dis->hDC, L"\u2715", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            
            break;
        }
        
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdcCtrl = (HDC)wParam;
            SetBkMode(hdcCtrl, TRANSPARENT);
            SetTextColor(hdcCtrl, RGB(255, 255, 255));
            static HBRUSH hBrush = NULL;
            if (!hBrush) hBrush = CreateSolidBrush(RGB(32, 32, 32));
            return (LRESULT)hBrush;
        }
        
        case WM_DESTROY:
            if (g_uiFont) DeleteObject(g_uiFont);
            if (g_iconFont) DeleteObject(g_iconFont);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Settings window procedure
static HFONT g_settingsFont = NULL;
static HFONT g_settingsSmallFont = NULL;
static HBRUSH g_settingsBgBrush = NULL;

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Create fonts
            g_settingsFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsSmallFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsBgBrush = CreateSolidBrush(RGB(32, 32, 32));
            
            // Get window width for centering
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int windowW = clientRect.right;
            
            int contentW = 560; // Total content width
            int marginX = (windowW - contentW) / 2; // Center margin
            
            int y = 20;
            int labelX = marginX;
            int labelW = 110;
            int controlX = marginX + labelW + 10;
            int controlW = contentW - labelW - 10;
            int rowH = 38;
            
            // Format dropdown
            HWND lblFormat = CreateWindowW(L"STATIC", L"Output Format", 
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblFormat, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbFormat = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                controlX, y, controlW, 120, hwnd, (HMENU)ID_CMB_FORMAT, g_hInstance, NULL);
            SendMessage(cmbFormat, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.264) - Best compatibility");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"AVI - Legacy format");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"WMV - Windows Media");
            SendMessage(cmbFormat, CB_SETCURSEL, g_config.outputFormat, 0);
            y += rowH;
            
            // Quality dropdown with descriptions
            HWND lblQuality = CreateWindowW(L"STATIC", L"Quality",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblQuality, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbQuality = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                controlX, y, controlW, 120, hwnd, (HMENU)ID_CMB_QUALITY, g_hInstance, NULL);
            SendMessage(cmbQuality, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Low - Small file, lower clarity");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Medium - Balanced quality/size");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"High - Sharp video, larger file");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Lossless - Perfect quality, huge file");
            SendMessage(cmbQuality, CB_SETCURSEL, g_config.quality, 0);
            y += rowH + 8;
            
            // Separator line
            CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_hInstance, NULL);
            y += 14;
            
            // Checkboxes side by side
            HWND chkMouse = CreateWindowW(L"BUTTON", L"Capture mouse cursor",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 200, 24, hwnd, (HMENU)ID_CHK_MOUSE, g_hInstance, NULL);
            SendMessage(chkMouse, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_MOUSE, g_config.captureMouse ? BST_CHECKED : BST_UNCHECKED);
            
            // Show border checkbox - on the right side
            HWND chkBorder = CreateWindowW(L"BUTTON", L"Show recording border",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX + 280, y, 200, 24, hwnd, (HMENU)ID_CHK_BORDER, g_hInstance, NULL);
            SendMessage(chkBorder, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_BORDER, g_config.showRecordingBorder ? BST_CHECKED : BST_UNCHECKED);
            y += 38;
            
            // Time limit - three dropdowns for hours, minutes, seconds
            HWND lblTime = CreateWindowW(L"STATIC", L"Time limit",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                labelX, y, labelW, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblTime, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Calculate time from seconds
            int totalSecs = g_config.maxRecordingSeconds;
            if (totalSecs < 1) totalSecs = 60; // Default to 1 minute minimum
            int hours = totalSecs / 3600;
            int mins = (totalSecs % 3600) / 60;
            int secs = totalSecs % 60;
            
            // Hours dropdown (CBS_DROPDOWNLIST for mouse wheel support)
            HWND cmbHours = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, 55, 300, hwnd, (HMENU)ID_CMB_HOURS, g_hInstance, NULL);
            SendMessage(cmbHours, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            for (int i = 0; i <= 24; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbHours, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbHours, CB_SETCURSEL, hours, 0);
            
            HWND lblH = CreateWindowW(L"STATIC", L"h",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 58, y, 15, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblH, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Minutes dropdown
            HWND cmbMins = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 78, y, 55, 300, hwnd, (HMENU)ID_CMB_MINUTES, g_hInstance, NULL);
            SendMessage(cmbMins, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbMins, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbMins, CB_SETCURSEL, mins, 0);
            
            HWND lblM = CreateWindowW(L"STATIC", L"m",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 136, y, 18, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblM, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Seconds dropdown
            HWND cmbSecs = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 158, y, 55, 300, hwnd, (HMENU)ID_CMB_SECONDS, g_hInstance, NULL);
            SendMessage(cmbSecs, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbSecs, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbSecs, CB_SETCURSEL, secs, 0);
            
            HWND lblS = CreateWindowW(L"STATIC", L"s",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 216, y, 15, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            y += rowH;
            
            // Save path - aligned with dropdowns
            HWND lblPath = CreateWindowW(L"STATIC", L"Save to",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                labelX, y + 1, labelW, 22, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblPath, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Edit control - height 22 matches font better for vertical centering
            HWND edtPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                controlX, y, controlW - 80, 22, hwnd, (HMENU)ID_EDT_PATH, g_hInstance, NULL);
            SendMessage(edtPath, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SetWindowTextA(edtPath, g_config.savePath);
            
            HWND btnBrowse = CreateWindowW(L"BUTTON", L"Browse",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                controlX + controlW - 72, y, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, g_hInstance, NULL);
            SendMessage(btnBrowse, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            return 0;
        }
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, g_settingsBgBrush);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, RGB(220, 220, 220));
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)g_settingsBgBrush;
        }
        
        case WM_CTLCOLORBTN: {
            HDC hdcBtn = (HDC)wParam;
            SetTextColor(hdcBtn, RGB(220, 220, 220));
            SetBkMode(hdcBtn, TRANSPARENT);
            return (LRESULT)g_settingsBgBrush;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_CMB_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.outputFormat = (OutputFormat)SendMessage(
                            GetDlgItem(hwnd, ID_CMB_FORMAT), CB_GETCURSEL, 0, 0);
                    }
                    break;
                    
                case ID_CMB_QUALITY:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.quality = (QualityPreset)SendMessage(
                            GetDlgItem(hwnd, ID_CMB_QUALITY), CB_GETCURSEL, 0, 0);
                    }
                    break;
                    
                case ID_CHK_MOUSE:
                    g_config.captureMouse = IsDlgButtonChecked(hwnd, ID_CHK_MOUSE) == BST_CHECKED;
                    break;
                    
                case ID_CHK_BORDER:
                    g_config.showRecordingBorder = IsDlgButtonChecked(hwnd, ID_CHK_BORDER) == BST_CHECKED;
                    break;
                    
                case ID_CMB_HOURS:
                case ID_CMB_MINUTES:
                case ID_CMB_SECONDS:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // Get values from all three dropdowns
                        int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
                        int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
                        int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
                        
                        // Calculate total seconds (minimum 1 second)
                        int total = hours * 3600 + mins * 60 + secs;
                        if (total < 1) total = 1;
                        g_config.maxRecordingSeconds = total;
                    }
                    break;
                    
                case ID_BTN_BROWSE: {
                    BROWSEINFOA bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = "Select Save Folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    
                    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
                    if (pidl) {
                        char path[MAX_PATH];
                        if (SHGetPathFromIDListA(pidl, path)) {
                            strncpy(g_config.savePath, path, MAX_PATH - 1);
                            SetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), path);
                            CreateDirectoryA(path, NULL);
                        }
                        CoTaskMemFree(pidl);
                    }
                    break;
                }
            }
            return 0;
            
        case WM_CLOSE: {
            // Save time limit from dropdowns
            int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
            int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
            int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
            
            int total = hours * 3600 + mins * 60 + secs;
            if (total < 1) total = 1; // Minimum 1 second
            g_config.maxRecordingSeconds = total;
            
            // Save path
            GetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), g_config.savePath, MAX_PATH);
            Config_Save(&g_config);
            
            // Clean up
            if (g_settingsFont) { DeleteObject(g_settingsFont); g_settingsFont = NULL; }
            if (g_settingsSmallFont) { DeleteObject(g_settingsSmallFont); g_settingsSmallFont = NULL; }
            if (g_settingsBgBrush) { DeleteObject(g_settingsBgBrush); g_settingsBgBrush = NULL; }
            
            DestroyWindow(hwnd);
            g_settingsWnd = NULL;
            return 0;
        }
        
        case WM_DESTROY:
            if (g_settingsFont) { DeleteObject(g_settingsFont); g_settingsFont = NULL; }
            if (g_settingsSmallFont) { DeleteObject(g_settingsSmallFont); g_settingsSmallFont = NULL; }
            if (g_settingsBgBrush) { DeleteObject(g_settingsBgBrush); g_settingsBgBrush = NULL; }
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Crosshair indicator window procedure
static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            // Dark background
            HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);
            
            // Draw crosshair
            HPEN bluePen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
            HPEN oldPen = (HPEN)SelectObject(hdc, bluePen);
            
            int cx = (rect.right - rect.left) / 2;
            int cy = (rect.bottom - rect.top) / 2;
            
            MoveToEx(hdc, cx, 0, NULL);
            LineTo(hdc, cx, rect.bottom);
            MoveToEx(hdc, 0, cy, NULL);
            LineTo(hdc, rect.right, cy);
            
            SelectObject(hdc, oldPen);
            DeleteObject(bluePen);
            
            // Draw size text
            if (!IsRectEmpty(&g_selectedRect)) {
                char sizeText[64];
                int w = g_selectedRect.right - g_selectedRect.left;
                int h = g_selectedRect.bottom - g_selectedRect.top;
                sprintf(sizeText, "%d x %d", w, h);
                
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                
                RECT textRect = rect;
                textRect.top = rect.bottom - 20;
                DrawTextA(hdc, sizeText, -1, &textRect, DT_CENTER | DT_VCENTER);
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Timer display font
static HFONT g_timerFont = NULL;
static BOOL g_timerHovered = FALSE;

// Timer display window procedure
static LRESULT CALLBACK TimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Same font style as overlay/toolbar - Segoe UI 12pt
            g_timerFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            
            // Apply rounded corners via DWM
            HMODULE hDwm = LoadLibraryA("dwmapi.dll");
            if (hDwm) {
                typedef HRESULT (WINAPI *DwmSetWindowAttributeFunc)(HWND, DWORD, LPCVOID, DWORD);
                DwmSetWindowAttributeFunc pDwmSetWindowAttribute = 
                    (DwmSetWindowAttributeFunc)GetProcAddress(hDwm, "DwmSetWindowAttribute");
                if (pDwmSetWindowAttribute) {
                    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
                    DWORD cornerPref = 2;
                    pDwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));
                }
            }
            return 0;
        }
        
        case WM_MOUSEMOVE:
            if (!g_timerHovered) {
                g_timerHovered = TRUE;
                InvalidateRect(hwnd, NULL, FALSE);
                
                // Track mouse leave
                TRACKMOUSEEVENT tme = {0};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        
        case WM_MOUSELEAVE:
            g_timerHovered = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        
        case WM_LBUTTONDOWN:
            // Click to stop recording
            if (g_isRecording) {
                Recording_Stop();
            }
            return 0;
        
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        
        case WM_TIMER:
            if (wParam == ID_TIMER_DISPLAY) {
                UpdateTimerDisplay();
            }
            return 0;
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            // Dark background - slightly lighter on hover
            COLORREF bgColor = g_timerHovered ? RGB(48, 48, 48) : RGB(32, 32, 32);
            HBRUSH bgBrush = CreateSolidBrush(bgColor);
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);
            
            // Use GDI+ for anti-aliased red dot
            if (GdipCreateFromHDC && GdipSetSmoothingMode && GdipCreateSolidFill && 
                GdipFillEllipse && GdipDeleteBrush && GdipDeleteGraphics) {
                GpGraphics* graphics = NULL;
                GdipCreateFromHDC(hdc, &graphics);
                if (graphics) {
                    GdipSetSmoothingMode(graphics, 4); // SmoothingModeAntiAlias
                    
                    // Red dot - ARGB format
                    GpBrush* redBrush = NULL;
                    GdipCreateSolidFill(0xFFEA4335, &redBrush); // Google red
                    if (redBrush) {
                        int dotSize = 8;
                        int dotY = (rect.bottom - dotSize) / 2 - 1;  // Move up 1px
                        GdipFillEllipse(graphics, redBrush, 8, (float)dotY, (float)dotSize, (float)dotSize);
                        GdipDeleteBrush(redBrush);
                    }
                    GdipDeleteGraphics(graphics);
                }
            }
            
            // Get timer text
            char timeText[32];
            GetWindowTextA(hwnd, timeText, 32);
            
            // Use Segoe UI font matching overlay style
            SelectObject(hdc, g_timerFont);
            SetBkMode(hdc, TRANSPARENT);
            // Slightly brighter text on hover
            COLORREF textColor = g_timerHovered ? RGB(230, 230, 230) : RGB(200, 200, 200);
            SetTextColor(hdc, textColor);
            
            // Draw timer
            RECT timerRect = rect;
            timerRect.left = 22;
            timerRect.right = 62;
            DrawTextA(hdc, timeText, -1, &timerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            
            // Draw vertical divider
            HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
            SelectObject(hdc, dividerPen);
            MoveToEx(hdc, 68, 6, NULL);
            LineTo(hdc, 68, rect.bottom - 6);
            DeleteObject(dividerPen);
            
            // Draw "Stop Recording" text
            RECT stopRect = rect;
            stopRect.left = 76;
            stopRect.right = rect.right - 4;
            DrawTextA(hdc, "Stop Recording", -1, &stopRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_DESTROY:
            if (g_timerFont) {
                DeleteObject(g_timerFont);
                g_timerFont = NULL;
            }
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

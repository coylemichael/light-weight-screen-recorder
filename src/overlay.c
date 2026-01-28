/*
 * Overlay Implementation
 * Selection UI, recording controls, and main logic
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation/precondition checks
 * - HRESULT checks use FAILED()/SUCCEEDED() macros exclusively
 * - UI errors show MessageBox to user when appropriate
 * - Returns BOOL to propagate errors; callers must check
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>   // For system tray (Shell_NotifyIcon)
#include <dwmapi.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include <stdio.h>

#include "action_toolbar.h"
#include "audio_device.h"
#include "border.h"
#include "capture.h"
#include "gdiplus_api.h"

// Additional GDI+ type for image loading (not in shared API)
typedef void* GpImage;

/* DWM window corner preference (Windows 11+) */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// OCR_NORMAL not defined in some Windows headers
#ifndef OCR_NORMAL
#define OCR_NORMAL 32512
#endif

#include "overlay.h"
#include "capture.h"
#include "encoder.h"
#include "config.h"
#include "replay_buffer.h"
#include "aac_encoder.h"
#include "logger.h"
#include "constants.h"
#include "health_monitor.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

// Hotkey ID for replay save (must match main.c)
#define HOTKEY_REPLAY_SAVE 1

// Custom window message for async replay save completion
#define WM_REPLAY_SAVE_COMPLETE (WM_USER + 200)

// External globals from main.c
extern AppConfig g_config;
extern CaptureState g_capture;
extern ReplayBufferState g_replayBuffer;
extern volatile LONG g_isRecording;  // Thread-safe: use InterlockedExchange
extern volatile LONG g_isSelecting;  // Thread-safe: use InterlockedExchange
extern HWND g_overlayWnd;
extern HWND g_controlWnd;

// Control IDs
#define ID_MODE_AREA       1001
#define ID_MODE_WINDOW     1002
#define ID_MODE_MONITOR    1003
#define ID_MODE_ALL        1004
#define ID_BTN_CLOSE       1005
#define ID_BTN_STOP        1006
#define ID_BTN_MINIMIZE    1020
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
#define ID_RECORDING_PANEL 1019  // Inline timer + stop button
#define ID_TIMER_RECORD    2001
#define ID_TIMER_LIMIT     2002
#define ID_TIMER_DISPLAY   2003
#define ID_TIMER_HOVER     2004  // Timer to update hover state on icon buttons
#define ID_TIMER_REPLAY_CHECK 2005  // Timer to check replay buffer health

// Replay buffer settings control IDs
#define ID_CHK_REPLAY_ENABLED   4001
#define ID_CMB_REPLAY_SOURCE    4002
#define ID_CMB_REPLAY_ASPECT    4003
#define ID_CMB_REPLAY_STORAGE   4004
#define ID_STATIC_REPLAY_INFO   4005
#define ID_BTN_REPLAY_HOTKEY    4006
#define ID_CMB_REPLAY_HOURS     4007
#define ID_CMB_REPLAY_MINS      4008
#define ID_CMB_REPLAY_SECS      4009
#define ID_CMB_REPLAY_FPS       4010
#define ID_STATIC_REPLAY_RAM    4011
#define ID_STATIC_REPLAY_CALC   4012

// Audio capture settings control IDs
#define ID_CHK_AUDIO_ENABLED    5001
#define ID_CMB_AUDIO_SOURCE1    5002
#define ID_CMB_AUDIO_SOURCE2    5003
#define ID_CMB_AUDIO_SOURCE3    5004
#define ID_SLD_AUDIO_VOLUME1    5005
#define ID_SLD_AUDIO_VOLUME2    5006
#define ID_SLD_AUDIO_VOLUME3    5007
#define ID_LBL_AUDIO_VOL1       5008
#define ID_LBL_AUDIO_VOL2       5009
#define ID_LBL_AUDIO_VOL3       5010

// Debug settings control IDs
#define ID_CHK_DEBUG_LOGGING    6001

// Settings window dimensions
#define SETTINGS_WIDTH  620
#define SETTINGS_HEIGHT 770

// Action toolbar button IDs
#define ID_ACTION_RECORD   3001
#define ID_ACTION_COPY     3002
#define ID_ACTION_SAVE     3003
#define ID_ACTION_MARKUP   3004

// System tray
#define WM_TRAYICON        (WM_USER + 100)
#define ID_TRAY_SHOW       6001
#define ID_TRAY_EXIT       6002

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

/* ============================================================================
 * OVERLAY UI STATE
 * ============================================================================
 * Consolidated state structs for the overlay window UI.
 * Thread Access: [Main thread only unless otherwise noted]
 */

/*
 * SelectionUIState - Mouse-driven selection rectangle state
 */
typedef struct SelectionUIState {
    SelectionState state;           /* Current selection state machine */
    HandlePosition activeHandle;    /* Which resize handle is active */
    BOOL isDragging;
    POINT dragStart;
    POINT dragEnd;
    POINT moveStart;                /* For moving selection */
    RECT selectedRect;
    RECT originalRect;              /* Original rect before resize/move */
} SelectionUIState;

/*
 * RecordingUIState - Recording thread and timing state
 * Note: stopRecording is atomic for thread-safe access
 */
typedef struct RecordingUIState {
    EncoderState encoder;           /* Traditional recording encoder */
    HANDLE thread;                  /* Recording thread handle */
    volatile LONG stopRecording;    /* Thread Access: [Any - atomic] */
    ULONGLONG startTime;            /* Recording start time (64-bit) */
    CaptureMode recordingMode;      /* Mode that started recording */
} RecordingUIState;

/*
 * OverlayWindowState - Window handles for overlay UI
 */
typedef struct OverlayWindowState {
    HINSTANCE hInstance;
    HWND settingsWnd;
    HWND crosshairWnd;
    HWND recordingPanel;            /* Inline timer + stop in control bar */
} OverlayWindowState;

/*
 * TrayIconState - System tray icon state
 */
typedef struct TrayIconState {
    BOOL minimizedToTray;
    NOTIFYICONDATAA iconData;
} TrayIconState;

/*
 * InteractionState - UI interaction tracking
 */
typedef struct InteractionState {
    BOOL recordingPanelHovered;
    BOOL waitingForHotkey;
    HWND lastHoveredIconBtn;
    HWND lastHoveredCaptureBtn;
} InteractionState;

/* Static instances of consolidated state */
static SelectionUIState g_selection = {0};
static RecordingUIState g_recording = {0};
static OverlayWindowState g_windows = {0};
static TrayIconState g_tray = {0};
static InteractionState g_interaction = {0};

/* Current capture mode selection (simple enough to leave as scalar) */
static CaptureMode g_currentMode = MODE_NONE;

/* Use SELECTION_HANDLE_SIZE from constants.h instead of local define */
#define HANDLE_SIZE SELECTION_HANDLE_SIZE

/* Recording thread */
static DWORD WINAPI RecordingThread(LPVOID param);

/* Window procedures */
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Helper functions */
static HandlePosition HitTestHandle(POINT pt);
static void UpdateActionToolbar(void);
static void UpdateTimerDisplay(void);
static void ShowActionToolbar(BOOL show);
static void CaptureToClipboard(void);
static void CaptureToFile(void);

// System tray functions
static void AddTrayIcon(void);
static void RemoveTrayIcon(void);
static void MinimizeToTray(void);
static void RestoreFromTray(void);

// Action toolbar callbacks
static void ActionToolbar_OnMinimize(void);
static void ActionToolbar_OnRecord(void);
static void ActionToolbar_OnClose(void);
static void ActionToolbar_OnSettings(void);

/**
 * Check for audio encoder errors after ReplayBuffer_Start and notify user.
 * Shows a MessageBox if audio was requested but encoder failed.
 */
static void CheckAudioError(void) {
    if (!g_config.audioEnabled) return;
    
    AACEncoderError err = (AACEncoderError)InterlockedCompareExchange(&g_replayBuffer.audioError, 0, 0);
    if (err == AAC_OK) return;
    
    const char* msg = NULL;
    switch (err) {
        case AAC_ERR_ENCODER_NOT_FOUND:
            msg = "AAC audio encoder not available.\n\n"
                  "Windows Media Foundation AAC encoder is required for audio recording.\n"
                  "Audio will be disabled for this session.";
            break;
        case AAC_ERR_TYPE_NEGOTIATION:
            msg = "AAC encoder failed to initialize (type negotiation error).\n\n"
                  "Audio will be disabled for this session.";
            break;
        case AAC_ERR_START_STREAM:
            msg = "AAC encoder failed to start streaming.\n\n"
                  "Audio will be disabled for this session.";
            break;
        case AAC_ERR_MEMORY:
            msg = "Failed to allocate memory for audio encoder.\n\n"
                  "Audio will be disabled for this session.";
            break;
        default:
            msg = "Audio encoder initialization failed.\n\n"
                  "Audio will be disabled for this session.";
            break;
    }
    
    Logger_Log("Audio encoder error displayed to user: %d\n", (int)err);
    MessageBoxA(NULL, msg, "Audio Error", MB_OK | MB_ICONWARNING);
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
    if (!g_gdip.CreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0) return;
    
    g_gdip.SetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    // Inset by 0.5 to ensure border is fully visible (GDI+ draws strokes centered on path)
    float x = (float)rect->left + 0.5f;
    float y = (float)rect->top + 0.5f;
    float w = (float)(rect->right - rect->left) - 1.0f;
    float h = (float)(rect->bottom - rect->top) - 1.0f;
    float r = (float)radius;
    float d = r * 2.0f;
    
    // Create rounded rectangle path
    GpPath* path = NULL;
    g_gdip.CreatePath(FillModeAlternate, &path);
    
    // Top-left arc
    g_gdip.AddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    // Top-right arc
    g_gdip.AddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    // Bottom-right arc
    g_gdip.AddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    // Bottom-left arc
    g_gdip.AddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    g_gdip.ClosePathFigure(path);
    
    // Fill
    GpSolidFill* brush = NULL;
    g_gdip.CreateSolidFill(ColorRefToARGB(fillColor, 255), &brush);
    g_gdip.FillPath(graphics, brush, path);
    g_gdip.BrushDelete(brush);
    
    // Border
    GpPen* pen = NULL;
    g_gdip.CreatePen1(ColorRefToARGB(borderColor, 255), 1.0f, UnitPixel, &pen);
    g_gdip.DrawPath(graphics, pen, path);
    g_gdip.PenDelete(pen);
    
    g_gdip.DeletePath(path);
    g_gdip.DeleteGraphics(graphics);
}

// Draw anti-aliased filled circle
static void DrawCircleAA(HDC hdc, int cx, int cy, int radius, COLORREF color) {
    if (!g_gdip.CreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0) return;
    
    g_gdip.SetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    GpSolidFill* brush = NULL;
    g_gdip.CreateSolidFill(ColorRefToARGB(color, 255), &brush);
    g_gdip.FillEllipse(graphics, brush, 
                    (float)(cx - radius), (float)(cy - radius), 
                    (float)(radius * 2), (float)(radius * 2));
    g_gdip.BrushDelete(brush);
    g_gdip.DeleteGraphics(graphics);
}

// Apply smooth rounded corners using DWM (Windows 11+)
static void ApplyRoundedCorners(HWND hwnd) {
    DWORD pref = 2;  // DWMWCP_ROUND
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
    if (!hBitmap || !pBits) {
        DeleteDC(memDC);
        ReleaseDC(NULL, screenDC);
        return;
    }
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    // Fill entire overlay with semi-transparent dark (alpha ~100 out of 255)
    int overlayAlpha = 100;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            BYTE* pixel = pBits + (y * width + x) * 4;
            pixel[0] = 0;   // B
            pixel[1] = 0;   // G  
            pixel[2] = 0;   // R
            pixel[3] = (BYTE)overlayAlpha; // A - explicit cast
        }
    }
    
    // If we have a selection (drawing or complete), punch a clear hole
    BOOL hasSelection = !IsRectEmpty(&g_selection.selectedRect) && 
                        (g_selection.state == SEL_DRAWING || g_selection.state == SEL_COMPLETE || 
                         g_selection.state == SEL_MOVING || g_selection.state == SEL_RESIZING);
    
    if (hasSelection) {
        // Convert screen coords to window coords
        int selLeft = g_selection.selectedRect.left - wndRect.left;
        int selTop = g_selection.selectedRect.top - wndRect.top;
        int selRight = g_selection.selectedRect.right - wndRect.left;
        int selBottom = g_selection.selectedRect.bottom - wndRect.top;
        
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
        if (g_selection.state == SEL_COMPLETE || g_selection.state == SEL_MOVING || g_selection.state == SEL_RESIZING) {
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
    if (!g_windows.crosshairWnd) return;
    if (!IsWindowVisible(g_windows.crosshairWnd)) return;
    
    // Get screen bounds to determine corner placement
    RECT screenRect;
    Capture_GetAllMonitorsBounds(&screenRect);
    
    int crossSize = CROSSHAIR_SIZE;
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
    
    SetWindowPos(g_windows.crosshairWnd, HWND_TOPMOST, posX, posY, 
                 crossSize, crossSize, SWP_NOACTIVATE);
    InvalidateRect(g_windows.crosshairWnd, NULL, FALSE);
}

// Hit test for resize handles - returns which handle is under the point
static HandlePosition HitTestHandle(POINT pt) {
    if (IsRectEmpty(&g_selection.selectedRect)) return HANDLE_NONE;
    
    int hs = HANDLE_SIZE;
    int cx = (g_selection.selectedRect.left + g_selection.selectedRect.right) / 2;
    int cy = (g_selection.selectedRect.top + g_selection.selectedRect.bottom) / 2;
    
    // Check corner handles first (higher priority)
    RECT handleRect;
    
    // Top-left
    SetRect(&handleRect, g_selection.selectedRect.left - hs, g_selection.selectedRect.top - hs, 
            g_selection.selectedRect.left + hs, g_selection.selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_TL;
    
    // Top-right
    SetRect(&handleRect, g_selection.selectedRect.right - hs, g_selection.selectedRect.top - hs,
            g_selection.selectedRect.right + hs, g_selection.selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_TR;
    
    // Bottom-left
    SetRect(&handleRect, g_selection.selectedRect.left - hs, g_selection.selectedRect.bottom - hs,
            g_selection.selectedRect.left + hs, g_selection.selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_BL;
    
    // Bottom-right
    SetRect(&handleRect, g_selection.selectedRect.right - hs, g_selection.selectedRect.bottom - hs,
            g_selection.selectedRect.right + hs, g_selection.selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_BR;
    
    // Edge handles
    // Top
    SetRect(&handleRect, cx - hs, g_selection.selectedRect.top - hs, cx + hs, g_selection.selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_T;
    
    // Bottom
    SetRect(&handleRect, cx - hs, g_selection.selectedRect.bottom - hs, cx + hs, g_selection.selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_B;
    
    // Left
    SetRect(&handleRect, g_selection.selectedRect.left - hs, cy - hs, g_selection.selectedRect.left + hs, cy + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_L;
    
    // Right
    SetRect(&handleRect, g_selection.selectedRect.right - hs, cy - hs, g_selection.selectedRect.right + hs, cy + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_R;
    
    return HANDLE_NONE;
}

// Check if point is inside the selection (for moving)
static BOOL PtInSelection(POINT pt) {
    return PtInRect(&g_selection.selectedRect, pt);
}

// Check if point is on the selection border (for moving with border hover)
static BOOL PtOnSelectionBorder(POINT pt) {
    if (IsRectEmpty(&g_selection.selectedRect)) return FALSE;
    
    int borderWidth = 8; // Width of the border hit zone
    
    // Create outer and inner rects
    RECT outer = g_selection.selectedRect;
    InflateRect(&outer, borderWidth, borderWidth);
    
    RECT inner = g_selection.selectedRect;
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
    if (show && !IsRectEmpty(&g_selection.selectedRect)) {
        int cx = (g_selection.selectedRect.left + g_selection.selectedRect.right) / 2;
        int posY = g_selection.selectedRect.bottom + 10;
        
        // Check if it would go off screen
        RECT screenRect;
        Capture_GetAllMonitorsBounds(&screenRect);
        if (posY + 40 > screenRect.bottom - 20) {
            posY = g_selection.selectedRect.top - 40 - 10;
        }
        
        ActionToolbar_Show(cx, posY);
    } else {
        ActionToolbar_Hide();
    }
}

// Update the action toolbar position
static void UpdateActionToolbar(void) {
    ShowActionToolbar(g_selection.state == SEL_COMPLETE);
}

// Capture screen region to clipboard
static void CaptureToClipboard(void) {
    if (IsRectEmpty(&g_selection.selectedRect)) return;
    
    int w = g_selection.selectedRect.right - g_selection.selectedRect.left;
    int h = g_selection.selectedRect.bottom - g_selection.selectedRect.top;
    
    // Hide overlay temporarily and wait for redraw
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    /* Force windows to repaint the area we just revealed, then wait for
     * the window manager to complete the operation. RedrawWindow with
     * RDW_UPDATENOW ensures synchronous repaint; the Sleep gives the
     * compositor time to finish any animations. */
    RedrawWindow(NULL, &g_selection.selectedRect, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(OVERLAY_HIDE_SETTLE_MS);
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    BitBlt(memDC, 0, 0, w, h, screenDC, g_selection.selectedRect.left, g_selection.selectedRect.top, SRCCOPY);
    
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
    g_selection.state = SEL_NONE;
    SetRectEmpty(&g_selection.selectedRect);
    InterlockedExchange(&g_isSelecting, FALSE);
    ShowWindow(g_controlWnd, SW_SHOW);
}

// Capture screen region to file (Save As dialog)
static void CaptureToFile(void) {
    if (IsRectEmpty(&g_selection.selectedRect)) return;
    
    int w = g_selection.selectedRect.right - g_selection.selectedRect.left;
    int h = g_selection.selectedRect.bottom - g_selection.selectedRect.top;
    
    // Hide overlay temporarily and wait for redraw
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    /* Force windows to repaint the area we just revealed, then wait for
     * the window manager to complete the operation. */
    RedrawWindow(NULL, &g_selection.selectedRect, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(OVERLAY_HIDE_SETTLE_MS);
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    BitBlt(memDC, 0, 0, w, h, screenDC, g_selection.selectedRect.left, g_selection.selectedRect.top, SRCCOPY);
    
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
    
    BOOL clipboardTookOwnership = FALSE;
    if (GetSaveFileNameA(&ofn)) {
        // TODO: Save as PNG (requires GDI+ or other library)
        // For now, just show success message
        MessageBoxA(NULL, "Save functionality requires PNG encoder.\nBitmap captured to clipboard instead.", 
                    "Save", MB_OK | MB_ICONINFORMATION);
        
        // Copy to clipboard as fallback
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            if (SetClipboardData(CF_BITMAP, hBitmap)) {
                clipboardTookOwnership = TRUE;  // Clipboard now owns the bitmap
            }
            CloseClipboard();
        }
    }
    
    // Only delete bitmap if clipboard didn't take ownership
    if (!clipboardTookOwnership) {
        DeleteObject(hBitmap);
    }
    
    // Clear selection state - overlay stays hidden, show control panel
    g_selection.state = SEL_NONE;
    SetRectEmpty(&g_selection.selectedRect);
    InterlockedExchange(&g_isSelecting, FALSE);
    ShowWindow(g_controlWnd, SW_SHOW);
}

// ============================================================================
// System Tray Functions
// ============================================================================

// Load icon from ICO file
static HICON LoadIconFromICO(const char* filename) {
    int iconWidth = GetSystemMetrics(SM_CXSMICON);
    int iconHeight = GetSystemMetrics(SM_CYSMICON);
    
    HICON hIcon = (HICON)LoadImageA(NULL, filename, IMAGE_ICON, 
                                     iconWidth, iconHeight, LR_LOADFROMFILE);
    return hIcon;
}

// Load icon from PNG file using GDI+ and scale to proper tray icon size
static HICON LoadIconFromPNG(const char* filename) {
    if (!g_gdip.CreateFromHDC) return NULL;  // GDI+ not loaded
    
    // Convert filename to wide string
    WCHAR wFilename[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, filename, -1, wFilename, MAX_PATH);
    
    // Load GDI+ image functions
    typedef int (WINAPI *GdipLoadImageFromFileFunc)(const WCHAR*, void**);
    typedef int (WINAPI *GdipCreateHBITMAPFromBitmapFunc)(void*, HBITMAP*, DWORD);
    typedef int (WINAPI *GdipGetImageWidthFunc)(void*, UINT*);
    typedef int (WINAPI *GdipGetImageHeightFunc)(void*, UINT*);
    typedef int (WINAPI *GdipDisposeImageFunc)(void*);
    typedef int (WINAPI *GdipGetImageThumbnailFunc)(void*, UINT, UINT, void**, void*, void*);
    
    GdipLoadImageFromFileFunc pGdipLoadImageFromFile = 
        (GdipLoadImageFromFileFunc)GetProcAddress(g_gdip.module, "GdipLoadImageFromFile");
    GdipCreateHBITMAPFromBitmapFunc pGdipCreateHBITMAPFromBitmap = 
        (GdipCreateHBITMAPFromBitmapFunc)GetProcAddress(g_gdip.module, "GdipCreateHBITMAPFromBitmap");
    GdipDisposeImageFunc pGdipDisposeImage = 
        (GdipDisposeImageFunc)GetProcAddress(g_gdip.module, "GdipDisposeImage");
    GdipGetImageThumbnailFunc pGdipGetImageThumbnail =
        (GdipGetImageThumbnailFunc)GetProcAddress(g_gdip.module, "GdipGetImageThumbnail");
    
    // Suppress unused warnings for functions we may use later
    (void)pGdipGetImageThumbnail;
    
    if (!pGdipLoadImageFromFile || !pGdipCreateHBITMAPFromBitmap || !pGdipDisposeImage) {
        return NULL;
    }
    
    void* image = NULL;
    if (pGdipLoadImageFromFile(wFilename, &image) != 0 || !image) {
        return NULL;
    }
    
    // Get system tray icon size (typically 16x16 or scaled for DPI)
    int iconWidth = GetSystemMetrics(SM_CXSMICON);
    int iconHeight = GetSystemMetrics(SM_CYSMICON);
    
    // Scale image to proper tray icon size using thumbnail
    void* scaledImage = NULL;
    if (pGdipGetImageThumbnail) {
        if (pGdipGetImageThumbnail(image, iconWidth, iconHeight, &scaledImage, NULL, NULL) == 0 && scaledImage) {
            pGdipDisposeImage(image);
            image = scaledImage;
        }
    }
    
    // Create HBITMAP from image
    HBITMAP hBitmap = NULL;
    pGdipCreateHBITMAPFromBitmap(image, &hBitmap, 0);
    pGdipDisposeImage(image);
    
    if (!hBitmap) return NULL;
    
    // Create icon from bitmap at proper size
    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmMask = CreateBitmap(iconWidth, iconHeight, 1, 1, NULL);
    ii.hbmColor = hBitmap;
    
    HICON hIcon = CreateIconIndirect(&ii);
    
    DeleteObject(ii.hbmMask);
    DeleteObject(hBitmap);
    
    return hIcon;
}

static HICON g_trayHIcon = NULL;  // Keep track of custom icon for cleanup
static void* g_settingsImage = NULL;  // GDI+ image for settings icon

// Load PNG file as GDI+ image (returns void* that is a GpImage*)
static void* LoadPNGImage(const char* filename) {
    if (!g_gdip.module) return NULL;
    
    WCHAR wFilename[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, filename, -1, wFilename, MAX_PATH);
    
    typedef int (WINAPI *GdipLoadImageFromFileFunc)(const WCHAR*, void**);
    GdipLoadImageFromFileFunc pGdipLoadImageFromFile = 
        (GdipLoadImageFromFileFunc)GetProcAddress(g_gdip.module, "GdipLoadImageFromFile");
    
    if (!pGdipLoadImageFromFile) return NULL;
    
    void* image = NULL;
    if (pGdipLoadImageFromFile(wFilename, &image) != 0) {
        return NULL;
    }
    return image;
}

// Draw GDI+ image to HDC at specified rectangle with proper alpha blending
static void DrawPNGImage(HDC hdc, void* image, int x, int y, int width, int height) {
    if (!image || !g_gdip.CreateFromHDC) return;
    
    typedef int (WINAPI *GdipDrawImageRectIFunc)(void*, void*, int, int, int, int);
    typedef int (WINAPI *GdipSetCompositingModeFunc)(void*, int);
    typedef int (WINAPI *GdipSetCompositingQualityFunc)(void*, int);
    typedef int (WINAPI *GdipSetInterpolationModeFunc)(void*, int);
    typedef int (WINAPI *GdipSetPixelOffsetModeFunc)(void*, int);
    
    GdipDrawImageRectIFunc pGdipDrawImageRectI = 
        (GdipDrawImageRectIFunc)GetProcAddress(g_gdip.module, "GdipDrawImageRectI");
    GdipSetCompositingModeFunc pGdipSetCompositingMode =
        (GdipSetCompositingModeFunc)GetProcAddress(g_gdip.module, "GdipSetCompositingMode");
    GdipSetCompositingQualityFunc pGdipSetCompositingQuality =
        (GdipSetCompositingQualityFunc)GetProcAddress(g_gdip.module, "GdipSetCompositingQuality");
    GdipSetInterpolationModeFunc pGdipSetInterpolationMode = 
        (GdipSetInterpolationModeFunc)GetProcAddress(g_gdip.module, "GdipSetInterpolationMode");
    GdipSetPixelOffsetModeFunc pGdipSetPixelOffsetMode =
        (GdipSetPixelOffsetModeFunc)GetProcAddress(g_gdip.module, "GdipSetPixelOffsetMode");
    
    if (!pGdipDrawImageRectI) return;
    
    GpGraphics* graphics = NULL;
    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0 || !graphics) return;
    
    // Set compositing mode to SourceOver for proper alpha blending
    if (pGdipSetCompositingMode) {
        pGdipSetCompositingMode(graphics, 0);  // CompositingModeSourceOver
    }
    
    // Set high quality compositing
    if (pGdipSetCompositingQuality) {
        pGdipSetCompositingQuality(graphics, 4);  // CompositingQualityHighQuality
    }
    
    // Set interpolation mode for smooth scaling
    if (pGdipSetInterpolationMode) {
        pGdipSetInterpolationMode(graphics, 7);  // InterpolationModeHighQualityBicubic
    }
    
    // Set pixel offset mode for better rendering
    if (pGdipSetPixelOffsetMode) {
        pGdipSetPixelOffsetMode(graphics, 4);  // PixelOffsetModeHighQuality
    }
    
    pGdipDrawImageRectI(graphics, image, x, y, width, height);
    g_gdip.DeleteGraphics(graphics);
}

// Free GDI+ image
static void FreePNGImage(void* image) {
    if (!image) return;
    typedef int (WINAPI *GdipDisposeImageFunc)(void*);
    GdipDisposeImageFunc pGdipDisposeImage = 
        (GdipDisposeImageFunc)GetProcAddress(g_gdip.module, "GdipDisposeImage");
    if (pGdipDisposeImage) pGdipDisposeImage(image);
}

// Load settings icon on startup
static void LoadSettingsIcon(void) {
    g_settingsImage = LoadPNGImage("static\\settings.png");
    if (!g_settingsImage) {
        // Try relative to executable
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            size_t remaining = sizeof(exePath) - (lastSlash + 1 - exePath);
            strncpy(lastSlash + 1, "..\\static\\settings.png", remaining - 1);
            exePath[sizeof(exePath) - 1] = '\0';
            g_settingsImage = LoadPNGImage(exePath);
        }
    }
}

static void AddTrayIcon(void) {
    g_tray.iconData.cbSize = sizeof(NOTIFYICONDATAA);
    g_tray.iconData.hWnd = g_controlWnd;
    g_tray.iconData.uID = 1;
    g_tray.iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.iconData.uCallbackMessage = WM_TRAYICON;
    
    // Try to load custom icon from static folder (prefer .ico for best quality)
    g_trayHIcon = LoadIconFromICO("static\\lwsr_icon.ico");
    if (!g_trayHIcon) {
        // Try relative to executable
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            size_t remaining = sizeof(exePath) - (lastSlash + 1 - exePath);
            strncpy(lastSlash + 1, "..\\static\\lwsr_icon.ico", remaining - 1);
            exePath[sizeof(exePath) - 1] = '\0';
            g_trayHIcon = LoadIconFromICO(exePath);
        }
    }
    
    g_tray.iconData.hIcon = g_trayHIcon ? g_trayHIcon : LoadIcon(NULL, IDI_APPLICATION);
    strncpy(g_tray.iconData.szTip, "LWSR - Screen Recorder", sizeof(g_tray.iconData.szTip) - 1);
    g_tray.iconData.szTip[sizeof(g_tray.iconData.szTip) - 1] = '\0';
    Shell_NotifyIconA(NIM_ADD, &g_tray.iconData);
}

static void RemoveTrayIcon(void) {
    Shell_NotifyIconA(NIM_DELETE, &g_tray.iconData);
    if (g_trayHIcon) {
        DestroyIcon(g_trayHIcon);
        g_trayHIcon = NULL;
    }
}

static void MinimizeToTray(void) {
    if (g_tray.minimizedToTray) return;
    
    // Hide all windows
    ShowWindow(g_controlWnd, SW_HIDE);
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    if (g_windows.settingsWnd) ShowWindow(g_windows.settingsWnd, SW_HIDE);
    
    g_tray.minimizedToTray = TRUE;
}

static void RestoreFromTray(void) {
    if (!g_tray.minimizedToTray) return;
    
    // Show control panel
    ShowWindow(g_controlWnd, SW_SHOW);
    SetForegroundWindow(g_controlWnd);
    
    g_tray.minimizedToTray = FALSE;
}

// ============================================================================
// Action Toolbar Callbacks
// ============================================================================

static void ActionToolbar_OnMinimize(void) {
    // Hide toolbar and selection, minimize to tray
    ActionToolbar_Hide();
    ShowWindow(g_overlayWnd, SW_HIDE);
    g_selection.state = SEL_NONE;
    SetRectEmpty(&g_selection.selectedRect);
    InterlockedExchange(&g_isSelecting, FALSE);
    MinimizeToTray();
}

static void ActionToolbar_OnRecord(void) {
    Recording_Start();
}

static void ActionToolbar_OnClose(void) {
    // Cancel selection and return to control panel
    ActionToolbar_Hide();
    ShowWindow(g_overlayWnd, SW_HIDE);
    g_selection.state = SEL_NONE;
    SetRectEmpty(&g_selection.selectedRect);
    InterlockedExchange(&g_isSelecting, FALSE);
    ShowWindow(g_controlWnd, SW_SHOW);
}

static void ActionToolbar_OnSettings(void) {
    // Hide toolbar and selection, show settings
    ActionToolbar_Hide();
    ShowWindow(g_overlayWnd, SW_HIDE);
    g_selection.state = SEL_NONE;
    SetRectEmpty(&g_selection.selectedRect);
    InterlockedExchange(&g_isSelecting, FALSE);
    
    // Open settings window centered on control panel
    if (g_windows.settingsWnd) {
        SendMessage(g_windows.settingsWnd, WM_CLOSE, 0, 0);
    }
    
    ShowWindow(g_controlWnd, SW_SHOW);
    
    RECT ctrlRect;
    GetWindowRect(g_controlWnd, &ctrlRect);
    int ctrlCenterX = (ctrlRect.left + ctrlRect.right) / 2;
    
    g_windows.settingsWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "LWSRSettings",
        NULL,
        WS_POPUP | WS_VISIBLE | WS_BORDER,
        ctrlCenterX - SETTINGS_WIDTH / 2, ctrlRect.bottom + 5,
        SETTINGS_WIDTH, SETTINGS_HEIGHT,
        g_controlWnd, NULL, g_windows.hInstance, NULL
    );
}

// Timer text for display
static char g_timerText[32] = "00:00";

// Update timer display text
static void UpdateTimerDisplay(void) {
    // Thread-safe check
    if (!g_windows.recordingPanel || !InterlockedCompareExchange(&g_isRecording, 0, 0)) return;
    
    // Update timer text - use GetTickCount64 to avoid overflow issues
    ULONGLONG elapsed = GetTickCount64() - g_recording.startTime;
    int secs = (int)((elapsed / 1000) % 60);
    int mins = (int)((elapsed / 60000) % 60);
    int hours = (int)(elapsed / 3600000);
    
    if (hours > 0) {
        snprintf(g_timerText, sizeof(g_timerText), "%d:%02d:%02d", hours, mins, secs);
    } else {
        snprintf(g_timerText, sizeof(g_timerText), "%02d:%02d", mins, secs);  // MM:SS with leading zero
    }
    
    // Trigger repaint of recording panel
    InvalidateRect(g_windows.recordingPanel, NULL, FALSE);
}

BOOL Overlay_Create(HINSTANCE hInstance) {
    if (!hInstance) return FALSE;
    
    g_windows.hInstance = hInstance;
    
    // GDI+ is now initialized globally via g_gdip in main.c
    
    // Load settings icon
    LoadSettingsIcon();
    
    // Initialize common controls (including trackbar)
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
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
    // Load icons from EXE resource at correct sizes for taskbar
    wcSettings.hIcon = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, 
                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wcSettings.hIconSm = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
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
    ActionToolbar_SetCallbacks(ActionToolbar_OnMinimize, ActionToolbar_OnRecord, 
                               ActionToolbar_OnClose, ActionToolbar_OnSettings);
    
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
    
    if (!g_overlayWnd) {
        // Clean up settings icon on failure
        if (g_settingsImage) {
            FreePNGImage(g_settingsImage);
            g_settingsImage = NULL;
        }
        return FALSE;
    }
    
    // Initial overlay bitmap will be set by UpdateOverlayBitmap when mode is selected
    
    // Create control panel (top center) - Windows 11 Snipping Tool style
    POINT center;
    GetPrimaryMonitorCenter(&center);
    
    int ctrlWidth = CONTROL_PANEL_WIDTH;
    int ctrlHeight = CONTROL_PANEL_HEIGHT;
    
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
        g_overlayWnd = NULL;
        // Clean up settings icon on failure
        if (g_settingsImage) {
            FreePNGImage(g_settingsImage);
            g_settingsImage = NULL;
        }
        return FALSE;
    }
    
    // Apply smooth rounded corners using DWM (Windows 11+)
    ApplyRoundedCorners(g_controlWnd);
    
    // Create crosshair indicator window
    g_windows.crosshairWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "LWSRCrosshair",
        NULL,
        WS_POPUP,
        -9999, -9999, CROSSHAIR_SIZE, CROSSHAIR_SIZE,  // Start off-screen
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_windows.crosshairWnd) {
        DestroyWindow(g_controlWnd);
        g_controlWnd = NULL;
        DestroyWindow(g_overlayWnd);
        g_overlayWnd = NULL;
        if (g_settingsImage) {
            FreePNGImage(g_settingsImage);
            g_settingsImage = NULL;
        }
        return FALSE;
    }
    
    SetLayeredWindowAttributes(g_windows.crosshairWnd, RGB(0, 0, 0), 200, LWA_ALPHA);
    
    // Action toolbar is now managed by action_toolbar module
    
    // Don't show overlay or crosshair initially - only when mode is selected
    InterlockedExchange(&g_isSelecting, FALSE);
    g_selection.state = SEL_NONE;
    
    // Only show the control panel at startup
    UpdateWindow(g_controlWnd);
    
    // Add system tray icon (always visible)
    AddTrayIcon();
    
    return TRUE;
}

void Overlay_Destroy(void) {
    // Remove tray icon
    RemoveTrayIcon();
    // Thread-safe check
    if (InterlockedCompareExchange(&g_isRecording, 0, 0)) {
        Recording_Stop();
    }
    
    // Free settings icon (GDI+ shutdown handled by main.c)
    if (g_settingsImage) {
        FreePNGImage(g_settingsImage);
        g_settingsImage = NULL;
    }
    
    // GDI+ is now shut down globally via g_gdip in main.c
    
    if (g_windows.crosshairWnd) {
        DestroyWindow(g_windows.crosshairWnd);
        g_windows.crosshairWnd = NULL;
    }
    
    if (g_windows.settingsWnd) {
        DestroyWindow(g_windows.settingsWnd);
        g_windows.settingsWnd = NULL;
    }
    
    // Shutdown action toolbar module
    ActionToolbar_Shutdown();
    
    if (g_windows.recordingPanel) {
        DestroyWindow(g_windows.recordingPanel);
        g_windows.recordingPanel = NULL;
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
    InterlockedExchange(&g_isSelecting, TRUE);
    g_selection.state = SEL_NONE;
    SetRectEmpty(&g_selection.selectedRect);
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
    if (!region) return FALSE;
    if (IsRectEmpty(&g_selection.selectedRect)) return FALSE;
    *region = g_selection.selectedRect;
    return TRUE;
}

HWND Overlay_GetWindow(void) {
    return g_overlayWnd;
}

void Recording_Start(void) {
    // Thread-safe check: read g_isRecording atomically
    if (InterlockedCompareExchange(&g_isRecording, 0, 0)) return;
    if (IsRectEmpty(&g_selection.selectedRect)) return;
    
    // Set capture region
    if (!Capture_SetRegion(&g_capture, g_selection.selectedRect)) {
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
    if (!Encoder_Init(&g_recording.encoder, outputPath, 
                      g_capture.captureWidth, g_capture.captureHeight,
                      fps, g_config.outputFormat, g_config.quality)) {
        char errMsg[512];
        snprintf(errMsg, sizeof(errMsg), 
            "Failed to initialize encoder.\nPath: %s\nSize: %dx%d\nFPS: %d",
            outputPath, g_capture.captureWidth, g_capture.captureHeight, fps);
        MessageBoxA(NULL, errMsg, "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Hide selection UI (but keep control bar visible)
    ShowWindow(g_overlayWnd, SW_HIDE);
    ShowWindow(g_windows.crosshairWnd, SW_HIDE);
    ActionToolbar_Hide();
    
    // Start recording - use atomic operations for thread safety
    InterlockedExchange(&g_isRecording, TRUE);
    InterlockedExchange(&g_isSelecting, FALSE);
    InterlockedExchange(&g_recording.stopRecording, FALSE);
    g_recording.startTime = GetTickCount64();
    g_recording.recordingMode = g_currentMode;  // Remember which mode started recording
    strncpy(g_timerText, "00:00", sizeof(g_timerText) - 1);
    g_timerText[sizeof(g_timerText) - 1] = '\0';
    
    // Show recording border if enabled
    if (g_config.showRecordingBorder) {
        Border_Show(g_selection.selectedRect);
    }
    
    // Update control panel to show inline timer/stop
    Overlay_SetRecordingState(TRUE);
    
    // Start recording thread
    g_recording.thread = CreateThread(NULL, 0, RecordingThread, NULL, 0, NULL);
    if (!g_recording.thread) {
        Logger_Log("Recording_Start: CreateThread failed\n");
        Encoder_Finalize(&g_recording.encoder);  // Clean up encoder on thread failure
        InterlockedExchange(&g_isRecording, FALSE);
        Overlay_SetRecordingState(FALSE);
        Border_Hide();
        return;
    }
    
    // Start time limit timer if configured
    if (g_config.maxRecordingSeconds > 0) {
        SetTimer(g_controlWnd, ID_TIMER_LIMIT, 
                 g_config.maxRecordingSeconds * 1000, NULL);
    }
}

void Recording_Stop(void) {
    // Thread-safe check: read g_isRecording atomically
    if (!InterlockedCompareExchange(&g_isRecording, 0, 0)) return;
    
    // Signal recording thread to stop (atomic write with memory barrier)
    InterlockedExchange(&g_recording.stopRecording, TRUE);
    
    // Wait for recording thread
    if (g_recording.thread) {
        WaitForSingleObject(g_recording.thread, 5000);
        CloseHandle(g_recording.thread);
        g_recording.thread = NULL;
    }
    
    // Finalize encoder
    Encoder_Finalize(&g_recording.encoder);
    
    // Thread-safe: signal recording stopped
    InterlockedExchange(&g_isRecording, FALSE);
    
    // Hide recording border
    Border_Hide();
    
    // Stop timers
    KillTimer(g_controlWnd, ID_TIMER_DISPLAY);
    KillTimer(g_controlWnd, ID_TIMER_LIMIT);
    
    // Restore control bar to normal state
    Overlay_SetRecordingState(FALSE);
    
    // Save config with last capture rect
    g_config.lastCaptureRect = g_selection.selectedRect;
    g_config.lastMode = g_currentMode;
    Config_Save(&g_config);
    
    // Show control bar
    ShowWindow(g_controlWnd, SW_SHOW);
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
    
    // Thread-safe loop: read stop flag atomically
    while (!InterlockedCompareExchange(&g_recording.stopRecording, 0, 0)) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        double targetTime = frameCount * frameIntervalSec;
        
        if (elapsed >= targetTime) {
            // Use frame-based timestamp for smooth playback
            UINT64 timestamp = frameCount * frameDuration100ns;
            BYTE* frame = Capture_GetFrame(&g_capture, NULL); // Ignore DXGI timestamp
            
            if (frame) {
                Encoder_WriteFrame(&g_recording.encoder, frame, timestamp);
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
    if (isRecording) {
        // Get button positions based on recording mode
        HWND modeBtn = NULL;
        RECT btnRect = {0};
        
        switch (g_recording.recordingMode) {
            case MODE_AREA:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_AREA);
                break;
            case MODE_WINDOW:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_WINDOW);
                break;
            case MODE_MONITOR:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_MONITOR);
                break;
            case MODE_ALL_MONITORS:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_ALL);
                break;
            default:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_AREA);
                break;
        }
        
        if (modeBtn) {
            GetWindowRect(modeBtn, &btnRect);
            MapWindowPoints(HWND_DESKTOP, g_controlWnd, (LPPOINT)&btnRect, 2);
        }
        
        // Hide the active mode button
        if (modeBtn) ShowWindow(modeBtn, SW_HIDE);
        
        // Disable (gray out) other mode buttons
        HWND btnArea = GetDlgItem(g_controlWnd, ID_MODE_AREA);
        HWND btnWindow = GetDlgItem(g_controlWnd, ID_MODE_WINDOW);
        HWND btnMonitor = GetDlgItem(g_controlWnd, ID_MODE_MONITOR);
        HWND btnAll = GetDlgItem(g_controlWnd, ID_MODE_ALL);
        
        if (btnArea != modeBtn) EnableWindow(btnArea, FALSE);
        if (btnWindow != modeBtn) EnableWindow(btnWindow, FALSE);
        if (btnMonitor != modeBtn) EnableWindow(btnMonitor, FALSE);
        if (btnAll != modeBtn) EnableWindow(btnAll, FALSE);
        
        // Invalidate disabled buttons to show grayed state
        InvalidateRect(btnArea, NULL, TRUE);
        InvalidateRect(btnWindow, NULL, TRUE);
        InvalidateRect(btnMonitor, NULL, TRUE);
        InvalidateRect(btnAll, NULL, TRUE);
        
        // Create recording panel in place of the mode button
        if (!g_windows.recordingPanel) {
            g_windows.recordingPanel = CreateWindowW(L"BUTTON", L"",
                WS_CHILD | BS_OWNERDRAW,
                btnRect.left, btnRect.top, 
                btnRect.right - btnRect.left, btnRect.bottom - btnRect.top,
                g_controlWnd, (HMENU)ID_RECORDING_PANEL, g_windows.hInstance, NULL);
        } else {
            SetWindowPos(g_windows.recordingPanel, NULL,
                btnRect.left, btnRect.top, 
                btnRect.right - btnRect.left, btnRect.bottom - btnRect.top,
                SWP_NOZORDER);
        }
        ShowWindow(g_windows.recordingPanel, SW_SHOW);
        
        // Start timer for display updates
        SetTimer(g_controlWnd, ID_TIMER_DISPLAY, 1000, NULL);
        
        // Keep control bar visible but ensure it's on top
        ShowWindow(g_controlWnd, SW_SHOW);
        SetWindowPos(g_controlWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        // Stop timer
        KillTimer(g_controlWnd, ID_TIMER_DISPLAY);
        
        // Hide recording panel
        if (g_windows.recordingPanel) {
            ShowWindow(g_windows.recordingPanel, SW_HIDE);
        }
        
        // Re-enable and show all mode buttons
        HWND btnArea = GetDlgItem(g_controlWnd, ID_MODE_AREA);
        HWND btnWindow = GetDlgItem(g_controlWnd, ID_MODE_WINDOW);
        HWND btnMonitor = GetDlgItem(g_controlWnd, ID_MODE_MONITOR);
        HWND btnAll = GetDlgItem(g_controlWnd, ID_MODE_ALL);
        
        EnableWindow(btnArea, TRUE);
        EnableWindow(btnWindow, TRUE);
        EnableWindow(btnMonitor, TRUE);
        EnableWindow(btnAll, TRUE);
        
        ShowWindow(btnArea, SW_SHOW);
        ShowWindow(btnWindow, SW_SHOW);
        ShowWindow(btnMonitor, SW_SHOW);
        ShowWindow(btnAll, SW_SHOW);
        
        InvalidateRect(btnArea, NULL, TRUE);
        InvalidateRect(btnWindow, NULL, TRUE);
        InvalidateRect(btnMonitor, NULL, TRUE);
        InvalidateRect(btnAll, NULL, TRUE);
        
        g_recording.recordingMode = MODE_NONE;
    }
}

// Overlay window procedure - handles selection
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_USER + 1: // Stop recording signal from second instance
            if (InterlockedCompareExchange(&g_isRecording, 0, 0)) {
                Recording_Stop();
            } else {
                PostQuitMessage(0);
            }
            return 0;
        
        case WM_SETCURSOR: {
            // Cursor depends on state and what's under the mouse
            POINT pt;
            GetCursorPos(&pt);
            
            if (g_selection.state == SEL_COMPLETE) {
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
                if (g_selection.state == SEL_COMPLETE) {
                    // Check if clicking on a handle
                    HandlePosition handle = HitTestHandle(pt);
                    if (handle != HANDLE_NONE) {
                        g_selection.state = SEL_RESIZING;
                        g_selection.activeHandle = handle;
                        g_selection.originalRect = g_selection.selectedRect;
                        g_selection.moveStart = pt;
                        SetCapture(hwnd);
                        ShowActionToolbar(FALSE);
                        return 0;
                    }
                    
                    // Check if clicking inside selection OR on border (move)
                    if (PtOnSelectionBorder(pt) || PtInSelection(pt)) {
                        g_selection.state = SEL_MOVING;
                        g_selection.originalRect = g_selection.selectedRect;
                        g_selection.moveStart = pt;
                        SetCapture(hwnd);
                        ShowActionToolbar(FALSE);
                        return 0;
                    }
                    
                    // Clicking outside - start new selection
                    g_selection.state = SEL_NONE;
                    SetRectEmpty(&g_selection.selectedRect);
                    ShowActionToolbar(FALSE);
                }
                
                // Start drawing new selection
                g_selection.state = SEL_DRAWING;
                g_selection.dragStart = pt;
                g_selection.dragEnd = pt;
                SetCapture(hwnd);
            }
            return 0;
        }
            
        case WM_MOUSEMOVE: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_selection.state == SEL_DRAWING) {
                g_selection.dragEnd = pt;
                
                // Update selection rect
                g_selection.selectedRect.left = min(g_selection.dragStart.x, g_selection.dragEnd.x);
                g_selection.selectedRect.top = min(g_selection.dragStart.y, g_selection.dragEnd.y);
                g_selection.selectedRect.right = max(g_selection.dragStart.x, g_selection.dragEnd.x);
                g_selection.selectedRect.bottom = max(g_selection.dragStart.y, g_selection.dragEnd.y);
                
                UpdateOverlayBitmap();
            } else if (g_selection.state == SEL_MOVING) {
                int dx = pt.x - g_selection.moveStart.x;
                int dy = pt.y - g_selection.moveStart.y;
                
                g_selection.selectedRect.left = g_selection.originalRect.left + dx;
                g_selection.selectedRect.top = g_selection.originalRect.top + dy;
                g_selection.selectedRect.right = g_selection.originalRect.right + dx;
                g_selection.selectedRect.bottom = g_selection.originalRect.bottom + dy;
                
                UpdateOverlayBitmap();
            } else if (g_selection.state == SEL_RESIZING) {
                int dx = pt.x - g_selection.moveStart.x;
                int dy = pt.y - g_selection.moveStart.y;
                
                g_selection.selectedRect = g_selection.originalRect;
                
                // Apply resize based on active handle
                switch (g_selection.activeHandle) {
                    case HANDLE_TL:
                        g_selection.selectedRect.left += dx;
                        g_selection.selectedRect.top += dy;
                        break;
                    case HANDLE_T:
                        g_selection.selectedRect.top += dy;
                        break;
                    case HANDLE_TR:
                        g_selection.selectedRect.right += dx;
                        g_selection.selectedRect.top += dy;
                        break;
                    case HANDLE_L:
                        g_selection.selectedRect.left += dx;
                        break;
                    case HANDLE_R:
                        g_selection.selectedRect.right += dx;
                        break;
                    case HANDLE_BL:
                        g_selection.selectedRect.left += dx;
                        g_selection.selectedRect.bottom += dy;
                        break;
                    case HANDLE_B:
                        g_selection.selectedRect.bottom += dy;
                        break;
                    case HANDLE_BR:
                        g_selection.selectedRect.right += dx;
                        g_selection.selectedRect.bottom += dy;
                        break;
                    default:
                        break;
                }
                
                // Normalize rect (ensure left < right, top < bottom)
                if (g_selection.selectedRect.left > g_selection.selectedRect.right) {
                    int tmp = g_selection.selectedRect.left;
                    g_selection.selectedRect.left = g_selection.selectedRect.right;
                    g_selection.selectedRect.right = tmp;
                }
                if (g_selection.selectedRect.top > g_selection.selectedRect.bottom) {
                    int tmp = g_selection.selectedRect.top;
                    g_selection.selectedRect.top = g_selection.selectedRect.bottom;
                    g_selection.selectedRect.bottom = tmp;
                }
                
                UpdateOverlayBitmap();
            } else if (InterlockedCompareExchange(&g_isSelecting, 0, 0) && g_selection.state == SEL_NONE) {
                // Just moving mouse over overlay, cursor handles itself
            }
            return 0;
        }
            
        case WM_LBUTTONUP: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_selection.state == SEL_DRAWING) {
                ReleaseCapture();
                
                int width = g_selection.selectedRect.right - g_selection.selectedRect.left;
                int height = g_selection.selectedRect.bottom - g_selection.selectedRect.top;
                
                if (width >= 10 && height >= 10) {
                    // Selection complete - show handles and action toolbar
                    g_selection.state = SEL_COMPLETE;
                    UpdateOverlayBitmap();
                    ShowActionToolbar(TRUE);
                } else {
                    // Too small - reset
                    g_selection.state = SEL_NONE;
                    SetRectEmpty(&g_selection.selectedRect);
                    UpdateOverlayBitmap();
                }
            } else if (g_selection.state == SEL_MOVING || g_selection.state == SEL_RESIZING) {
                ReleaseCapture();
                g_selection.state = SEL_COMPLETE;
                g_selection.activeHandle = HANDLE_NONE;
                UpdateOverlayBitmap();
                ShowActionToolbar(TRUE);
            } else if (InterlockedCompareExchange(&g_isSelecting, 0, 0) && g_currentMode != MODE_AREA) {
                // Window/Monitor click mode
                if (g_currentMode == MODE_WINDOW) {
                    HWND targetWnd = WindowFromPoint(pt);
                    if (targetWnd) {
                        HWND topLevel = GetAncestor(targetWnd, GA_ROOT);
                        if (topLevel) {
                            Capture_GetWindowRect(topLevel, &g_selection.selectedRect);
                            g_selection.state = SEL_COMPLETE;
                            UpdateOverlayBitmap();
                            ShowActionToolbar(TRUE);
                        }
                    }
                } else if (g_currentMode == MODE_MONITOR) {
                    RECT monRect;
                    int monIndex;
                    if (Capture_GetMonitorFromPoint(pt, &monRect, &monIndex)) {
                        g_selection.selectedRect = monRect;
                        g_selection.state = SEL_COMPLETE;
                        UpdateOverlayBitmap();
                        ShowActionToolbar(TRUE);
                    }
                } else if (g_currentMode == MODE_ALL_MONITORS) {
                    Capture_GetAllMonitorsBounds(&g_selection.selectedRect);
                    g_selection.state = SEL_COMPLETE;
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
            // Check for configurable cancel key
            if (wParam == (WPARAM)g_config.cancelKey) {
                if (InterlockedCompareExchange(&g_isRecording, 0, 0)) {
                    Recording_Stop();
                } else if (g_selection.state == SEL_DRAWING || g_selection.state == SEL_MOVING || g_selection.state == SEL_RESIZING) {
                    ReleaseCapture();
                    g_selection.state = SEL_NONE;
                    SetRectEmpty(&g_selection.selectedRect);
                    UpdateOverlayBitmap();
                    ShowActionToolbar(FALSE);
                } else if (g_selection.state == SEL_COMPLETE) {
                    // Cancel selection
                    g_selection.state = SEL_NONE;
                    SetRectEmpty(&g_selection.selectedRect);
                    UpdateOverlayBitmap();
                    ShowActionToolbar(FALSE);
                } else {
                    PostQuitMessage(0);
                }
            } else if (wParam == VK_RETURN && g_selection.state == SEL_COMPLETE) {
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

/* ============================================================================
 * CONTROL PANEL WM_DRAWITEM HELPERS
 * ============================================================================
 * These functions extract the owner-draw button painting logic from the
 * large WM_DRAWITEM handler for better readability and maintainability.
 */

/**
 * Draw a mode button (Capture Area, Capture Window, etc.) with selection state.
 */
static void DrawModeButton(LPDRAWITEMSTRUCT dis, BOOL isSelected, BOOL isHovered) {
    COLORREF bgColor, borderColor;
    
    if (isSelected) {
        bgColor = RGB(0, 95, 184);     /* Windows blue for selected */
        borderColor = RGB(0, 120, 215);
    } else if (isHovered || (dis->itemState & ODS_SELECTED)) {
        bgColor = RGB(55, 55, 55);     /* Hover color */
        borderColor = RGB(80, 80, 80);
    } else {
        bgColor = RGB(32, 32, 32);     /* Normal background */
        borderColor = RGB(80, 80, 80);
    }
    
    DrawRoundedRectAA(dis->hDC, &dis->rcItem, 6, bgColor, borderColor);
    
    /* Draw text */
    SelectObject(dis->hDC, g_uiFont);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, RGB(255, 255, 255));
    
    WCHAR text[64];
    GetWindowTextW(dis->hwndItem, text, 64);
    RECT textRect = dis->rcItem;
    DrawTextW(dis->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/**
 * Draw the record button (red circle when idle, white square when recording).
 */
static void DrawRecordButton(LPDRAWITEMSTRUCT dis, BOOL isRecording) {
    int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
    int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
    
    if (isRecording) {
        /* White square (stop icon) */
        HBRUSH iconBrush = CreateSolidBrush(RGB(255, 255, 255));
        RECT stopRect = { cx - 4, cy - 4, cx + 4, cy + 4 };
        FillRect(dis->hDC, &stopRect, iconBrush);
        DeleteObject(iconBrush);
    } else {
        /* Red filled circle (record icon) */
        DrawCircleAA(dis->hDC, cx, cy, 6, RGB(220, 50, 50));
    }
}

/**
 * Draw an icon button using Segoe MDL2 Assets font.
 * @param dis     Draw item struct
 * @param icon    Unicode character for the icon (e.g., L"\uE713" for gear)
 * @param size    Font size
 */
static void DrawMDL2IconButton(LPDRAWITEMSTRUCT dis, LPCWSTR icon, int size) {
    HFONT mdl2Font = CreateFontW(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    HFONT oldFont = (HFONT)SelectObject(dis->hDC, mdl2Font);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, RGB(150, 150, 150));
    DrawTextW(dis->hDC, icon, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, oldFont);
    DeleteObject(mdl2Font);
}

/**
 * Draw the recording panel with timer and stop button.
 * Contains red dot + elapsed time on left, stop button on right.
 */
static void DrawRecordingPanel(LPDRAWITEMSTRUCT dis, const char* timerText) {
    RECT rect = dis->rcItem;
    int width = rect.right - rect.left;
    int centerX = width / 2;
    
    /* Check hover state */
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(dis->hwndItem, &pt);
    BOOL isHover = PtInRect(&rect, pt);
    
    /* Background - slightly lighter on hover */
    COLORREF btnBgColor = isHover ? RGB(48, 48, 48) : RGB(32, 32, 32);
    HBRUSH bgBrush = CreateSolidBrush(btnBgColor);
    FillRect(dis->hDC, &rect, bgBrush);
    DeleteObject(bgBrush);
    
    /* Draw rounded border */
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    RoundRect(dis->hDC, rect.left, rect.top, rect.right, rect.bottom, 6, 6);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(borderPen);
    
    /* Measure timer text width */
    SelectObject(dis->hDC, g_uiFont);
    SIZE timerSize;
    GetTextExtentPoint32A(dis->hDC, timerText, (int)strlen(timerText), &timerSize);
    int dotSize = 8;
    int dotGap = 6;
    int leftContentWidth = dotSize + dotGap + timerSize.cx;
    int leftStartX = rect.left + (centerX - leftContentWidth) / 2;
    
    /* Draw anti-aliased red recording dot using GDI+ */
    if (g_gdip.CreateFromHDC && g_gdip.SetSmoothingMode && g_gdip.CreateSolidFill && 
        g_gdip.FillEllipse && g_gdip.BrushDelete && g_gdip.DeleteGraphics) {
        GpGraphics* graphics = NULL;
        g_gdip.CreateFromHDC(dis->hDC, &graphics);
        if (graphics) {
            g_gdip.SetSmoothingMode(graphics, 4);
            GpBrush* redBrush = NULL;
            g_gdip.CreateSolidFill(0xFFEA4335, &redBrush);
            if (redBrush) {
                int dotY = (rect.top + rect.bottom - dotSize) / 2;
                g_gdip.FillEllipse(graphics, redBrush, (float)leftStartX, (float)dotY, 
                                   (float)dotSize, (float)dotSize);
                g_gdip.BrushDelete(redBrush);
            }
            g_gdip.DeleteGraphics(graphics);
        }
    }
    
    /* Draw timer text */
    SetBkMode(dis->hDC, TRANSPARENT);
    COLORREF textColor = isHover ? RGB(230, 230, 230) : RGB(200, 200, 200);
    SetTextColor(dis->hDC, textColor);
    
    RECT timerRect = rect;
    timerRect.left = leftStartX + dotSize + dotGap;
    timerRect.right = rect.left + centerX;
    DrawTextA(dis->hDC, timerText, -1, &timerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    
    /* Draw vertical divider at center */
    HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    SelectObject(dis->hDC, dividerPen);
    MoveToEx(dis->hDC, rect.left + centerX, rect.top + 6, NULL);
    LineTo(dis->hDC, rect.left + centerX, rect.bottom - 6);
    DeleteObject(dividerPen);
    
    /* Right half: red stop square + "Stop" (centered) */
    SIZE stopSize;
    GetTextExtentPoint32A(dis->hDC, "Stop", 4, &stopSize);
    int stopSquareSize = 8;
    int stopGap = 6;
    int rightContentWidth = stopSquareSize + stopGap + stopSize.cx;
    int rightStartX = rect.left + centerX + (centerX - rightContentWidth) / 2;
    
    /* Draw red stop square */
    int stopSquareY = (rect.top + rect.bottom - stopSquareSize) / 2;
    HBRUSH stopBrush = CreateSolidBrush(RGB(234, 67, 53));
    RECT stopSquareRect = { rightStartX, stopSquareY, rightStartX + stopSquareSize, stopSquareY + stopSquareSize };
    FillRect(dis->hDC, &stopSquareRect, stopBrush);
    DeleteObject(stopBrush);
    
    /* Draw "Stop" text */
    RECT stopTextRect = rect;
    stopTextRect.left = rightStartX + stopSquareSize + stopGap;
    stopTextRect.right = rect.right - 4;
    DrawTextA(dis->hDC, "Stop", -1, &stopTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

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
            // Layout: [Capture Area][Capture Window][Capture Monitor][Capture All Monitors] ... [Settings - O X]
            int btnX = 8;
            int btnWidth = 130;  // Standard width for capture buttons
            int btnHeight = 30;
            int btnGap = 4;
            
            CreateWindowW(L"BUTTON", L"Capture Area",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                btnX, 7, btnWidth, btnHeight, hwnd, (HMENU)ID_MODE_AREA, g_windows.hInstance, NULL);
            btnX += btnWidth + btnGap;
            
            CreateWindowW(L"BUTTON", L"Capture Window",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                btnX, 7, btnWidth, btnHeight, hwnd, (HMENU)ID_MODE_WINDOW, g_windows.hInstance, NULL);
            btnX += btnWidth + btnGap;
            
            CreateWindowW(L"BUTTON", L"Capture Monitor",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                btnX, 7, btnWidth, btnHeight, hwnd, (HMENU)ID_MODE_MONITOR, g_windows.hInstance, NULL);
            btnX += btnWidth + btnGap;
            
            CreateWindowW(L"BUTTON", L"Capture All Monitors",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                btnX, 7, btnWidth + 20, btnHeight, hwnd, (HMENU)ID_MODE_ALL, g_windows.hInstance, NULL);
            
            // Small icon buttons on right side (square 28x28, vertically centered)
            int iconBtnSize = 28;
            int iconBtnY = (44 - iconBtnSize) / 2;
            int rightX = 730 - 8 - iconBtnSize;  // Start from right edge
            
            // Close button (X) - rightmost
            CreateWindowW(L"BUTTON", L"\u2715",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                rightX, iconBtnY, iconBtnSize, iconBtnSize, hwnd, (HMENU)ID_BTN_CLOSE, g_windows.hInstance, NULL);
            rightX -= iconBtnSize + 4;
            
            // Record button (filled circle)
            CreateWindowW(L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                rightX, iconBtnY, iconBtnSize, iconBtnSize, hwnd, (HMENU)ID_BTN_RECORD, g_windows.hInstance, NULL);
            rightX -= iconBtnSize + 4;
            
            // Minimize button (-)
            CreateWindowW(L"BUTTON", L"-",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                rightX, iconBtnY, iconBtnSize, iconBtnSize, hwnd, (HMENU)ID_BTN_MINIMIZE, g_windows.hInstance, NULL);
            rightX -= iconBtnSize + 4;
            
            // Settings button (gear icon) - left of minimize
            CreateWindowW(L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                rightX, iconBtnY, iconBtnSize, iconBtnSize, hwnd, (HMENU)ID_BTN_SETTINGS, g_windows.hInstance, NULL);
            
            // No mode selected by default
            g_currentMode = MODE_NONE;
            
            // Start hover timer for icon button updates
            SetTimer(hwnd, ID_TIMER_HOVER, 50, NULL);
            
            // Start replay buffer health check timer (every 2 seconds)
            SetTimer(hwnd, ID_TIMER_REPLAY_CHECK, 2000, NULL);
            
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
                    if (g_windows.settingsWnd) {
                        // Close settings if already open (use WM_CLOSE to trigger cleanup)
                        SendMessage(g_windows.settingsWnd, WM_CLOSE, 0, 0);
                    } else {
                        // Open settings below control panel, centered
                        RECT ctrlRect;
                        GetWindowRect(hwnd, &ctrlRect);
                        int ctrlCenterX = (ctrlRect.left + ctrlRect.right) / 2;
                        
                        g_windows.settingsWnd = CreateWindowExA(
                            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                            "LWSRSettings",
                            NULL,
                            WS_POPUP | WS_VISIBLE | WS_BORDER,
                            ctrlCenterX - SETTINGS_WIDTH / 2, ctrlRect.bottom + 5,
                            SETTINGS_WIDTH, SETTINGS_HEIGHT,
                            hwnd, NULL, g_windows.hInstance, NULL
                        );
                        
                        // Refresh settings button to show highlight
                        HWND settingsBtn = GetDlgItem(hwnd, ID_BTN_SETTINGS);
                        if (settingsBtn) InvalidateRect(settingsBtn, NULL, TRUE);
                    }
                    break;
                case ID_BTN_RECORD:
                    // Toggle recording - thread-safe check
                    if (InterlockedCompareExchange(&g_isRecording, 0, 0)) {
                        Recording_Stop();
                    } else {
                        // If no selection, use full primary monitor
                        if (IsRectEmpty(&g_selection.selectedRect)) {
                            HMONITOR hMon = MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY);
                            MONITORINFO mi = { sizeof(mi) };
                            GetMonitorInfo(hMon, &mi);
                            g_selection.selectedRect = mi.rcMonitor;
                        }
                        Recording_Start();
                    }
                    // Redraw button to show state change
                    InvalidateRect(GetDlgItem(hwnd, ID_BTN_RECORD), NULL, TRUE);
                    break;
                case ID_BTN_CLOSE: {
                    // Hide window immediately to avoid visual artifacts
                    ShowWindow(hwnd, SW_HIDE);
                    
                    // Stop recording if in progress - thread-safe check
                    if (InterlockedCompareExchange(&g_isRecording, 0, 0)) {
                        Recording_Stop();
                    }
                    
                    // Stop replay buffer
                    extern ReplayBufferState g_replayBuffer;
                    if (g_replayBuffer.isBuffering) {
                        ReplayBuffer_Stop(&g_replayBuffer);
                    }
                    
                    // Fast exit - normal return path has slow NVIDIA driver cleanup
                    ExitProcess(0);
                    break;
                }
                case ID_BTN_MINIMIZE:
                    // Minimize to system tray
                    MinimizeToTray();
                    break;
                case ID_BTN_STOP:
                    Recording_Stop();
                    break;
                case ID_RECORDING_PANEL:
                    // Click on recording panel stops recording - thread-safe check
                    if (InterlockedCompareExchange(&g_isRecording, 0, 0)) {
                        Recording_Stop();
                    }
                    break;
            }
            return 0;
            
        case WM_TIMER:
            if (wParam == ID_TIMER_LIMIT) {
                // Time limit reached
                Recording_Stop();
            } else if (wParam == ID_TIMER_DISPLAY) {
                // Update timer display
                UpdateTimerDisplay();
            } else if (wParam == ID_TIMER_REPLAY_CHECK) {
                // Stall detection moved to HealthMonitor thread (health_monitor.c)
                // This timer is kept for future periodic UI updates if needed
                // The HealthMonitor posts WM_WORKER_STALLED when recovery is needed
            } else if (wParam == ID_TIMER_HOVER) {
                // Check which icon button is currently hovered (if any)
                POINT pt;
                GetCursorPos(&pt);
                
                HWND iconBtns[] = {
                    GetDlgItem(hwnd, ID_BTN_SETTINGS),
                    GetDlgItem(hwnd, ID_BTN_MINIMIZE),
                    GetDlgItem(hwnd, ID_BTN_RECORD),
                    GetDlgItem(hwnd, ID_BTN_CLOSE)
                };
                
                HWND currentHovered = NULL;
                for (int i = 0; i < 4; i++) {
                    if (iconBtns[i]) {
                        RECT rc;
                        GetWindowRect(iconBtns[i], &rc);
                        if (PtInRect(&rc, pt)) {
                            currentHovered = iconBtns[i];
                            break;
                        }
                    }
                }
                
                // Only invalidate if hover state changed
                if (currentHovered != g_interaction.lastHoveredIconBtn) {
                    // Invalidate old hovered button (to remove highlight)
                    if (g_interaction.lastHoveredIconBtn) {
                        InvalidateRect(g_interaction.lastHoveredIconBtn, NULL, FALSE);
                    }
                    // Invalidate new hovered button (to add highlight)
                    if (currentHovered) {
                        InvalidateRect(currentHovered, NULL, FALSE);
                    }
                    g_interaction.lastHoveredIconBtn = currentHovered;
                }
                
                // Also check capture buttons for hover
                HWND captureBtns[] = {
                    GetDlgItem(hwnd, ID_MODE_AREA),
                    GetDlgItem(hwnd, ID_MODE_WINDOW),
                    GetDlgItem(hwnd, ID_MODE_MONITOR),
                    GetDlgItem(hwnd, ID_MODE_ALL)
                };
                
                HWND currentHoveredCapture = NULL;
                for (int i = 0; i < 4; i++) {
                    if (captureBtns[i]) {
                        RECT rc;
                        GetWindowRect(captureBtns[i], &rc);
                        if (PtInRect(&rc, pt)) {
                            currentHoveredCapture = captureBtns[i];
                            break;
                        }
                    }
                }
                
                // Only invalidate if capture button hover state changed
                if (currentHoveredCapture != g_interaction.lastHoveredCaptureBtn) {
                    if (g_interaction.lastHoveredCaptureBtn) {
                        InvalidateRect(g_interaction.lastHoveredCaptureBtn, NULL, FALSE);
                    }
                    if (currentHoveredCapture) {
                        InvalidateRect(currentHoveredCapture, NULL, FALSE);
                    }
                    g_interaction.lastHoveredCaptureBtn = currentHoveredCapture;
                }
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
            
            // Check if this is a mode button (capture buttons only)
            BOOL isModeButton = (ctlId >= ID_MODE_AREA && ctlId <= ID_MODE_ALL);
            
            // Check if this is an icon button (no visible border, transparent bg)
            BOOL isIconButton = (ctlId == ID_BTN_SETTINGS || ctlId == ID_BTN_MINIMIZE || 
                                 ctlId == ID_BTN_RECORD || ctlId == ID_BTN_CLOSE);
            
            // Check actual mouse position for hover (more reliable than ODS_HOTLIGHT)
            BOOL isHovered = FALSE;
            {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(dis->hwndItem, &pt);
                isHovered = PtInRect(&dis->rcItem, pt);
            }
            
            // For settings button, check if settings window is open
            BOOL isSettingsActive = (ctlId == ID_BTN_SETTINGS && g_windows.settingsWnd != NULL && IsWindowVisible(g_windows.settingsWnd));
            
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
            BOOL showHoverBg = FALSE;
            if (isSelected) {
                bgColor = RGB(0, 95, 184); // Windows blue for selected
                borderColor = RGB(0, 120, 215);
            } else if (isIconButton) {
                // Icon buttons: show hover effect or settings active state
                if (isSettingsActive || isHovered) {
                    bgColor = RGB(55, 55, 55);  // Hover/active color
                    borderColor = RGB(55, 55, 55);
                    showHoverBg = TRUE;
                } else {
                    bgColor = RGB(32, 32, 32);  // Normal background
                    borderColor = RGB(32, 32, 32);
                }
            } else if (isHovered || (dis->itemState & ODS_SELECTED)) {
                bgColor = RGB(55, 55, 55); // Hover color
                borderColor = RGB(80, 80, 80);
            } else {
                bgColor = RGB(32, 32, 32); // Normal background
                borderColor = RGB(80, 80, 80);
            }
            
            // For icon buttons with hover, draw rounded rect; otherwise flat fill
            if (isIconButton) {
                if (showHoverBg) {
                    // Draw rounded hover background
                    DrawRoundedRectAA(dis->hDC, &dis->rcItem, 4, bgColor, borderColor);
                } else {
                    // Flat fill to match bar background
                    HBRUSH bgBrush = CreateSolidBrush(bgColor);
                    FillRect(dis->hDC, &dis->rcItem, bgBrush);
                    DeleteObject(bgBrush);
                }
            } else {
                // Draw anti-aliased rounded button background
                DrawRoundedRectAA(dis->hDC, &dis->rcItem, 6, bgColor, borderColor);
            }
            
            // Draw text for mode buttons (no icon, just centered text)
            if (isModeButton) {
                DrawModeButton(dis, isSelected, isHovered);
                return TRUE;
            }
            
            // Record button
            if (ctlId == ID_BTN_RECORD) {
                BOOL isRecording = InterlockedCompareExchange(&g_isRecording, 0, 0) != 0;
                DrawRecordButton(dis, isRecording);
                return TRUE;
            }
            
            // Minimize button - use Segoe MDL2 Assets icon font
            if (ctlId == ID_BTN_MINIMIZE) {
                DrawMDL2IconButton(dis, L"\uE921", 12);  /* ChromeMinimize */
                return TRUE;
            }
            
            // Settings button - use Segoe MDL2 Assets gear icon
            if (ctlId == ID_BTN_SETTINGS) {
                DrawMDL2IconButton(dis, L"\uE713", 14);  /* Settings gear */
                return TRUE;
            }
            
            // Recording panel (inline timer + stop button)
            if (ctlId == ID_RECORDING_PANEL) {
                DrawRecordingPanel(dis, g_timerText);
                return TRUE;
            }
            
            // Close button - use Segoe MDL2 Assets icon font
            if (ctlId == ID_BTN_CLOSE) {
                DrawMDL2IconButton(dis, L"\uE8BB", 12);  /* ChromeClose */
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
        
        case WM_HOTKEY: {
            Logger_Log("WM_HOTKEY received: wParam=%llu\n", (unsigned long long)wParam);
            if (wParam == HOTKEY_REPLAY_SAVE) {
                Logger_Log("HOTKEY_REPLAY_SAVE matched, isBuffering=%d, bufferReady=%d\n", 
                           g_replayBuffer.isBuffering, g_replayBuffer.bufferReady);
                // Check if replay buffer is actually running with frames
                if (!g_replayBuffer.isBuffering) {
                    Logger_Log("Not buffering, aborting save\n");
                    MessageBeep(MB_ICONWARNING);
                    return 0;
                }
                if (!g_replayBuffer.bufferReady) {
                    Logger_Log("Buffer not ready yet, aborting save\n");
                    MessageBeep(MB_ICONWARNING);
                    return 0;
                }
                
                // Generate filename with timestamp
                char filename[MAX_PATH];
                SYSTEMTIME st;
                GetLocalTime(&st);
                snprintf(filename, sizeof(filename), "%s\\Replay_%04d%02d%02d_%02d%02d%02d.mp4",
                    g_config.savePath, (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                    (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
                
                Logger_Log("Generated filename: %s\n", filename);
                Logger_Log("Calling ReplayBuffer_SaveAsync...\n");
                
                // Request async save - will post WM_REPLAY_SAVE_COMPLETE when done
                BOOL started = ReplayBuffer_SaveAsync(&g_replayBuffer, filename,
                                                      hwnd, WM_REPLAY_SAVE_COMPLETE);
                
                if (started) {
                    Logger_Log("Async save started\n");
                    // Could show a "saving..." indicator here
                } else {
                    Logger_Log("Failed to start async save\n");
                    MessageBeep(MB_ICONWARNING);
                }
            }
            return 0;
        }
        
        case WM_REPLAY_SAVE_COMPLETE: {
            // Async save completed - wParam contains success status
            BOOL success = (BOOL)wParam;
            Logger_Log("WM_REPLAY_SAVE_COMPLETE received: success=%d\n", success);
            
            if (success) {
                MessageBeep(MB_OK);  // Success audio feedback
            } else {
                MessageBeep(MB_ICONERROR);  // Failure audio feedback
            }
            return 0;
        }
        
        case WM_WORKER_STALLED: {
            // Health monitor detected a stalled worker thread
            StallType stallType = (StallType)wParam;
            Logger_Log("WM_WORKER_STALLED received: type=%d\n", stallType);
            
            // Only recover if replay buffer is supposed to be running
            if (g_config.replayEnabled && g_replayBuffer.isBuffering) {
                Logger_Log("Initiating replay buffer recovery...\n");
                
                // Duplicate the thread handle BEFORE Stop() closes it
                // Cleanup thread needs its own handle to wait on
                HANDLE hungThreadCopy = NULL;
                if (g_replayBuffer.bufferThread) {
                    if (!DuplicateHandle(GetCurrentProcess(), g_replayBuffer.bufferThread,
                                         GetCurrentProcess(), &hungThreadCopy,
                                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
                        Logger_Log("DuplicateHandle failed: %u\n", GetLastError());
                        hungThreadCopy = NULL;
                    }
                }
                
                // Schedule cleanup of orphaned resources (fire-and-forget thread)
                // Pass the duplicated handle so cleanup can wait for it (NULL is handled)
                HealthMonitor_ScheduleCleanup(hungThreadCopy, stallType);
                
                // Force state to STALLED so Stop() knows threads are hung
                InterlockedExchange(&g_replayBuffer.state, REPLAY_STATE_STALLED);
                
                // Stop the replay buffer (will timeout on hung thread)
                ReplayBuffer_Stop(&g_replayBuffer);
                
                // Small delay before restart
                Sleep(500);
                
                // Restart the replay buffer
                ReplayBuffer_Start(&g_replayBuffer, &g_config);
                CheckAudioError();
                
                // Notify health monitor that restart is complete
                HealthMonitor_NotifyRestart();
                
                Logger_Log("Replay buffer restarted after stall recovery\n");
            }
            return 0;
        }
        
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                // Left click or double-click on tray icon - restore window
                RestoreFromTray();
            } else if (lParam == WM_RBUTTONUP) {
                // Right click - show context menu
                POINT pt;
                GetCursorPos(&pt);
                
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_SHOW, "Show");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");
                
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, 
                                         pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                
                if (cmd == ID_TRAY_SHOW) {
                    RestoreFromTray();
                } else if (cmd == ID_TRAY_EXIT) {
                    RemoveTrayIcon();
                    PostQuitMessage(0);
                }
            }
            return 0;
        
        case WM_DESTROY:
            if (g_uiFont) DeleteObject(g_uiFont);
            if (g_iconFont) DeleteObject(g_iconFont);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Helper function to get key name from virtual key code
static void GetKeyNameFromVK(int vk, char* buffer, int bufferSize) {
    if (bufferSize < 1) return;
    buffer[0] = '\0';
    
    // Helper macro for safe string copy
    #define SAFE_COPY(str) do { strncpy(buffer, str, bufferSize - 1); buffer[bufferSize - 1] = '\0'; return; } while(0)
    
    // Handle special keys
    switch (vk) {
        case VK_F1: SAFE_COPY("F1");
        case VK_F2: SAFE_COPY("F2");
        case VK_F3: SAFE_COPY("F3");
        case VK_F4: SAFE_COPY("F4");
        case VK_F5: SAFE_COPY("F5");
        case VK_F6: SAFE_COPY("F6");
        case VK_F7: SAFE_COPY("F7");
        case VK_F8: SAFE_COPY("F8");
        case VK_F9: SAFE_COPY("F9");
        case VK_F10: SAFE_COPY("F10");
        case VK_F11: SAFE_COPY("F11");
        case VK_F12: SAFE_COPY("F12");
        case VK_ESCAPE: SAFE_COPY("Escape");
        case VK_TAB: SAFE_COPY("Tab");
        case VK_RETURN: SAFE_COPY("Enter");
        case VK_SPACE: SAFE_COPY("Space");
        case VK_BACK: SAFE_COPY("Backspace");
        case VK_DELETE: SAFE_COPY("Delete");
        case VK_INSERT: SAFE_COPY("Insert");
        case VK_HOME: SAFE_COPY("Home");
        case VK_END: SAFE_COPY("End");
        case VK_PRIOR: SAFE_COPY("Page Up");
        case VK_NEXT: SAFE_COPY("Page Down");
        case VK_LEFT: SAFE_COPY("Left");
        case VK_RIGHT: SAFE_COPY("Right");
        case VK_UP: SAFE_COPY("Up");
        case VK_DOWN: SAFE_COPY("Down");
        case VK_PAUSE: SAFE_COPY("Pause");
        case VK_SCROLL: SAFE_COPY("Scroll Lock");
        case VK_SNAPSHOT: SAFE_COPY("Print Screen");
        case VK_NUMLOCK: SAFE_COPY("Num Lock");
        default:
            // For letters and numbers, just use the character
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
                if (bufferSize >= 2) {
                    buffer[0] = (char)vk;
                    buffer[1] = '\0';
                }
                return;
            }
            // Numpad keys
            if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
                snprintf(buffer, bufferSize, "Numpad %d", vk - VK_NUMPAD0);
                return;
            }
            // Default: use scan code
            UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
            if (GetKeyNameTextA(scanCode << 16, buffer, bufferSize) == 0) {
                snprintf(buffer, bufferSize, "Key 0x%02X", vk);
            }
            break;
    }
    #undef SAFE_COPY
}

// Settings window procedure
static HFONT g_settingsFont = NULL;
static HFONT g_settingsSmallFont = NULL;
static HBRUSH g_settingsBgBrush = NULL;

// Helper: Calculate aspect ratio dimensions (returns width:height ratio multiplied by 1000)
static void GetAspectRatioDimensions(int aspectIndex, int* ratioW, int* ratioH) {
    // 0=Native, 1=16:9, 2=9:16, 3=1:1, 4=4:5, 5=16:10, 6=4:3, 7=21:9, 8=32:9
    switch (aspectIndex) {
        case 1: *ratioW = 16; *ratioH = 9; break;   // 16:9 (YouTube, Standard)
        case 2: *ratioW = 9;  *ratioH = 16; break;  // 9:16 (TikTok, Shorts, Reels)
        case 3: *ratioW = 1;  *ratioH = 1; break;   // 1:1 (Square - Instagram)
        case 4: *ratioW = 4;  *ratioH = 5; break;   // 4:5 (Instagram Portrait)
        case 5: *ratioW = 16; *ratioH = 10; break;  // 16:10
        case 6: *ratioW = 4;  *ratioH = 3; break;   // 4:3
        case 7: *ratioW = 21; *ratioH = 9; break;   // 21:9 (Ultrawide)
        case 8: *ratioW = 32; *ratioH = 9; break;   // 32:9 (Super Ultrawide)
        default: *ratioW = 0; *ratioH = 0; break;   // Native
    }
}

// Helper: Calculate aspect ratio rect centered on monitor bounds
static RECT CalculateAspectRect(RECT monBounds, int aspectW, int aspectH) {
    int monW = monBounds.right - monBounds.left;
    int monH = monBounds.bottom - monBounds.top;
    
    int rectW, rectH;
    
    // Fit to monitor while maintaining aspect ratio
    if (monW * aspectH > monH * aspectW) {
        // Monitor is wider than aspect ratio - fit to height
        rectH = monH;
        rectW = (rectH * aspectW) / aspectH;
    } else {
        // Monitor is taller than aspect ratio - fit to width
        rectW = monW;
        rectH = (rectW * aspectH) / aspectW;
    }
    
    // Center on monitor
    RECT result;
    result.left = monBounds.left + (monW - rectW) / 2;
    result.top = monBounds.top + (monH - rectH) / 2;
    result.right = result.left + rectW;
    result.bottom = result.top + rectH;
    
    return result;
}

// Helper: Populate an audio dropdown with devices
// Returns the index that matches the given deviceId, or 0 if not found
static int PopulateAudioDropdown(HWND comboBox, const AudioDeviceList* devices, const char* selectedDeviceId) {
    if (!comboBox || !devices) return 0;
    
    int selectedIdx = 0;
    int currentIdx = 0;
    
    // Add "None (Disabled)" option
    int itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)"None (Disabled)");
    SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)NULL);  // NULL = no device
    currentIdx++;
    
    // Add separator for outputs (system audio)
    itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)"--- System Audio (Loopback) ---");
    SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)NULL);  // Separator - no data
    currentIdx++;
    
    // Add output devices
    for (int i = 0; i < devices->count; i++) {
        if (devices->devices[i].type == AUDIO_DEVICE_OUTPUT) {
            char displayName[160];
            if (devices->devices[i].isDefault) {
                snprintf(displayName, sizeof(displayName), "%s (Default)", devices->devices[i].name);
            } else {
                strncpy(displayName, devices->devices[i].name, sizeof(displayName) - 1);
                displayName[sizeof(displayName) - 1] = '\0';
            }
            itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)displayName);
            // Store pointer to device ID (devices array must remain valid!)
            SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)devices->devices[i].id);
            
            // Check if this is the selected device
            if (selectedDeviceId && selectedDeviceId[0] != '\0' &&
                strcmp(devices->devices[i].id, selectedDeviceId) == 0) {
                selectedIdx = currentIdx;
            }
            currentIdx++;
        }
    }
    
    // Add separator for inputs (microphones)
    itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)"--- Microphones ---");
    SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)NULL);  // Separator - no data
    currentIdx++;
    
    // Add input devices
    for (int i = 0; i < devices->count; i++) {
        if (devices->devices[i].type == AUDIO_DEVICE_INPUT) {
            char displayName[160];
            if (devices->devices[i].isDefault) {
                snprintf(displayName, sizeof(displayName), "%s (Default)", devices->devices[i].name);
            } else {
                strncpy(displayName, devices->devices[i].name, sizeof(displayName) - 1);
                displayName[sizeof(displayName) - 1] = '\0';
            }
            itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)displayName);
            // Store pointer to device ID
            SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)devices->devices[i].id);
            
            // Check if this is the selected device
            if (selectedDeviceId && selectedDeviceId[0] != '\0' &&
                strcmp(devices->devices[i].id, selectedDeviceId) == 0) {
                selectedIdx = currentIdx;
            }
            currentIdx++;
        }
    }
    
    return selectedIdx;
}

// Update RAM usage estimate label in settings
static void UpdateReplayRAMEstimate(HWND hwndSettings) {
    HWND lblRam = GetDlgItem(hwndSettings, ID_STATIC_REPLAY_RAM);
    HWND lblCalc = GetDlgItem(hwndSettings, ID_STATIC_REPLAY_CALC);
    if (!lblRam || !lblCalc) return;
    
    int durationSecs = g_config.replayDuration;
    int fps = g_config.replayFPS;
    
    // Get monitor resolution for estimate
    int estWidth = GetSystemMetrics(SM_CXSCREEN);
    int estHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // Adjust for aspect ratio if set
    if (g_config.replayAspectRatio > 0) {
        int ratioW, ratioH;
        GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
        if (ratioW > 0 && ratioH > 0) {
            // Calculate effective resolution with aspect ratio applied
            if (estWidth * ratioH > estHeight * ratioW) {
                // Monitor is wider - fit to height
                estWidth = (estHeight * ratioW) / ratioH;
            } else {
                // Monitor is taller - fit to width
                estHeight = (estWidth * ratioH) / ratioW;
            }
        }
    }
    
    int ramMB = ReplayBuffer_EstimateRAMUsage(durationSecs, estWidth, estHeight, fps, g_config.quality);
    
    // Update explanation text
    char explainText[256];
    snprintf(explainText, sizeof(explainText), "When enabled, ~%d MB of RAM is reserved for the video buffer. See the calculation below:", ramMB);
    SetWindowTextA(lblRam, explainText);
    
    // Update calculation text
    char calcText[128];
    if (durationSecs >= 60) {
        int mins = durationSecs / 60;
        int secs = durationSecs % 60;
        if (secs > 0) {
            snprintf(calcText, sizeof(calcText), "%dm %ds @ %d FPS, %dx%d = ~%d MB", mins, secs, fps, estWidth, estHeight, ramMB);
        } else {
            snprintf(calcText, sizeof(calcText), "%dm @ %d FPS, %dx%d = ~%d MB", mins, fps, estWidth, estHeight, ramMB);
        }
    } else {
        snprintf(calcText, sizeof(calcText), "%ds @ %d FPS, %dx%d = ~%d MB", durationSecs, fps, estWidth, estHeight, ramMB);
    }
    SetWindowTextA(lblCalc, calcText);
}

// Update preview border based on current replay capture source
static void UpdateReplayPreview(void) {
    // Hide any existing overlays first
    PreviewBorder_Hide();
    AreaSelector_Hide();
    
    // Show appropriate preview based on capture source
    switch (g_config.replayCaptureSource) {
        case MODE_MONITOR: {
            RECT monBounds;
            if (Capture_GetMonitorBoundsByIndex(g_config.replayMonitorIndex, &monBounds)) {
                // Check if aspect ratio is set (not Native)
                if (g_config.replayAspectRatio > 0) {
                    int ratioW, ratioH;
                    GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
                    
                    // Use saved area rect if valid, otherwise calculate default
                    RECT aspectRect = g_config.replayAreaRect;
                    int areaW = aspectRect.right - aspectRect.left;
                    int areaH = aspectRect.bottom - aspectRect.top;
                    
                    // Validate saved area is within monitor and has correct aspect ratio
                    // If not, calculate a new default centered rect
                    BOOL needsRecalc = FALSE;
                    if (areaW <= 0 || areaH <= 0) needsRecalc = TRUE;
                    if (aspectRect.left < monBounds.left || aspectRect.right > monBounds.right) needsRecalc = TRUE;
                    if (aspectRect.top < monBounds.top || aspectRect.bottom > monBounds.bottom) needsRecalc = TRUE;
                    
                    if (needsRecalc) {
                        aspectRect = CalculateAspectRect(monBounds, ratioW, ratioH);
                        g_config.replayAreaRect = aspectRect;
                    }
                    
                    AreaSelector_Show(aspectRect, TRUE);  // Allow moving aspect ratio regions
                } else {
                    // Native - show overlay covering full monitor (locked, no moving)
                    AreaSelector_Show(monBounds, FALSE);
                }
            }
            break;
        }
        case MODE_ALL_MONITORS: {
            RECT allBounds;
            if (Capture_GetAllMonitorsBounds(&allBounds)) {
                // Check if aspect ratio is set (not Native)
                if (g_config.replayAspectRatio > 0) {
                    int ratioW, ratioH;
                    GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
                    
                    RECT aspectRect = g_config.replayAreaRect;
                    int areaW = aspectRect.right - aspectRect.left;
                    int areaH = aspectRect.bottom - aspectRect.top;
                    
                    BOOL needsRecalc = FALSE;
                    if (areaW <= 0 || areaH <= 0) needsRecalc = TRUE;
                    if (aspectRect.left < allBounds.left || aspectRect.right > allBounds.right) needsRecalc = TRUE;
                    if (aspectRect.top < allBounds.top || aspectRect.bottom > allBounds.bottom) needsRecalc = TRUE;
                    
                    if (needsRecalc) {
                        aspectRect = CalculateAspectRect(allBounds, ratioW, ratioH);
                        g_config.replayAreaRect = aspectRect;
                    }
                    
                    AreaSelector_Show(aspectRect, TRUE);  // Allow moving aspect ratio regions
                } else {
                    // Native - show overlay covering all monitors (locked, no moving)
                    AreaSelector_Show(allBounds, FALSE);
                }
            }
            break;
        }
        case MODE_AREA: {
            // Show draggable area selector
            // Check if saved area is valid, otherwise create a centered default
            RECT areaRect = g_config.replayAreaRect;
            int areaW = areaRect.right - areaRect.left;
            int areaH = areaRect.bottom - areaRect.top;
            
            if (areaW < 100 || areaH < 100) {
                // Invalid or first-time - create a 640x480 box centered on primary monitor
                HMONITOR hMon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(hMon, &mi);
                int monW = mi.rcMonitor.right - mi.rcMonitor.left;
                int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
                int defaultW = 640;
                int defaultH = 480;
                areaRect.left = mi.rcMonitor.left + (monW - defaultW) / 2;
                areaRect.top = mi.rcMonitor.top + (monH - defaultH) / 2;
                areaRect.right = areaRect.left + defaultW;
                areaRect.bottom = areaRect.top + defaultH;
                g_config.replayAreaRect = areaRect;
            }
            
            AreaSelector_Show(areaRect, TRUE);
            break;
        }
        case MODE_WINDOW:
            // No preview for window mode - user selects window separately
            break;
        default:
            break;
    }
    
    // Ensure settings window and control panel stay on top of the preview overlays
    if (g_windows.settingsWnd) {
        SetWindowPos(g_windows.settingsWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g_controlWnd) {
        SetWindowPos(g_controlWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Save area selector position to config
static void SaveAreaSelectorPosition(void) {
    if (AreaSelector_IsVisible()) {
        AreaSelector_GetRect(&g_config.replayAreaRect);
    }
}

/* ============================================================================
 * SETTINGS WINDOW CREATION HELPERS
 * ============================================================================
 * These functions break up the large WM_CREATE handler into smaller,
 * more manageable sections for each settings category.
 */

/* Settings layout parameters */
typedef struct {
    int y;              /* Current Y position */
    int labelX;         /* X position for labels */
    int labelW;         /* Label width */
    int controlX;       /* X position for controls */
    int controlW;       /* Control width */
    int rowH;           /* Row height */
    int contentW;       /* Total content width */
    HFONT font;         /* Standard font */
    HFONT smallFont;    /* Small font */
} SettingsLayout;

/**
 * Create the output format and quality settings controls.
 * Creates dropdowns for format (MP4/AVI/WMV) and quality preset.
 */
static void CreateOutputSettings(HWND hwnd, SettingsLayout* layout) {
    /* Format dropdown */
    HWND lblFormat = CreateWindowW(L"STATIC", L"Output Format", 
        WS_CHILD | WS_VISIBLE,
        layout->labelX, layout->y + 5, layout->labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblFormat, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    HWND cmbFormat = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        layout->controlX, layout->y, layout->controlW, 120, hwnd, (HMENU)ID_CMB_FORMAT, g_windows.hInstance, NULL);
    SendMessage(cmbFormat, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.264) - Best compatibility");
    SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.265) - Smaller files, less compatible");
    SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"AVI - Legacy format");
    SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"WMV - Windows Media");
    SendMessage(cmbFormat, CB_SETCURSEL, g_config.outputFormat, 0);
    layout->y += layout->rowH;
    
    /* Quality dropdown */
    HWND lblQuality = CreateWindowW(L"STATIC", L"Quality",
        WS_CHILD | WS_VISIBLE,
        layout->labelX, layout->y + 5, layout->labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblQuality, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    HWND cmbQuality = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        layout->controlX, layout->y, layout->controlW, 120, hwnd, (HMENU)ID_CMB_QUALITY, g_windows.hInstance, NULL);
    SendMessage(cmbQuality, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Good ~60 Mbps (YouTube, TikTok)");
    SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"High ~75 Mbps (Discord, Twitter/X)");
    SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Ultra ~90 Mbps (Archival, editing)");
    SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Lossless ~130 Mbps (No artifacts)");
    SendMessage(cmbQuality, CB_SETCURSEL, g_config.quality, 0);
    layout->y += layout->rowH + 8;
}

/**
 * Create checkbox settings for capture options.
 * Creates checkboxes for mouse cursor and recording border.
 */
static void CreateCaptureCheckboxes(HWND hwnd, SettingsLayout* layout) {
    /* Separator line */
    CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        layout->labelX, layout->y, layout->contentW, 2, hwnd, NULL, g_windows.hInstance, NULL);
    layout->y += 14;
    
    /* Checkboxes side by side */
    HWND chkMouse = CreateWindowW(L"BUTTON", L"Capture mouse cursor",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        layout->labelX, layout->y, 200, 24, hwnd, (HMENU)ID_CHK_MOUSE, g_windows.hInstance, NULL);
    SendMessage(chkMouse, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_MOUSE, g_config.captureMouse ? BST_CHECKED : BST_UNCHECKED);
    
    HWND chkBorder = CreateWindowW(L"BUTTON", L"Show recording border",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        layout->labelX + 280, layout->y, 200, 24, hwnd, (HMENU)ID_CHK_BORDER, g_windows.hInstance, NULL);
    SendMessage(chkBorder, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_BORDER, g_config.showRecordingBorder ? BST_CHECKED : BST_UNCHECKED);
    layout->y += 38;
}

/**
 * Create time limit controls with hours/minutes/seconds dropdowns.
 */
static void CreateTimeLimitControls(HWND hwnd, SettingsLayout* layout) {
    HWND lblTime = CreateWindowW(L"STATIC", L"Time limit",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblTime, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    /* Calculate time from seconds */
    int totalSecs = g_config.maxRecordingSeconds;
    if (totalSecs < 1) totalSecs = 60;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;
    
    /* Hours dropdown */
    HWND cmbHours = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX, layout->y, 55, 300, hwnd, (HMENU)ID_CMB_HOURS, g_windows.hInstance, NULL);
    SendMessage(cmbHours, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessage(cmbHours, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    for (int i = 0; i <= 24; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(cmbHours, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(cmbHours, CB_SETCURSEL, hours, 0);
    
    HWND lblH = CreateWindowW(L"STATIC", L"h",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        layout->controlX + 58, layout->y, 15, 26, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblH, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    /* Minutes dropdown */
    HWND cmbMins = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX + 78, layout->y, 55, 300, hwnd, (HMENU)ID_CMB_MINUTES, g_windows.hInstance, NULL);
    SendMessage(cmbMins, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessage(cmbMins, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    for (int i = 0; i <= 59; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(cmbMins, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(cmbMins, CB_SETCURSEL, mins, 0);
    
    HWND lblM = CreateWindowW(L"STATIC", L"m",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        layout->controlX + 136, layout->y, 18, 26, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblM, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    /* Seconds dropdown */
    HWND cmbSecs = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX + 158, layout->y, 55, 300, hwnd, (HMENU)ID_CMB_SECONDS, g_windows.hInstance, NULL);
    SendMessage(cmbSecs, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessage(cmbSecs, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    for (int i = 0; i <= 59; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(cmbSecs, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(cmbSecs, CB_SETCURSEL, secs, 0);
    
    HWND lblS = CreateWindowW(L"STATIC", L"s",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        layout->controlX + 216, layout->y, 15, 26, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblS, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    layout->y += layout->rowH;
}

/**
 * Create save path controls with text field and browse button.
 */
static void CreateSavePathControls(HWND hwnd, SettingsLayout* layout) {
    HWND lblPath = CreateWindowW(L"STATIC", L"Save to",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        layout->labelX, layout->y + 1, layout->labelW, 22, hwnd, NULL, g_windows.hInstance, NULL);
    SendMessage(lblPath, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    HWND edtPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        layout->controlX, layout->y, layout->controlW - 80, 22, hwnd, (HMENU)ID_EDT_PATH, g_windows.hInstance, NULL);
    SendMessage(edtPath, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SetWindowTextA(edtPath, g_config.savePath);
    
    HWND btnBrowse = CreateWindowW(L"BUTTON", L"Browse",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        layout->controlX + layout->controlW - 72, layout->y, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, g_windows.hInstance, NULL);
    SendMessage(btnBrowse, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    layout->y += layout->rowH + 12;
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HFONT g_settingsTitleFont = NULL;
    
    switch (msg) {
        case WM_CREATE: {
            // Create fonts
            g_settingsFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsSmallFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsTitleFont = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblFormat, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbFormat = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                controlX, y, controlW, 120, hwnd, (HMENU)ID_CMB_FORMAT, g_windows.hInstance, NULL);
            SendMessage(cmbFormat, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.264) - Best compatibility");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.265) - Smaller files, less compatible");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"AVI - Legacy format");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"WMV - Windows Media");
            SendMessage(cmbFormat, CB_SETCURSEL, g_config.outputFormat, 0);
            y += rowH;
            
            // Quality dropdown with descriptions
            HWND lblQuality = CreateWindowW(L"STATIC", L"Quality",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblQuality, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbQuality = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                controlX, y, controlW, 120, hwnd, (HMENU)ID_CMB_QUALITY, g_windows.hInstance, NULL);
            SendMessage(cmbQuality, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Good ~60 Mbps (YouTube, TikTok)");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"High ~75 Mbps (Discord, Twitter/X)");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Ultra ~90 Mbps (Archival, editing)");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Lossless ~130 Mbps (No artifacts)");
            SendMessage(cmbQuality, CB_SETCURSEL, g_config.quality, 0);
            y += rowH + 8;
            
            // Separator line
            CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_windows.hInstance, NULL);
            y += 14;
            
            // Checkboxes side by side
            HWND chkMouse = CreateWindowW(L"BUTTON", L"Capture mouse cursor",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 200, 24, hwnd, (HMENU)ID_CHK_MOUSE, g_windows.hInstance, NULL);
            SendMessage(chkMouse, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_MOUSE, g_config.captureMouse ? BST_CHECKED : BST_UNCHECKED);
            
            // Show border checkbox - on the right side
            HWND chkBorder = CreateWindowW(L"BUTTON", L"Show recording border",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX + 280, y, 200, 24, hwnd, (HMENU)ID_CHK_BORDER, g_windows.hInstance, NULL);
            SendMessage(chkBorder, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_BORDER, g_config.showRecordingBorder ? BST_CHECKED : BST_UNCHECKED);
            y += 38;
            
            // Time limit - three dropdowns for hours, minutes, seconds
            HWND lblTime = CreateWindowW(L"STATIC", L"Time limit",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                labelX, y, labelW, 26, hwnd, NULL, g_windows.hInstance, NULL);
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
                controlX, y, 55, 300, hwnd, (HMENU)ID_CMB_HOURS, g_windows.hInstance, NULL);
            SendMessage(cmbHours, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbHours, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 24; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbHours, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbHours, CB_SETCURSEL, hours, 0);
            
            HWND lblH = CreateWindowW(L"STATIC", L"h",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 58, y, 15, 26, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblH, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Minutes dropdown
            HWND cmbMins = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 78, y, 55, 300, hwnd, (HMENU)ID_CMB_MINUTES, g_windows.hInstance, NULL);
            SendMessage(cmbMins, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbMins, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbMins, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbMins, CB_SETCURSEL, mins, 0);
            
            HWND lblM = CreateWindowW(L"STATIC", L"m",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 136, y, 18, 26, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblM, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Seconds dropdown
            HWND cmbSecs = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 158, y, 55, 300, hwnd, (HMENU)ID_CMB_SECONDS, g_windows.hInstance, NULL);
            SendMessage(cmbSecs, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbSecs, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbSecs, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbSecs, CB_SETCURSEL, secs, 0);
            
            HWND lblS = CreateWindowW(L"STATIC", L"s",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 216, y, 15, 26, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            y += rowH;
            
            // Save path - aligned with dropdowns
            HWND lblPath = CreateWindowW(L"STATIC", L"Save to",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                labelX, y + 1, labelW, 22, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblPath, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Edit control - height 22 matches font better for vertical centering
            HWND edtPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                controlX, y, controlW - 80, 22, hwnd, (HMENU)ID_EDT_PATH, g_windows.hInstance, NULL);
            SendMessage(edtPath, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SetWindowTextA(edtPath, g_config.savePath);
            
            HWND btnBrowse = CreateWindowW(L"BUTTON", L"Browse",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                controlX + controlW - 72, y, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, g_windows.hInstance, NULL);
            SendMessage(btnBrowse, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            y += rowH + 12;
            
            // ===== REPLAY BUFFER SECTION =====
            // Separator line
            CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_windows.hInstance, NULL);
            y += 14;
            
            // Enable replay checkbox
            HWND chkReplayEnabled = CreateWindowW(L"BUTTON", L"Enable Instant Replay (H.265)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 250, 24, hwnd, (HMENU)ID_CHK_REPLAY_ENABLED, g_windows.hInstance, NULL);
            SendMessage(chkReplayEnabled, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_REPLAY_ENABLED, g_config.replayEnabled ? BST_CHECKED : BST_UNCHECKED);
            y += 38;
            
            // Capture source dropdown
            HWND lblReplaySource = CreateWindowW(L"STATIC", L"Capture source",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblReplaySource, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbReplaySource = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, controlW, 200, hwnd, (HMENU)ID_CMB_REPLAY_SOURCE, g_windows.hInstance, NULL);
            SendMessage(cmbReplaySource, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Enumerate and add monitors dynamically
            int monitorCount = GetSystemMetrics(SM_CMONITORS);
            WCHAR monitorName[64];
            for (int i = 0; i < monitorCount; i++) {
                if (i == 0) {
                    wsprintfW(monitorName, L"Monitor %d (Primary)", i + 1);
                } else {
                    wsprintfW(monitorName, L"Monitor %d", i + 1);
                }
                SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)monitorName);
            }
            SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)L"All Monitors");
            SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)L"Specific Window");
            SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)L"Custom Area");
            
            // Set current selection based on config
            int sourceIndex = 0;
            if (g_config.replayCaptureSource == MODE_MONITOR) {
                // Specific monitor selected
                sourceIndex = g_config.replayMonitorIndex;
                if (sourceIndex >= monitorCount) sourceIndex = 0;
            } else if (g_config.replayCaptureSource == MODE_ALL_MONITORS) {
                sourceIndex = monitorCount;  // All Monitors is after individual monitors
            } else if (g_config.replayCaptureSource == MODE_WINDOW) {
                sourceIndex = monitorCount + 1;
            } else if (g_config.replayCaptureSource == MODE_AREA) {
                sourceIndex = monitorCount + 2;
            }
            SendMessage(cmbReplaySource, CB_SETCURSEL, sourceIndex, 0);
            y += rowH;
            
            // Aspect ratio dropdown (only enabled for monitor capture)
            HWND lblAspect = CreateWindowW(L"STATIC", L"Aspect ratio",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblAspect, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbAspect = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, controlW, 250, hwnd, (HMENU)ID_CMB_REPLAY_ASPECT, g_windows.hInstance, NULL);
            SendMessage(cmbAspect, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"Native (No change)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"16:9 (YouTube, Standard)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"9:16 (TikTok, Shorts, Reels)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"1:1 (Square - Instagram)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"4:5 (Instagram Portrait)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"16:10");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"4:3");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"21:9 (Ultrawide)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"32:9 (Super Ultrawide)");
            SendMessage(cmbAspect, CB_SETCURSEL, g_config.replayAspectRatio, 0);
            
            // Enable/disable aspect ratio based on source
            BOOL enableAspect = (g_config.replayCaptureSource == MODE_MONITOR || 
                                 g_config.replayCaptureSource == MODE_ALL_MONITORS);
            EnableWindow(cmbAspect, enableAspect);
            y += rowH;
            
            // Frame rate dropdown
            HWND lblFPS = CreateWindowW(L"STATIC", L"Frame rate",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblFPS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbFPS = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, controlW, 150, hwnd, (HMENU)ID_CMB_REPLAY_FPS, g_windows.hInstance, NULL);
            SendMessage(cmbFPS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"30 FPS");
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"60 FPS");
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"120 FPS");
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"240 FPS");
            // Set selection based on config
            int fpsIdx = (g_config.replayFPS >= 240) ? 3 : (g_config.replayFPS >= 120) ? 2 : (g_config.replayFPS >= 60) ? 1 : 0;
            SendMessage(cmbFPS, CB_SETCURSEL, fpsIdx, 0);
            y += rowH;
            
            // Buffer duration - using dropdowns for proper centering
            HWND lblReplayDuration = CreateWindowW(L"STATIC", L"Duration",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblReplayDuration, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Calculate hours, minutes, seconds from config
            int replayTotalSecs = g_config.replayDuration;
            int replayHours = replayTotalSecs / 3600;
            int replayMins = (replayTotalSecs % 3600) / 60;
            int replaySecs = replayTotalSecs % 60;
            
            // Hours dropdown
            HWND cmbReplayHours = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, 55, 300, hwnd, (HMENU)ID_CMB_REPLAY_HOURS, g_windows.hInstance, NULL);
            SendMessage(cmbReplayHours, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbReplayHours, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 24; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbReplayHours, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbReplayHours, CB_SETCURSEL, replayHours, 0);
            
            HWND lblReplayH = CreateWindowW(L"STATIC", L"h",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 58, y, 15, 26, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblReplayH, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Minutes dropdown
            HWND cmbReplayMinutes = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 78, y, 55, 300, hwnd, (HMENU)ID_CMB_REPLAY_MINS, g_windows.hInstance, NULL);
            SendMessage(cmbReplayMinutes, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbReplayMinutes, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbReplayMinutes, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbReplayMinutes, CB_SETCURSEL, replayMins, 0);
            
            HWND lblReplayM = CreateWindowW(L"STATIC", L"m",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 136, y, 18, 26, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblReplayM, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Seconds dropdown
            HWND cmbReplaySecs = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 158, y, 55, 300, hwnd, (HMENU)ID_CMB_REPLAY_SECS, g_windows.hInstance, NULL);
            SendMessage(cmbReplaySecs, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbReplaySecs, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbReplaySecs, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbReplaySecs, CB_SETCURSEL, replaySecs, 0);
            
            HWND lblReplayS = CreateWindowW(L"STATIC", L"s",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 216, y, 15, 26, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblReplayS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            y += rowH;
            
            // Save hotkey button
            HWND lblHotkey = CreateWindowW(L"STATIC", L"Save hotkey",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 6, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblHotkey, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Get current hotkey name
            char hotkeyName[64];
            GetKeyNameFromVK(g_config.replaySaveKey, hotkeyName, sizeof(hotkeyName));
            
            // Create button showing current hotkey - click to change
            HWND btnHotkey = CreateWindowExA(0, "BUTTON", hotkeyName,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                controlX, y + 1, 120, 26, hwnd, (HMENU)ID_BTN_REPLAY_HOTKEY, g_windows.hInstance, NULL);
            SendMessage(btnHotkey, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Hint text - larger and more visible
            HWND lblHotkeyHint = CreateWindowW(L"STATIC", L"(Click to change)",
                WS_CHILD | WS_VISIBLE,
                controlX + 130, y + 7, 140, 24, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblHotkeyHint, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            y += rowH;
            
            // RAM explanation and estimate
            {
                int durationSecs = g_config.replayDuration;
                int fps = g_config.replayFPS;
                int estWidth = GetSystemMetrics(SM_CXSCREEN);
                int estHeight = GetSystemMetrics(SM_CYSCREEN);
                
                // Adjust for aspect ratio if set
                if (g_config.replayAspectRatio > 0) {
                    int ratioW, ratioH;
                    GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
                    if (ratioW > 0 && ratioH > 0) {
                        if (estWidth * ratioH > estHeight * ratioW) {
                            estWidth = (estHeight * ratioW) / ratioH;
                        } else {
                            estHeight = (estWidth * ratioH) / ratioW;
                        }
                    }
                }
                
                int ramMB = ReplayBuffer_EstimateRAMUsage(durationSecs, estWidth, estHeight, fps, g_config.quality);
                
                // Explanation text
                char explainText[256];
                snprintf(explainText, sizeof(explainText), "When enabled, ~%d MB of RAM is reserved for the video buffer. See the calculation below:", ramMB);
                HWND lblExplain = CreateWindowExA(0, "STATIC", explainText,
                    WS_CHILD | WS_VISIBLE,
                    labelX, y + 4, contentW, 20, hwnd, (HMENU)ID_STATIC_REPLAY_RAM, g_windows.hInstance, NULL);
                SendMessage(lblExplain, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
                y += 32;
                
                // Calculation breakdown
                char calcText[128];
                if (durationSecs >= 60) {
                    int calcMins = durationSecs / 60;
                    int calcSecs = durationSecs % 60;
                    if (calcSecs > 0) {
                        snprintf(calcText, sizeof(calcText), "%dm %ds @ %d FPS, %dx%d = ~%d MB", calcMins, calcSecs, fps, estWidth, estHeight, ramMB);
                    } else {
                        snprintf(calcText, sizeof(calcText), "%dm @ %d FPS, %dx%d = ~%d MB", calcMins, fps, estWidth, estHeight, ramMB);
                    }
                } else {
                    snprintf(calcText, sizeof(calcText), "%ds @ %d FPS, %dx%d = ~%d MB", durationSecs, fps, estWidth, estHeight, ramMB);
                }
                
                HWND lblCalc = CreateWindowExA(0, "STATIC", calcText,
                    WS_CHILD | WS_VISIBLE,
                    labelX + 20, y, contentW - 20, 20, hwnd, (HMENU)ID_STATIC_REPLAY_CALC, g_windows.hInstance, NULL);
                SendMessage(lblCalc, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            }
            
            // ============================================================
            // AUDIO CAPTURE SECTION
            // ============================================================
            y += 35;
            
            // Divider line
            CreateWindowExA(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_windows.hInstance, NULL);
            y += 14;
            
            // Enable Audio Capture checkbox
            HWND chkAudio = CreateWindowExA(0, "BUTTON", "Enable Audio Capture",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 200, 24, hwnd, (HMENU)ID_CHK_AUDIO_ENABLED, g_windows.hInstance, NULL);
            SendMessage(chkAudio, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(chkAudio, BM_SETCHECK, g_config.audioEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            y += 38;
            
            // Enumerate audio devices - MUST be static so CB_SETITEMDATA pointers remain valid
            // after WM_CREATE returns (they point to device IDs in this struct)
            static AudioDeviceList audioDevices;
            AudioDevice_Enumerate(&audioDevices);
            
            // Audio Source 1
            int audioDropW = 260;  // Reduced dropdown width
            int sliderX = controlX + audioDropW + 10;
            int sliderW = 100;
            int volLblX = sliderX + sliderW + 5;
            int volLblW = 40;
            
            HWND lblAudio1 = CreateWindowExA(0, "STATIC", "Audio source 1",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblAudio1, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            HWND cmbAudio1 = CreateWindowExA(0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, audioDropW, 200, hwnd, (HMENU)ID_CMB_AUDIO_SOURCE1, g_windows.hInstance, NULL);
            SendMessage(cmbAudio1, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Volume slider 1
            HWND sldVol1 = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                sliderX, y + 2, sliderW, 22, hwnd, (HMENU)ID_SLD_AUDIO_VOLUME1, g_windows.hInstance, NULL);
            SendMessage(sldVol1, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessage(sldVol1, TBM_SETPOS, TRUE, g_config.audioVolume1);
            
            char volBuf1[16]; snprintf(volBuf1, sizeof(volBuf1), "%d%%", g_config.audioVolume1);
            HWND lblVol1 = CreateWindowExA(0, "STATIC", volBuf1,
                WS_CHILD | WS_VISIBLE,
                volLblX, y + 5, volLblW, 20, hwnd, (HMENU)ID_LBL_AUDIO_VOL1, g_windows.hInstance, NULL);
            SendMessage(lblVol1, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            y += rowH;
            
            // Audio Source 2
            HWND lblAudio2 = CreateWindowExA(0, "STATIC", "Audio source 2",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblAudio2, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            HWND cmbAudio2 = CreateWindowExA(0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, audioDropW, 200, hwnd, (HMENU)ID_CMB_AUDIO_SOURCE2, g_windows.hInstance, NULL);
            SendMessage(cmbAudio2, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Volume slider 2
            HWND sldVol2 = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                sliderX, y + 2, sliderW, 22, hwnd, (HMENU)ID_SLD_AUDIO_VOLUME2, g_windows.hInstance, NULL);
            SendMessage(sldVol2, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessage(sldVol2, TBM_SETPOS, TRUE, g_config.audioVolume2);
            
            char volBuf2[16]; snprintf(volBuf2, sizeof(volBuf2), "%d%%", g_config.audioVolume2);
            HWND lblVol2 = CreateWindowExA(0, "STATIC", volBuf2,
                WS_CHILD | WS_VISIBLE,
                volLblX, y + 5, volLblW, 20, hwnd, (HMENU)ID_LBL_AUDIO_VOL2, g_windows.hInstance, NULL);
            SendMessage(lblVol2, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            y += rowH;
            
            // Audio Source 3
            HWND lblAudio3 = CreateWindowExA(0, "STATIC", "Audio source 3",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblAudio3, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            HWND cmbAudio3 = CreateWindowExA(0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, audioDropW, 200, hwnd, (HMENU)ID_CMB_AUDIO_SOURCE3, g_windows.hInstance, NULL);
            SendMessage(cmbAudio3, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Volume slider 3
            HWND sldVol3 = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                sliderX, y + 2, sliderW, 22, hwnd, (HMENU)ID_SLD_AUDIO_VOLUME3, g_windows.hInstance, NULL);
            SendMessage(sldVol3, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessage(sldVol3, TBM_SETPOS, TRUE, g_config.audioVolume3);
            
            char volBuf3[16]; snprintf(volBuf3, sizeof(volBuf3), "%d%%", g_config.audioVolume3);
            HWND lblVol3 = CreateWindowExA(0, "STATIC", volBuf3,
                WS_CHILD | WS_VISIBLE,
                volLblX, y + 5, volLblW, 20, hwnd, (HMENU)ID_LBL_AUDIO_VOL3, g_windows.hInstance, NULL);
            SendMessage(lblVol3, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Populate audio dropdowns using helper function
            SendMessage(cmbAudio1, CB_SETCURSEL, PopulateAudioDropdown(cmbAudio1, &audioDevices, g_config.audioSource1), 0);
            SendMessage(cmbAudio2, CB_SETCURSEL, PopulateAudioDropdown(cmbAudio2, &audioDevices, g_config.audioSource2), 0);
            SendMessage(cmbAudio3, CB_SETCURSEL, PopulateAudioDropdown(cmbAudio3, &audioDevices, g_config.audioSource3), 0);
            
            // ============================================================
            // DEBUG SETTINGS SECTION
            // ============================================================
            y += 35;
            
            // Divider line
            CreateWindowExA(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_windows.hInstance, NULL);
            y += 14;
            
            // Enable Debug Logging checkbox
            HWND chkDebug = CreateWindowExA(0, "BUTTON", "Enable Debug Logging",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 200, 24, hwnd, (HMENU)ID_CHK_DEBUG_LOGGING, g_windows.hInstance, NULL);
            SendMessage(chkDebug, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(chkDebug, BM_SETCHECK, g_config.debugLogging ? BST_CHECKED : BST_UNCHECKED, 0);
            
            // Add description text
            HWND lblDebugInfo = CreateWindowExA(0, "STATIC", "(Logs are saved to Debug folder next to exe)",
                WS_CHILD | WS_VISIBLE,
                labelX + 205, y + 4, 300, 20, hwnd, NULL, g_windows.hInstance, NULL);
            SendMessage(lblDebugInfo, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Initialize preview border and area selector
            PreviewBorder_Init(g_windows.hInstance);
            AreaSelector_Init(g_windows.hInstance);
            
            // Show preview for current capture source
            UpdateReplayPreview();
            
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
                        // Update RAM estimate since bitrate changed
                        UpdateReplayRAMEstimate(hwnd);
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
                
                // Replay buffer settings handlers
                case ID_CHK_REPLAY_ENABLED: {
                    BOOL wasEnabled = g_config.replayEnabled;
                    g_config.replayEnabled = IsDlgButtonChecked(hwnd, ID_CHK_REPLAY_ENABLED) == BST_CHECKED;
                    Logger_Log("Replay enabled toggled: %d -> %d\n", wasEnabled, g_config.replayEnabled);
                    
                    // Start or stop replay buffer based on new state
                    if (g_config.replayEnabled && !wasEnabled) {
                        // Starting replay buffer
                        Logger_Log("Starting replay buffer from settings\n");
                        ReplayBuffer_Start(&g_replayBuffer, &g_config);
                        CheckAudioError();
                        BOOL ok = RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
                        Logger_Log("RegisterHotKey from settings: %s (key=0x%02X)\n", ok ? "OK" : "FAILED", g_config.replaySaveKey);
                    } else if (!g_config.replayEnabled && wasEnabled) {
                        // Stopping replay buffer
                        Logger_Log("Stopping replay buffer from settings\n");
                        UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
                        ReplayBuffer_Stop(&g_replayBuffer);
                    }
                    break;
                }
                    
                case ID_CMB_REPLAY_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int sel = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SOURCE), CB_GETCURSEL, 0, 0);
                        int monCount = GetSystemMetrics(SM_CMONITORS);
                        
                        if (sel < monCount) {
                            // Individual monitor selected
                            g_config.replayCaptureSource = MODE_MONITOR;
                            g_config.replayMonitorIndex = sel;
                        } else if (sel == monCount) {
                            g_config.replayCaptureSource = MODE_ALL_MONITORS;
                        } else if (sel == monCount + 1) {
                            g_config.replayCaptureSource = MODE_WINDOW;
                        } else {
                            g_config.replayCaptureSource = MODE_AREA;
                        }
                        
                        // Enable/disable aspect ratio dropdown
                        BOOL enableAspect = (g_config.replayCaptureSource == MODE_MONITOR || 
                                             g_config.replayCaptureSource == MODE_ALL_MONITORS);
                        EnableWindow(GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT), enableAspect);
                        
                        // Update preview border/area selector
                        UpdateReplayPreview();
                    }
                    break;
                    
                case ID_CMB_REPLAY_HOURS:
                case ID_CMB_REPLAY_MINS:
                case ID_CMB_REPLAY_SECS:
                    // Duration is read on settings close, but update RAM estimate live
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int h = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
                        int m = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
                        int s = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
                        int total = h * 3600 + m * 60 + s;
                        if (total < 1) total = 1;
                        // Update config immediately so RAM estimate is accurate
                        g_config.replayDuration = total;
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                    
                case ID_CMB_REPLAY_ASPECT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.replayAspectRatio = (int)SendMessage(
                            GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT), CB_GETCURSEL, 0, 0);
                        
                        // Force recalculation of area for new aspect ratio
                        if (g_config.replayAspectRatio > 0) {
                            // Invalidate saved area to force recalc
                            g_config.replayAreaRect.left = 0;
                            g_config.replayAreaRect.top = 0;
                            g_config.replayAreaRect.right = 0;
                            g_config.replayAreaRect.bottom = 0;
                        }
                        
                        // Update preview
                        UpdateReplayPreview();
                        
                        // Update RAM estimate (resolution changes with aspect ratio)
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                
                case ID_CMB_REPLAY_FPS:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_FPS), CB_GETCURSEL, 0, 0);
                        int fpsValues[] = { 30, 60, 120, 240 };
                        g_config.replayFPS = (idx >= 0 && idx < 4) ? fpsValues[idx] : 60;
                        
                        // Update RAM estimate
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                    
                case ID_BTN_REPLAY_HOTKEY:
                    // Enter hotkey capture mode
                    g_interaction.waitingForHotkey = TRUE;
                    SetWindowTextA(GetDlgItem(hwnd, ID_BTN_REPLAY_HOTKEY), "Press a key...");
                    SetFocus(hwnd);  // Focus the window to receive key events
                    break;
                    
                case ID_CHK_AUDIO_ENABLED:
                    if (IsDlgButtonChecked(hwnd, ID_CHK_AUDIO_ENABLED) == BST_CHECKED) {
                        // User is trying to enable audio - check if encoder is available
                        if (!AACEncoder_IsAvailable()) {
                            MessageBoxA(hwnd,
                                "AAC audio encoder is not available on this system.\n\n"
                                "Audio recording requires the Microsoft AAC encoder which is\n"
                                "included with Windows Media Feature Pack.\n\n"
                                "On Windows N/KN editions, install the Media Feature Pack from:\n"
                                "Settings > Apps > Optional features > Add a feature",
                                "Audio Encoder Not Available",
                                MB_OK | MB_ICONWARNING);
                            // Uncheck the checkbox
                            SendMessage(GetDlgItem(hwnd, ID_CHK_AUDIO_ENABLED), BM_SETCHECK, BST_UNCHECKED, 0);
                            g_config.audioEnabled = FALSE;
                        } else {
                            g_config.audioEnabled = TRUE;
                        }
                    } else {
                        g_config.audioEnabled = FALSE;
                    }
                    break;
                
                case ID_CHK_DEBUG_LOGGING:
                    g_config.debugLogging = (IsDlgButtonChecked(hwnd, ID_CHK_DEBUG_LOGGING) == BST_CHECKED);
                    // Live toggle: start or stop logging immediately
                    if (g_config.debugLogging && !Logger_IsInitialized()) {
                        // Start logging
                        char exePath[MAX_PATH];
                        char debugFolder[MAX_PATH];
                        char logFilename[MAX_PATH];
                        GetModuleFileNameA(NULL, exePath, MAX_PATH);
                        char* lastSlash = strrchr(exePath, '\\');
                        if (lastSlash) {
                            *lastSlash = '\0';
                            snprintf(debugFolder, sizeof(debugFolder), "%s\\Debug", exePath);
                            CreateDirectoryA(debugFolder, NULL);
                            SYSTEMTIME st;
                            GetLocalTime(&st);
                            snprintf(logFilename, sizeof(logFilename), "%s\\lwsr_log_%04d%02d%02d_%02d%02d%02d.txt",
                                    debugFolder, (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
                            Logger_Init(logFilename, "w");
                            Logger_Log("Debug logging enabled (live toggle)\n");
                        }
                    } else if (!g_config.debugLogging && Logger_IsInitialized()) {
                        // Stop logging
                        Logger_Log("Debug logging disabled (live toggle)\n");
                        Logger_Shutdown();
                    }
                    break;
                    
                case ID_CMB_AUDIO_SOURCE1:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE1), CB_GETCURSEL, 0, 0);
                        char* deviceId = (char*)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE1), CB_GETITEMDATA, idx, 0);
                        if (deviceId && deviceId != (char*)CB_ERR) {
                            strncpy(g_config.audioSource1, deviceId, sizeof(g_config.audioSource1) - 1);
                        } else {
                            g_config.audioSource1[0] = '\0';
                        }
                        // Restart replay buffer to apply new audio source
                        if (g_replayBuffer.isBuffering) {
                            ReplayBuffer_Stop(&g_replayBuffer);
                            ReplayBuffer_Start(&g_replayBuffer, &g_config);
                            CheckAudioError();
                        }
                    }
                    break;
                    
                case ID_CMB_AUDIO_SOURCE2:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE2), CB_GETCURSEL, 0, 0);
                        char* deviceId = (char*)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE2), CB_GETITEMDATA, idx, 0);
                        if (deviceId && deviceId != (char*)CB_ERR) {
                            strncpy(g_config.audioSource2, deviceId, sizeof(g_config.audioSource2) - 1);
                        } else {
                            g_config.audioSource2[0] = '\0';
                        }
                        // Restart replay buffer to apply new audio source
                        if (g_replayBuffer.isBuffering) {
                            ReplayBuffer_Stop(&g_replayBuffer);
                            ReplayBuffer_Start(&g_replayBuffer, &g_config);
                            CheckAudioError();
                        }
                    }
                    break;
                    
                case ID_CMB_AUDIO_SOURCE3:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE3), CB_GETCURSEL, 0, 0);
                        char* deviceId = (char*)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE3), CB_GETITEMDATA, idx, 0);
                        if (deviceId && deviceId != (char*)CB_ERR) {
                            strncpy(g_config.audioSource3, deviceId, sizeof(g_config.audioSource3) - 1);
                        } else {
                            g_config.audioSource3[0] = '\0';
                        }
                        // Restart replay buffer to apply new audio source
                        if (g_replayBuffer.isBuffering) {
                            ReplayBuffer_Stop(&g_replayBuffer);
                            ReplayBuffer_Start(&g_replayBuffer, &g_config);
                            CheckAudioError();
                        }
                    }
                    break;
            }
            return 0;
        
        case WM_HSCROLL: {
            // Handle volume slider changes
            HWND hSlider = (HWND)lParam;
            int ctrlId = GetDlgCtrlID(hSlider);
            int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
            char buf[16];
            
            if (ctrlId == ID_SLD_AUDIO_VOLUME1) {
                g_config.audioVolume1 = pos;
                snprintf(buf, sizeof(buf), "%d%%", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUDIO_VOL1), buf);
            } else if (ctrlId == ID_SLD_AUDIO_VOLUME2) {
                g_config.audioVolume2 = pos;
                snprintf(buf, sizeof(buf), "%d%%", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUDIO_VOL2), buf);
            } else if (ctrlId == ID_SLD_AUDIO_VOLUME3) {
                g_config.audioVolume3 = pos;
                snprintf(buf, sizeof(buf), "%d%%", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUDIO_VOL3), buf);
            }
            return 0;
        }
        
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:  // For Alt combinations
            if (g_interaction.waitingForHotkey) {
                // Get the virtual key code
                int vk = (int)wParam;
                
                // Ignore modifier keys alone
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || 
                    vk == VK_LSHIFT || vk == VK_RSHIFT || 
                    vk == VK_LCONTROL || vk == VK_RCONTROL ||
                    vk == VK_LMENU || vk == VK_RMENU) {
                    return 0;
                }
                
                // Unregister old hotkey if replay is enabled
                if (g_config.replayEnabled) {
                    Logger_Log("Unregistering old hotkey (key change)\n");
                    UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
                }
                
                // Save the new hotkey
                g_config.replaySaveKey = vk;
                Logger_Log("Hotkey changed to VK=0x%02X\n", vk);
                
                // Re-register with new hotkey if replay is enabled
                if (g_config.replayEnabled) {
                    BOOL ok = RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
                    Logger_Log("RegisterHotKey (key change): %s (key=0x%02X)\n", ok ? "OK" : "FAILED", g_config.replaySaveKey);
                }
                
                // Update button text with key name
                char keyName[64];
                GetKeyNameFromVK(vk, keyName, sizeof(keyName));
                SetWindowTextA(GetDlgItem(hwnd, ID_BTN_REPLAY_HOTKEY), keyName);
                
                g_interaction.waitingForHotkey = FALSE;
                return 0;
            }
            break;
            
        case WM_CLOSE: {
            // Save area selector position if visible
            SaveAreaSelectorPosition();
            
            // Hide preview overlays
            PreviewBorder_Hide();
            AreaSelector_Hide();
            
            // Save time limit from dropdowns
            int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
            int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
            int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
            int total = hours * 3600 + mins * 60 + secs;
            if (total < 1) total = 1;
            g_config.maxRecordingSeconds = total;
            
            // Save replay duration from dropdowns (simple: 3 boxes -> 1 number)
            int rh = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
            int rm = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
            int rs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
            int replayTotal = rh * 3600 + rm * 60 + rs;
            if (replayTotal < 1) replayTotal = 1;
            g_config.replayDuration = replayTotal;
            
            // Save path
            GetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), g_config.savePath, MAX_PATH);
            Config_Save(&g_config);
            
            // Clean up preview overlays
            PreviewBorder_Shutdown();
            AreaSelector_Shutdown();
            
            // Clean up fonts and brushes
            if (g_settingsFont) { DeleteObject(g_settingsFont); g_settingsFont = NULL; }
            if (g_settingsSmallFont) { DeleteObject(g_settingsSmallFont); g_settingsSmallFont = NULL; }
            if (g_settingsBgBrush) { DeleteObject(g_settingsBgBrush); g_settingsBgBrush = NULL; }
            
            DestroyWindow(hwnd);
            g_windows.settingsWnd = NULL;
            
            // Refresh settings button to remove highlight
            if (g_controlWnd) {
                HWND settingsBtn = GetDlgItem(g_controlWnd, ID_BTN_SETTINGS);
                if (settingsBtn) InvalidateRect(settingsBtn, NULL, TRUE);
            }
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
            if (!IsRectEmpty(&g_selection.selectedRect)) {
                char sizeText[64];
                int w = g_selection.selectedRect.right - g_selection.selectedRect.left;
                int h = g_selection.selectedRect.bottom - g_selection.selectedRect.top;
                snprintf(sizeText, sizeof(sizeText), "%d x %d", w, h);
                
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


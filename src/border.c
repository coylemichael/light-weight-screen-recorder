/*
 * Recording Border Highlight Implementation
 * Uses a layered window with per-pixel alpha for a clean red border
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation checks
 * - Window creation failures return FALSE
 * - GDI resource cleanup in shutdown function
 * - Returns BOOL to propagate errors; callers must check
 */

#include "border.h"
#include "constants.h"
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM

/* ============================================================================
 * RECORDING BORDER STATE
 * ============================================================================
 * Static globals for the red recording border.
 * Thread Access: [Main thread only - UI operations]
 */

/*
 * RecordingBorderState - State for the red recording border
 * Used during active recording to highlight the capture area.
 */
typedef struct RecordingBorderState {
    HINSTANCE hInstance;
    HWND wnd;
    BOOL isVisible;
    RECT currentRect;
} RecordingBorderState;

static RecordingBorderState g_recording = {0};

// Forward declarations
static LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void UpdateBorderBitmap(int width, int height);

BOOL Border_Init(HINSTANCE hInstance) {
    if (!hInstance) return FALSE;
    
    g_recording.hInstance = hInstance;
    
    // Register window class
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = BorderWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "LWSRBorder";
    
    if (!RegisterClassExA(&wc)) {
        // Class might already be registered
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return FALSE;
        }
    }
    
    // Create the border window
    // WS_EX_TRANSPARENT makes it click-through
    // WS_EX_LAYERED allows per-pixel alpha
    g_recording.wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LWSRBorder",
        NULL,
        WS_POPUP,
        -9999, -9999, 1, 1,  // Start off-screen to avoid initial flash
        NULL, NULL, hInstance, NULL
    );
    
    // Note: SetWindowDisplayAffinity doesn't work with ULW_ALPHA layered windows
    // The border will appear in recordings - this is expected behavior
    
    return g_recording.wnd != NULL;
}

void Border_Shutdown(void) {
    if (g_recording.wnd) {
        DestroyWindow(g_recording.wnd);
        g_recording.wnd = NULL;
    }
    g_recording.isVisible = FALSE;
}

// Create and set a 32-bit ARGB bitmap for the border with transparency
static void UpdateBorderBitmap(int width, int height) {
    if (!g_recording.wnd || width < 1 || height < 1) return;
    
    HDC screenDC = NULL;
    HDC memDC = NULL;
    HBITMAP hBitmap = NULL;
    HBITMAP oldBitmap = NULL;
    
    // Create a compatible DC
    screenDC = GetDC(NULL);
    if (!screenDC) return;
    
    memDC = CreateCompatibleDC(screenDC);
    if (!memDC) goto cleanup;
    
    // Create a 32-bit DIB section
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    BYTE* pixels = NULL;
    hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    if (!hBitmap || !pixels) goto cleanup;
    
    oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    // Clear to transparent
    memset(pixels, 0, width * height * BYTES_PER_PIXEL_BGRA);
    
    // Draw red border with full alpha
    // BGRA format: Blue, Green, Red, Alpha
    BYTE r = BORDER_COLOR_R, g = BORDER_COLOR_G, b = BORDER_COLOR_B, a = 255;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            BOOL isBorder = (x < BORDER_THICKNESS || x >= width - BORDER_THICKNESS ||
                            y < BORDER_THICKNESS || y >= height - BORDER_THICKNESS);
            
            if (isBorder) {
                int idx = (y * width + x) * 4;
                // Pre-multiplied alpha for UpdateLayeredWindow
                pixels[idx + 0] = (BYTE)(b * a / 255); // Blue
                pixels[idx + 1] = (BYTE)(g * a / 255); // Green
                pixels[idx + 2] = (BYTE)(r * a / 255); // Red
                pixels[idx + 3] = a;                    // Alpha
            }
        }
    }
    
    // Update the layered window
    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {width, height};
    BLENDFUNCTION blend = {0};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    
    POINT ptDst = {g_recording.currentRect.left, g_recording.currentRect.top};
    
    UpdateLayeredWindow(g_recording.wnd, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
cleanup:
    if (oldBitmap) SelectObject(memDC, oldBitmap);
    if (hBitmap) DeleteObject(hBitmap);
    if (memDC) DeleteDC(memDC);
    if (screenDC) ReleaseDC(NULL, screenDC);
}

void Border_Show(RECT captureRect) {
    if (!g_recording.wnd) return;
    
    // Position border around capture area
    int pad = BORDER_THICKNESS;
    g_recording.currentRect.left = captureRect.left - pad;
    g_recording.currentRect.top = captureRect.top - pad;
    g_recording.currentRect.right = captureRect.right + pad;
    g_recording.currentRect.bottom = captureRect.bottom + pad;
    
    int width = g_recording.currentRect.right - g_recording.currentRect.left;
    int height = g_recording.currentRect.bottom - g_recording.currentRect.top;
    
    // Update the bitmap with the new size
    UpdateBorderBitmap(width, height);
    
    ShowWindow(g_recording.wnd, SW_SHOWNA); // Show without activating
    g_recording.isVisible = TRUE;
}

void Border_Hide(void) {
    if (g_recording.wnd) {
        ShowWindow(g_recording.wnd, SW_HIDE);
    }
    g_recording.isVisible = FALSE;
}

static LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            // Make window completely click-through
            return HTTRANSPARENT;
            
        case WM_ERASEBKGND:
            return 1; /* Prevent background erase */
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ============================================================================
 * PREVIEW BORDER STATE (for settings window)
 * ============================================================================
 * Shows a dashed blue preview of the capture area.
 * Thread Access: [Main thread only - UI operations]
 */

/*
 * PreviewBorderState - State for the preview border in settings
 */
typedef struct PreviewBorderState {
    HWND wnd;
    BOOL visible;
    RECT rect;
} PreviewBorderState;

static PreviewBorderState g_preview = {0};

BOOL PreviewBorder_Init(HINSTANCE hInstance) {
    if (!hInstance) return FALSE;
    
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = BorderWndProc;  // Reuse same proc - click-through
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "LWSRPreviewBorder";
    
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return FALSE;
    }
    
    g_preview.wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LWSRPreviewBorder", NULL, WS_POPUP,
        -9999, -9999, 1, 1,
        NULL, NULL, hInstance, NULL
    );
    
    return g_preview.wnd != NULL;
}

void PreviewBorder_Shutdown(void) {
    if (g_preview.wnd) {
        DestroyWindow(g_preview.wnd);
        g_preview.wnd = NULL;
    }
    g_preview.visible = FALSE;
}

void PreviewBorder_Hide(void) {
    if (g_preview.wnd) {
        ShowWindow(g_preview.wnd, SW_HIDE);
    }
    g_preview.visible = FALSE;
}

/* ============================================================================
 * AREA SELECTOR STATE (draggable capture region)
 * ============================================================================
 * Provides a resizable/draggable rectangle for custom area selection.
 * Thread Access: [Main thread only - UI operations]
 */

/*
 * AreaSelectorState - State for the draggable capture region selector
 */
typedef struct AreaSelectorState {
    HWND wnd;
    BOOL visible;
    RECT rect;
    BOOL dragging;
    POINT dragOffset;
    int resizeEdge;    /* 0=none, 1=left, 2=right, 4=top, 8=bottom (combinable) */
    BOOL locked;       /* When TRUE, cannot move or resize */
} AreaSelectorState;

static AreaSelectorState g_area = {0};

#define RESIZE_HANDLE_SIZE 8
#define MIN_AREA_SIZE 100

static int GetResizeEdge(HWND hwnd, int x, int y) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int edge = 0;
    
    if (x < RESIZE_HANDLE_SIZE) edge |= 1;       // Left
    if (x >= rc.right - RESIZE_HANDLE_SIZE) edge |= 2;  // Right
    if (y < RESIZE_HANDLE_SIZE) edge |= 4;       // Top
    if (y >= rc.bottom - RESIZE_HANDLE_SIZE) edge |= 8; // Bottom
    
    return edge;
}

static LRESULT CALLBACK AreaSelectorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            // Semi-transparent fill
            HBRUSH fillBrush = CreateSolidBrush(RGB(255, 50, 50));
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, fillBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
            HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
            
            Rectangle(hdc, 0, 0, rc.right, rc.bottom);
            
            // Draw resize handles at corners (only if not locked)
            if (!g_area.locked) {
                HBRUSH handleBrush = CreateSolidBrush(RGB(255, 255, 255));
                HPEN handlePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));  // White pen to match fill
                SelectObject(hdc, handleBrush);
                SelectObject(hdc, handlePen);
                
                // Corner handles
                Rectangle(hdc, 0, 0, RESIZE_HANDLE_SIZE, RESIZE_HANDLE_SIZE);
                Rectangle(hdc, rc.right - RESIZE_HANDLE_SIZE, 0, rc.right, RESIZE_HANDLE_SIZE);
                Rectangle(hdc, 0, rc.bottom - RESIZE_HANDLE_SIZE, RESIZE_HANDLE_SIZE, rc.bottom);
                Rectangle(hdc, rc.right - RESIZE_HANDLE_SIZE, rc.bottom - RESIZE_HANDLE_SIZE, rc.right, rc.bottom);
                DeleteObject(handleBrush);
                DeleteObject(handlePen);
                SelectObject(hdc, borderPen);  // Restore border pen
            }
            
            // Draw text in center
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                                     ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(hdc, font);
            const char* text = g_area.locked ? "Capture Area" : "Drag to move, corners to resize";
            DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            SelectObject(hdc, oldFont);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(font);
            DeleteObject(fillBrush);
            DeleteObject(borderPen);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_SETCURSOR: {
            // If locked, just show arrow cursor
            if (g_area.locked) {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
            }
            
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int edge = GetResizeEdge(hwnd, pt.x, pt.y);
            
            LPCTSTR cursor = IDC_ARROW;
            if (edge == 5 || edge == 10) cursor = IDC_SIZENWSE;      // Top-left or bottom-right
            else if (edge == 6 || edge == 9) cursor = IDC_SIZENESW;  // Top-right or bottom-left
            else if (edge == 1 || edge == 2) cursor = IDC_SIZEWE;    // Left or right
            else if (edge == 4 || edge == 8) cursor = IDC_SIZENS;    // Top or bottom
            else cursor = IDC_SIZEALL;  // Move
            
            SetCursor(LoadCursor(NULL, cursor));
            return TRUE;
        }
        
        case WM_LBUTTONDOWN: {
            // Don't allow dragging if locked
            if (g_area.locked) return 0;
            
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            
            g_area.resizeEdge = GetResizeEdge(hwnd, x, y);
            g_area.dragging = TRUE;
            
            POINT pt;
            GetCursorPos(&pt);
            g_area.dragOffset.x = pt.x - g_area.rect.left;
            g_area.dragOffset.y = pt.y - g_area.rect.top;
            
            SetCapture(hwnd);
            return 0;
        }
        
        case WM_MOUSEMOVE:
            if (g_area.dragging) {
                POINT pt;
                GetCursorPos(&pt);
                
                int width = g_area.rect.right - g_area.rect.left;
                int height = g_area.rect.bottom - g_area.rect.top;
                
                if (g_area.resizeEdge == 0) {
                    // Move
                    g_area.rect.left = pt.x - g_area.dragOffset.x;
                    g_area.rect.top = pt.y - g_area.dragOffset.y;
                    g_area.rect.right = g_area.rect.left + width;
                    g_area.rect.bottom = g_area.rect.top + height;
                } else {
                    // Resize
                    if (g_area.resizeEdge & 1) g_area.rect.left = min(pt.x, g_area.rect.right - MIN_AREA_SIZE);
                    if (g_area.resizeEdge & 2) g_area.rect.right = max(pt.x, g_area.rect.left + MIN_AREA_SIZE);
                    if (g_area.resizeEdge & 4) g_area.rect.top = min(pt.y, g_area.rect.bottom - MIN_AREA_SIZE);
                    if (g_area.resizeEdge & 8) g_area.rect.bottom = max(pt.y, g_area.rect.top + MIN_AREA_SIZE);
                }
                
                SetWindowPos(hwnd, NULL, g_area.rect.left, g_area.rect.top,
                            g_area.rect.right - g_area.rect.left,
                            g_area.rect.bottom - g_area.rect.top,
                            SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        
        case WM_LBUTTONUP:
            g_area.dragging = FALSE;
            g_area.resizeEdge = 0;
            ReleaseCapture();
            return 0;
        
        case WM_MOUSEACTIVATE:
            // Prevent this window from being activated by mouse clicks
            return MA_NOACTIVATE;
        
        case WM_ERASEBKGND:
            return 1;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL AreaSelector_Init(HINSTANCE hInstance) {
    if (!hInstance) return FALSE;
    
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AreaSelectorWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "LWSRAreaSelector";
    
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return FALSE;
    }
    
    // Don't create window until needed
    return TRUE;
}

void AreaSelector_Shutdown(void) {
    if (g_area.wnd) {
        DestroyWindow(g_area.wnd);
        g_area.wnd = NULL;
    }
    g_area.visible = FALSE;
}

void AreaSelector_Show(RECT initialRect, BOOL allowMove) {
    // Set locked state
    g_area.locked = !allowMove;
    
    // Ensure minimum size (only if movable)
    if (allowMove && (initialRect.right - initialRect.left < MIN_AREA_SIZE)) {
        initialRect.right = initialRect.left + 400;
        initialRect.bottom = initialRect.top + 300;
    }
    
    g_area.rect = initialRect;
    
    if (!g_area.wnd) {
        g_area.wnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "LWSRAreaSelector", NULL,
            WS_POPUP | WS_VISIBLE,
            g_area.rect.left, g_area.rect.top,
            g_area.rect.right - g_area.rect.left,
            g_area.rect.bottom - g_area.rect.top,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );
        if (!g_area.wnd) return;
    } else {
        SetWindowPos(g_area.wnd, HWND_TOPMOST, 
                    g_area.rect.left, g_area.rect.top,
                    g_area.rect.right - g_area.rect.left,
                    g_area.rect.bottom - g_area.rect.top,
                    SWP_SHOWWINDOW | SWP_NOACTIVATE);
        // Force repaint to update text and handles
        InvalidateRect(g_area.wnd, NULL, TRUE);
    }
    
    // Make it semi-transparent (need WS_EX_LAYERED first)
    SetWindowLongPtr(g_area.wnd, GWL_EXSTYLE, 
                     GetWindowLongPtr(g_area.wnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(g_area.wnd, 0, 100, LWA_ALPHA);
    
    g_area.visible = TRUE;
}

void AreaSelector_Hide(void) {
    if (g_area.wnd) {
        ShowWindow(g_area.wnd, SW_HIDE);
    }
    g_area.visible = FALSE;
}

BOOL AreaSelector_GetRect(RECT* outRect) {
    if (!g_area.visible || !outRect) return FALSE;
    *outRect = g_area.rect;
    return TRUE;
}

BOOL AreaSelector_IsVisible(void) {
    return g_area.visible;
}

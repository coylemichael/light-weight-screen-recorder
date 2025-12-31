/*
 * Recording Border Highlight Implementation
 * Uses a layered window with per-pixel alpha for a clean red border
 */

#include "border.h"
#include <stdlib.h>

// Module state
static HINSTANCE g_hInstance = NULL;
static HWND g_borderWnd = NULL;
static BOOL g_isVisible = FALSE;
static RECT g_currentRect = {0};

// Forward declarations
static LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void UpdateBorderBitmap(int width, int height);

BOOL Border_Init(HINSTANCE hInstance) {
    g_hInstance = hInstance;
    
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
    g_borderWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LWSRBorder",
        NULL,
        WS_POPUP,
        -9999, -9999, 1, 1,  // Start off-screen to avoid initial flash
        NULL, NULL, hInstance, NULL
    );
    
    // Note: SetWindowDisplayAffinity doesn't work with ULW_ALPHA layered windows
    // The border will appear in recordings - this is expected behavior
    
    return g_borderWnd != NULL;
}

void Border_Shutdown(void) {
    if (g_borderWnd) {
        DestroyWindow(g_borderWnd);
        g_borderWnd = NULL;
    }
    g_isVisible = FALSE;
}

// Create and set a 32-bit ARGB bitmap for the border with transparency
static void UpdateBorderBitmap(int width, int height) {
    if (!g_borderWnd || width < 1 || height < 1) return;
    
    // Create a compatible DC
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    
    // Create a 32-bit DIB section
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    BYTE* pixels = NULL;
    HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    
    if (!hBitmap || !pixels) {
        DeleteDC(memDC);
        ReleaseDC(NULL, screenDC);
        return;
    }
    
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    // Clear to transparent
    memset(pixels, 0, width * height * 4);
    
    // Draw red border with full alpha
    // BGRA format: Blue, Green, Red, Alpha
    BYTE r = 220, g = 50, b = 50, a = 255;
    
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
    
    POINT ptDst = {g_currentRect.left, g_currentRect.top};
    
    UpdateLayeredWindow(g_borderWnd, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
    // Cleanup
    SelectObject(memDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
}

void Border_Show(RECT captureRect) {
    if (!g_borderWnd) return;
    
    // Position border around capture area
    int pad = BORDER_THICKNESS;
    g_currentRect.left = captureRect.left - pad;
    g_currentRect.top = captureRect.top - pad;
    g_currentRect.right = captureRect.right + pad;
    g_currentRect.bottom = captureRect.bottom + pad;
    
    int width = g_currentRect.right - g_currentRect.left;
    int height = g_currentRect.bottom - g_currentRect.top;
    
    // Update the bitmap with the new size
    UpdateBorderBitmap(width, height);
    
    ShowWindow(g_borderWnd, SW_SHOWNA); // Show without activating
    g_isVisible = TRUE;
}

void Border_Hide(void) {
    if (g_borderWnd) {
        ShowWindow(g_borderWnd, SW_HIDE);
    }
    g_isVisible = FALSE;
}

BOOL Border_IsVisible(void) {
    return g_isVisible;
}

static LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            // Make window completely click-through
            return HTTRANSPARENT;
            
        case WM_ERASEBKGND:
            return 1; // Prevent background erase
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

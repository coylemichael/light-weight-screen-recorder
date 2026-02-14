/*
 * layered_window.c - DIB + UpdateLayeredWindow helper for transparent overlays
 */

#include "layered_window.h"

BOOL LayeredBitmap_Create(LayeredBitmap* lb, int width, int height) {
    if (!lb || width <= 0 || height <= 0) return FALSE;
    
    // Zero out struct
    memset(lb, 0, sizeof(LayeredBitmap));
    lb->width = width;
    lb->height = height;
    
    // Get screen DC
    lb->screenDC = GetDC(NULL);
    if (!lb->screenDC) return FALSE;
    
    // Create memory DC
    lb->memDC = CreateCompatibleDC(lb->screenDC);
    if (!lb->memDC) {
        ReleaseDC(NULL, lb->screenDC);
        lb->screenDC = NULL;
        return FALSE;
    }
    
    // Create 32-bit top-down DIB
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down (negative)
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    lb->hBitmap = CreateDIBSection(lb->screenDC, &bmi, DIB_RGB_COLORS, 
                                    (void**)&lb->pixels, NULL, 0);
    if (!lb->hBitmap || !lb->pixels) {
        DeleteDC(lb->memDC);
        ReleaseDC(NULL, lb->screenDC);
        memset(lb, 0, sizeof(LayeredBitmap));
        return FALSE;
    }
    
    lb->hOldBitmap = (HBITMAP)SelectObject(lb->memDC, lb->hBitmap);
    
    // Pixels are already zeroed by CreateDIBSection (fully transparent)
    return TRUE;
}

void LayeredBitmap_Apply(LayeredBitmap* lb, HWND hwnd, int x, int y) {
    if (!lb || !lb->hBitmap || !hwnd) return;
    
    POINT ptSrc = {0, 0};
    POINT ptDst = {x, y};
    SIZE sizeWnd = {lb->width, lb->height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    
    UpdateLayeredWindow(hwnd, lb->screenDC, &ptDst, &sizeWnd, 
                        lb->memDC, &ptSrc, 0, &blend, ULW_ALPHA);
}

void LayeredBitmap_Destroy(LayeredBitmap* lb) {
    if (!lb) return;
    
    if (lb->hOldBitmap && lb->memDC) {
        SelectObject(lb->memDC, lb->hOldBitmap);
    }
    if (lb->hBitmap) {
        DeleteObject(lb->hBitmap);
    }
    if (lb->memDC) {
        DeleteDC(lb->memDC);
    }
    if (lb->screenDC) {
        ReleaseDC(NULL, lb->screenDC);
    }
    
    memset(lb, 0, sizeof(LayeredBitmap));
}

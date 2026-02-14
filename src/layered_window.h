/*
 * layered_window.h - DIB + UpdateLayeredWindow helper for transparent overlays
 */

#ifndef LAYERED_WINDOW_H
#define LAYERED_WINDOW_H

#include <windows.h>

/*
 * LayeredBitmap - Manages a 32-bit BGRA DIB for layered windows
 * 
 * Usage:
 *   LayeredBitmap lb = {0};
 *   if (LayeredBitmap_Create(&lb, width, height)) {
 *       // Draw to lb.pixels (BGRA, top-down) or lb.memDC
 *       LayeredBitmap_Apply(&lb, hwnd, x, y);
 *       LayeredBitmap_Destroy(&lb);
 *   }
 */
typedef struct {
    HDC screenDC;       // Screen DC (must release)
    HDC memDC;          // Memory DC for drawing
    HBITMAP hBitmap;    // DIB section
    HBITMAP hOldBitmap; // Previous bitmap (for cleanup)
    BYTE* pixels;       // Direct pixel access (BGRA, top-down)
    int width;
    int height;
} LayeredBitmap;

/*
 * Create a 32-bit BGRA DIB section for layered window rendering.
 * Pixels are zeroed (fully transparent).
 * Returns TRUE on success.
 */
BOOL LayeredBitmap_Create(LayeredBitmap* lb, int width, int height);

/*
 * Apply the bitmap to a layered window using UpdateLayeredWindow.
 * The window must have WS_EX_LAYERED style.
 */
void LayeredBitmap_Apply(LayeredBitmap* lb, HWND hwnd, int x, int y);

/*
 * Release all GDI resources.
 */
void LayeredBitmap_Destroy(LayeredBitmap* lb);

#endif /* LAYERED_WINDOW_H */

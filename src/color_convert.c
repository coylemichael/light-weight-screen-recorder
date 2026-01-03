/*
 * Color Space Conversion Implementation
 * BGRA to NV12 conversion for H.264 encoding
 */

#include "color_convert.h"

// Convert BGRA to NV12 format using BT.601 coefficients
// NV12: Y plane (width * height), then interleaved UV plane (width * height / 2)
void ColorConvert_BGRAtoNV12(const BYTE* bgra, BYTE* nv12, int width, int height) {
    int ySize = width * height;
    BYTE* yPlane = nv12;
    BYTE* uvPlane = nv12 + ySize;
    
    // Process all pixels for Y, and 2x2 blocks for UV subsampling
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int srcIdx = (y * width + x) * 4;
            int b = bgra[srcIdx + 0];
            int g = bgra[srcIdx + 1];
            int r = bgra[srcIdx + 2];
            
            // BT.601 conversion for Y (luma)
            // Y = 0.257*R + 0.504*G + 0.098*B + 16
            // Using integer math: Y = ((66*R + 129*G + 25*B + 128) >> 8) + 16
            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[y * width + x] = (BYTE)(yVal < 0 ? 0 : (yVal > 255 ? 255 : yVal));
            
            // UV for every 2x2 block (top-left pixel of each block)
            if ((x % 2 == 0) && (y % 2 == 0)) {
                // BT.601 conversion for U (Cb) and V (Cr)
                // U = -0.148*R - 0.291*G + 0.439*B + 128
                // V =  0.439*R - 0.368*G - 0.071*B + 128
                int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                int uvIdx = (y / 2) * width + x;
                uvPlane[uvIdx] = (BYTE)(uVal < 0 ? 0 : (uVal > 255 ? 255 : uVal));
                uvPlane[uvIdx + 1] = (BYTE)(vVal < 0 ? 0 : (vVal > 255 ? 255 : vVal));
            }
        }
    }
}

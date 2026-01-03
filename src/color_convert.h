/*
 * Color Space Conversion
 * BGRA to NV12 conversion for H.264 encoding
 */

#ifndef COLOR_CONVERT_H
#define COLOR_CONVERT_H

#include <windows.h>

// Convert BGRA pixel data to NV12 format (required for H.264 encoding)
// NV12: Y plane (width * height bytes), then interleaved UV plane (width * height / 2 bytes)
// Uses BT.601 color space coefficients
// 
// Parameters:
//   bgra   - Source BGRA data (4 bytes per pixel)
//   nv12   - Destination NV12 buffer (must be width * height * 3 / 2 bytes)
//   width  - Frame width in pixels (should be even)
//   height - Frame height in pixels (should be even)
void ColorConvert_BGRAtoNV12(const BYTE* bgra, BYTE* nv12, int width, int height);

// Calculate required NV12 buffer size in bytes
#define NV12_BUFFER_SIZE(width, height) ((width) * (height) * 3 / 2)

#endif // COLOR_CONVERT_H

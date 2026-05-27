/*
 * OCR Preview - Debug windows showing OCR pre-processing stages
 *
 * When "Show Processing" is enabled, displays small popup windows
 * for each stage of the OCR image pipeline so you can visually
 * assess which filters help.
 */

#ifndef OCR_PREVIEW_H
#define OCR_PREVIEW_H

#include <windows.h>

/* Pre-processing stage identifiers */
typedef enum {
    OCR_STAGE_RAW = 0,        /* Original BGRA capture */
    OCR_STAGE_GRAYSCALE,      /* Luminance conversion */
    OCR_STAGE_CONTRAST,       /* Contrast-stretched */
    OCR_STAGE_BINARY,         /* Otsu binarization */
    OCR_STAGE_SCALED,         /* Upscaled for small text */
    OCR_STAGE_COUNT
} OcrPreviewStage;

/* Register the preview window class. Call once at startup. */
BOOL OcrPreview_Register(HINSTANCE hInstance);

/* Update a stage's preview with new grayscale pixel data.
 * Creates the window on first call. Grayscale = 1 byte/pixel.
 * For RAW stage, pass bgraPixels with bpp=4. */
void OcrPreview_Update(OcrPreviewStage stage, const BYTE* pixels,
                       int width, int height, int bytesPerPixel);

/* Close all preview windows. */
void OcrPreview_CloseAll(void);

/* Destroy all preview windows and free resources.
 * Must be called from the thread that created the windows. */
void OcrPreview_DestroyAll(void);

/* Pump pending messages for preview windows.
 * Must be called from the thread that created the windows. */
void OcrPreview_PumpMessages(void);

#endif /* OCR_PREVIEW_H */

/*
 * OCR Engine - Tesseract wrapper with dynamic loading
 * 
 * Loads tesseract53.dll at runtime via LoadLibrary so the feature
 * degrades gracefully if Tesseract isn't installed. Ships DLLs
 * alongside lwsr.exe in bin/.
 */

#ifndef OCR_ENGINE_H
#define OCR_ENGINE_H

#include <windows.h>

typedef struct OcrEngine OcrEngine;

/* Initialize OCR engine. dataPath is the folder containing tessdata/.
 * Returns NULL if Tesseract DLLs not found (feature disabled). */
OcrEngine* OcrEngine_Init(const char* dataPath);

/* Run OCR on a BGRA pixel buffer with full pre-processing pipeline.
 * If showProcessing is TRUE, updates debug preview windows for each stage
 * and logs per-stage OCR accuracy against expectedName (if non-NULL).
 * Returns allocated UTF-8 string (caller must free). Returns NULL on failure. */
char* OcrEngine_Recognize(OcrEngine* engine, const BYTE* bgraPixels,
                          int width, int height, int bytesPerPixel, int stride,
                          BOOL showProcessing, const char* expectedName);

/* Shut down and free all resources */
void OcrEngine_Shutdown(OcrEngine* engine);

/* Check if engine loaded successfully */
BOOL OcrEngine_IsReady(const OcrEngine* engine);

#endif /* OCR_ENGINE_H */

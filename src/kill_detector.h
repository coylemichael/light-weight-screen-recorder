/*
 * Kill Detector - Player name auto-detection from screen region
 *
 * Reads the player name region via GPU readback, runs OCR, and
 * validates the NAME#NNNN pattern to extract the player's name.
 * The detected name will be used by kill feed matching (future module).
 *
 * Thread model:
 *   - Capture thread calls KillDetector_FeedFrame() every frame
 *   - FeedFrame does GPU readback + OCR inline on a 3s cadence
 *   - No background threads
 *
 * USES: ocr_engine, capture (readback), config, logger
 */

#ifndef KILL_DETECTOR_H
#define KILL_DETECTOR_H

#include <windows.h>
#include <d3d11.h>
#include "config.h"
#include "capture.h"

typedef struct KillDetector KillDetector;

/* Initialize kill detector. Returns NULL if OCR engine unavailable or disabled. */
KillDetector* KillDetector_Init(const AppConfig* config, const CaptureState* capture,
                                HWND overlayWnd);

/* Called from capture loop every frame. Handles timing internally. */
void KillDetector_FeedFrame(KillDetector* det, CaptureState* capture,
                            ID3D11Texture2D* bgraTexture);

/* Set player name (thread-safe) */
void KillDetector_SetPlayerName(KillDetector* det, const char* name);

/* Get current player name (thread-safe copy) */
void KillDetector_GetPlayerName(KillDetector* det, char* out, int outSize);

/* Auto-detect player name from player name region.
 * Returns TRUE if a name was detected. */
BOOL KillDetector_DetectPlayerName(KillDetector* det, CaptureState* capture,
                                   ID3D11Texture2D* bgraTexture);

/* Enable/disable OCR pre-processing debug preview windows (thread-safe) */
void KillDetector_SetShowProcessing(KillDetector* det, BOOL show);

/* Shutdown and free all resources */
void KillDetector_Shutdown(KillDetector* det);

#endif /* KILL_DETECTOR_H */

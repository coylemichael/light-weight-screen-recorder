/*
 * overlay.h - Header for overlay.c
 * Recording indicator overlay window + hotkey handling
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include <windows.h>
#include "config.h"

// Create the overlay window system
BOOL Overlay_Create(HINSTANCE hInstance);

// Destroy overlay windows
void Overlay_Destroy(void);

// Set the current capture mode
void Overlay_SetMode(CaptureMode mode);

// Get the selected region
BOOL Overlay_GetSelectedRegion(RECT* region);

// Update recording state display
void Overlay_SetRecordingState(BOOL isRecording);

// Start recording with current selection
void Recording_Start(void);

// Stop current recording
void Recording_Stop(void);

#endif // OVERLAY_H

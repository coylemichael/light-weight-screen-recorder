/*
 * overlay.h - Header for overlay.c
 * Recording indicator overlay window + hotkey handling
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include <windows.h>

// Create the overlay window system
BOOL Overlay_Create(HINSTANCE hInstance);

// Destroy overlay windows
void Overlay_Destroy(void);

// Helper function to calculate aspect ratio dimensions
// Used by settings dialog for replay preview
void GetAspectRatioDimensions(int aspectIndex, int* ratioW, int* ratioH);

// Update the replay capture source preview overlay
void UpdateReplayPreview(void);

// Save the current area selector position to config
void SaveAreaSelectorPosition(void);

/* Control panel button IDs - shared with settings_dialog.c for cross-window
 * invalidation (e.g. settings dialog refreshes mode buttons on close). */
#define ID_MODE_AREA       1001
#define ID_MODE_WINDOW     1002
#define ID_MODE_MONITOR    1003
#define ID_BTN_SETTINGS    1013

#endif // OVERLAY_H

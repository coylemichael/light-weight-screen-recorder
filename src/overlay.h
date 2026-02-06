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

#endif // OVERLAY_H

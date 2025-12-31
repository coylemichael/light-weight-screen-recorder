/*
 * Recording Border Highlight
 * Shows a red border around the capture area during recording
 */

#ifndef BORDER_H
#define BORDER_H

#include <windows.h>

// Border thickness in pixels
#define BORDER_THICKNESS 1

// Initialize the border module
BOOL Border_Init(HINSTANCE hInstance);

// Shutdown and cleanup
void Border_Shutdown(void);

// Show border around the specified rect
void Border_Show(RECT captureRect);

// Hide the border
void Border_Hide(void);

// Check if border is currently visible
BOOL Border_IsVisible(void);

#endif // BORDER_H

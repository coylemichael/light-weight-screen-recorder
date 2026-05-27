/*
 * Screen region selection border
 * Provides red recording border and draggable area selector
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

// Flash border yellow briefly (~150ms) for marker feedback
// No-op if border is not currently visible
void Border_Flash(void);

// Flash border with a custom color briefly (~150ms)
// Used for auto-clip (green) vs marker (yellow) distinction
void Border_FlashColor(int r, int g, int b);

// Draggable area selection for custom area mode
BOOL AreaSelector_Init(HINSTANCE hInstance);
void AreaSelector_Shutdown(void);
void AreaSelector_Show(RECT initialRect, BOOL allowMove);  // allowMove=FALSE for locked full-screen views
void AreaSelector_Hide(void);
BOOL AreaSelector_GetRect(RECT* outRect);
BOOL AreaSelector_IsVisible(void);

#endif // BORDER_H

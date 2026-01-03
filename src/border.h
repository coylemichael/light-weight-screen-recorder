/*
 * Recording Border Highlight
 * Shows a red border around the capture area during recording
 */

#ifndef BORDER_H
#define BORDER_H

#include <windows.h>

// Border thickness in pixels
#define BORDER_THICKNESS 1

// Preview border thickness (thinner for settings preview)
#define PREVIEW_BORDER_THICKNESS 2

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

// Preview border for settings window
BOOL PreviewBorder_Init(HINSTANCE hInstance);
void PreviewBorder_Shutdown(void);
void PreviewBorder_Show(RECT rect);
void PreviewBorder_Hide(void);

// Draggable area selection for custom area mode
BOOL AreaSelector_Init(HINSTANCE hInstance);
void AreaSelector_Shutdown(void);
void AreaSelector_Show(RECT initialRect, BOOL allowMove);  // allowMove=FALSE for locked full-screen views
void AreaSelector_Hide(void);
BOOL AreaSelector_GetRect(RECT* outRect);
BOOL AreaSelector_IsVisible(void);

#endif // BORDER_H

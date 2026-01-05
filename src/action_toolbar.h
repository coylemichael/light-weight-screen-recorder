// action_toolbar.h - Custom action toolbar with smooth rounded corners
#ifndef ACTION_TOOLBAR_H
#define ACTION_TOOLBAR_H

#include <windows.h>

// Initialize the action toolbar (call once at startup)
BOOL ActionToolbar_Init(HINSTANCE hInstance);

// Cleanup (call before exit)
void ActionToolbar_Shutdown(void);

// Show the toolbar centered horizontally at (x, y)
void ActionToolbar_Show(int centerX, int topY);

// Hide the toolbar
void ActionToolbar_Hide(void);

// Set button click callbacks
// onMinimize: called when "-" button clicked (minimize to tray)
// onRecord: called when record button clicked
// onClose: called when "X" button clicked
// onSettings: called when "..." button clicked
void ActionToolbar_SetCallbacks(void (*onMinimize)(void), void (*onRecord)(void), 
                                 void (*onClose)(void), void (*onSettings)(void));

// Get the toolbar window handle (for z-order management, etc.)
HWND ActionToolbar_GetWindow(void);

#endif // ACTION_TOOLBAR_H

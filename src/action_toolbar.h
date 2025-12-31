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
void ActionToolbar_SetCallbacks(void (*onRecord)(void), void (*onCopy)(void), 
                                 void (*onSave)(void), void (*onMarkup)(void));

// Get the toolbar window handle (for z-order management, etc.)
HWND ActionToolbar_GetWindow(void);

#endif // ACTION_TOOLBAR_H

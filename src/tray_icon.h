/*
 * tray_icon.h - System tray icon management
 * 
 * Handles system tray icon, minimize to tray, and restore functionality.
 */

#ifndef TRAY_ICON_H
#define TRAY_ICON_H

#include <windows.h>

/* System tray message ID */
#define WM_TRAYICON  (WM_USER + 100)

/* Initialize tray icon (call after control window is created) */
void TrayIcon_Add(HWND controlWnd, HINSTANCE hInstance);

/* Remove tray icon (call before shutdown) */
void TrayIcon_Remove(void);

/* Minimize all windows to tray */
void TrayIcon_Minimize(HWND controlWnd, HWND overlayWnd, HWND settingsWnd);

/* Restore from tray */
void TrayIcon_Restore(HWND controlWnd);

/* Check if currently minimized to tray */
BOOL TrayIcon_IsMinimized(void);

/* Set minimized state (for external coordination) */
void TrayIcon_SetMinimized(BOOL minimized);

#endif // TRAY_ICON_H

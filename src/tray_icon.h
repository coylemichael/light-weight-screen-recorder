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
void TrayIcon_Add(HWND controlWnd);

/* Remove tray icon (call before shutdown) */
void TrayIcon_Remove(void);

/* Minimize all windows to tray (hides control, overlay, action toolbar, settings) */
void TrayIcon_Minimize(HWND controlWnd, HWND overlayWnd, HWND settingsWnd);

/* Restore from tray. NOTE: Asymmetric with TrayIcon_Minimize — only re-shows
 * the control window. Overlay, action toolbar, and settings remain hidden;
 * overlay.c re-shows them via its own logic when needed. */
void TrayIcon_Restore(HWND controlWnd);

/* Check if currently minimized to tray */
BOOL TrayIcon_IsMinimized(void);

#endif // TRAY_ICON_H

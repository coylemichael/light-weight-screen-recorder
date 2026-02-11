/*
 * tray_icon.c - System tray icon management
 * 
 * Handles system tray icon, minimize to tray, and restore functionality.
 * Extracted from overlay.c for better separation of concerns.
 */

#include <windows.h>
#include <shellapi.h>
#include "tray_icon.h"
#include "action_toolbar.h"

/* Tray icon state */
static NOTIFYICONDATAA g_iconData = {0};
static BOOL g_minimizedToTray = FALSE;
static HICON g_trayHIcon = NULL;

/* Load icon from ICO file */
static HICON LoadIconFromICO(const char* filename) {
    int iconWidth = GetSystemMetrics(SM_CXSMICON);
    int iconHeight = GetSystemMetrics(SM_CYSMICON);
    
    HICON hIcon = (HICON)LoadImageA(NULL, filename, IMAGE_ICON, 
                                     iconWidth, iconHeight, LR_LOADFROMFILE);
    return hIcon;
}

void TrayIcon_Add(HWND controlWnd, HINSTANCE hInstance) {
    (void)hInstance;  /* Reserved for future use */
    
    g_iconData.cbSize = sizeof(NOTIFYICONDATAA);
    g_iconData.hWnd = controlWnd;
    g_iconData.uID = 1;
    g_iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_iconData.uCallbackMessage = WM_TRAYICON;
    
    /* Try to load custom icon from static folder (prefer .ico for best quality) */
    g_trayHIcon = LoadIconFromICO("static\\lwsr_icon.ico");
    if (!g_trayHIcon) {
        /* Try relative to executable */
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            size_t remaining = sizeof(exePath) - (lastSlash + 1 - exePath);
            strncpy(lastSlash + 1, "..\\static\\lwsr_icon.ico", remaining - 1);
            exePath[sizeof(exePath) - 1] = '\0';
            g_trayHIcon = LoadIconFromICO(exePath);
        }
    }
    
    g_iconData.hIcon = g_trayHIcon ? g_trayHIcon : LoadIcon(NULL, IDI_APPLICATION);
    strncpy(g_iconData.szTip, "LWSR - Screen Recorder", sizeof(g_iconData.szTip) - 1);
    g_iconData.szTip[sizeof(g_iconData.szTip) - 1] = '\0';
    Shell_NotifyIconA(NIM_ADD, &g_iconData);
}

void TrayIcon_Remove(void) {
    Shell_NotifyIconA(NIM_DELETE, &g_iconData);
    if (g_trayHIcon) {
        DestroyIcon(g_trayHIcon);
        g_trayHIcon = NULL;
    }
}

void TrayIcon_Minimize(HWND controlWnd, HWND overlayWnd, HWND settingsWnd) {
    if (g_minimizedToTray) return;
    
    /* Hide all windows */
    ShowWindow(controlWnd, SW_HIDE);
    ShowWindow(overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    if (settingsWnd) ShowWindow(settingsWnd, SW_HIDE);
    
    g_minimizedToTray = TRUE;
}

void TrayIcon_Restore(HWND controlWnd) {
    if (!g_minimizedToTray) return;
    
    /* Show control panel */
    ShowWindow(controlWnd, SW_SHOW);
    SetForegroundWindow(controlWnd);
    
    g_minimizedToTray = FALSE;
}

BOOL TrayIcon_IsMinimized(void) {
    return g_minimizedToTray;
}

void TrayIcon_SetMinimized(BOOL minimized) {
    g_minimizedToTray = minimized;
}

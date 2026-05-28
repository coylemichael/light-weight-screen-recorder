/*
 * settings_dialog.c - Settings window UI and control handling
 *
 * Extracted from overlay.c to reduce complexity.
 * Handles the settings dialog window creation, display, and event handling.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>

#include "settings_dialog.h"
#include "config.h"
#include "capture.h"
#include "audio_device.h"
#include "aac_encoder.h"
#include "replay_buffer.h"
#include "logger.h"
#include "constants.h"
#include "mem_utils.h"
#include "overlay.h"
#include "debug_console.h"

#pragma comment(lib, "comctl32.lib")

/* ============================================================================
 * EXTERNAL GLOBALS
 * ============================================================================ */

extern AppConfig g_config;
extern ReplayBufferState g_replayBuffer;
extern CaptureState g_capture;
extern HWND g_controlWnd;

/* Forward declarations from overlay.c (still needed for preview/area functions) */
extern void UpdateReplayPreview(void);
extern void AreaSelector_Hide(void);
extern void AreaSelector_Shutdown(void);
extern BOOL AreaSelector_IsVisible(void);
extern void AreaSelector_GetRect(RECT* rect);
extern void GetAspectRatioDimensions(int aspectIndex, int* ratioW, int* ratioH);
extern void SaveAreaSelectorPosition(void);

/* Forward declarations for region debug overlay */
static void AutoClipRegionOverlay_Show(void);
static void AutoClipRegionOverlay_Hide(void);

/* Hotkey IDs HOTKEY_REPLAY_SAVE, HOTKEY_MARKER defined in constants.h */

/* ============================================================================
 * MODULE-LEVEL STATE
 * ============================================================================ */

static HWND s_settingsWnd = NULL;
static HFONT s_settingsFont = NULL;
static HFONT s_settingsSmallFont = NULL;
static HBRUSH s_settingsBgBrush = NULL;
static HBRUSH s_editBrush = NULL;
static HINSTANCE s_hInstance = NULL;
static HWND* s_externalHandleRef = NULL;  /* For updating caller's reference */

/* Hotkey capture state */
static BOOL s_waitingForHotkey = FALSE;
static BOOL s_waitingForMarkerHotkey = FALSE;

/* Tab state */
static SettingsTab s_currentTab = SETTINGS_TAB_GENERAL;

/* Log child-control creation failures without aborting the dialog. Downstream
 * use (SendMessage, AddToSection, ShowSection) already tolerates NULL HWNDs,
 * so a failed control just leaves a visible gap and a diagnostic line.
 * Per docs/tracking/may26review/plan/settings_dialog.md item 9. */
#define CHECK_CTL(h_) do { \
    if (!(h_)) Logger_Log("settings_dialog: control creation failed at %s:%d (err=%lu)\n", \
                          __FILE__, __LINE__, (unsigned long)GetLastError()); \
} while (0)

/* Control handle arrays for each tab section (max 48 controls for Video which is larger) */
#define MAX_SECTION_CONTROLS 48
static HWND s_generalControls[MAX_SECTION_CONTROLS];
static HWND s_audioControls[MAX_SECTION_CONTROLS];
static HWND s_videoControls[MAX_SECTION_CONTROLS];
static HWND s_autoClipControls[MAX_SECTION_CONTROLS];
static int s_generalControlCount = 0;
static int s_audioControlCount = 0;
static int s_videoControlCount = 0;
static int s_autoClipControlCount = 0;

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================ */

/**
 * Convert a virtual key code to a human-readable key name.
 */
static void GetKeyNameFromVK(int vk, char* buffer, int bufSize) {
    // Handle special keys first
    switch (vk) {
        case VK_F1: case VK_F2: case VK_F3: case VK_F4:
        case VK_F5: case VK_F6: case VK_F7: case VK_F8:
        case VK_F9: case VK_F10: case VK_F11: case VK_F12:
            snprintf(buffer, bufSize, "F%d", vk - VK_F1 + 1);
            return;
        case VK_F13: case VK_F14: case VK_F15: case VK_F16:
        case VK_F17: case VK_F18: case VK_F19: case VK_F20:
        case VK_F21: case VK_F22: case VK_F23: case VK_F24:
            snprintf(buffer, bufSize, "F%d", vk - VK_F13 + 13);
            return;
        case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3:
        case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7:
        case VK_NUMPAD8: case VK_NUMPAD9:
            snprintf(buffer, bufSize, "Num %d", vk - VK_NUMPAD0);
            return;
        case VK_MULTIPLY:    strncpy(buffer, "Num *", bufSize); return;
        case VK_ADD:         strncpy(buffer, "Num +", bufSize); return;
        case VK_SUBTRACT:    strncpy(buffer, "Num -", bufSize); return;
        case VK_DECIMAL:     strncpy(buffer, "Num .", bufSize); return;
        case VK_DIVIDE:      strncpy(buffer, "Num /", bufSize); return;
        case VK_PRIOR:       strncpy(buffer, "Page Up", bufSize); return;
        case VK_NEXT:        strncpy(buffer, "Page Down", bufSize); return;
        case VK_END:         strncpy(buffer, "End", bufSize); return;
        case VK_HOME:        strncpy(buffer, "Home", bufSize); return;
        case VK_LEFT:        strncpy(buffer, "Left", bufSize); return;
        case VK_UP:          strncpy(buffer, "Up", bufSize); return;
        case VK_RIGHT:       strncpy(buffer, "Right", bufSize); return;
        case VK_DOWN:        strncpy(buffer, "Down", bufSize); return;
        case VK_INSERT:      strncpy(buffer, "Insert", bufSize); return;
        case VK_DELETE:      strncpy(buffer, "Delete", bufSize); return;
        case VK_SPACE:       strncpy(buffer, "Space", bufSize); return;
        case VK_TAB:         strncpy(buffer, "Tab", bufSize); return;
        case VK_RETURN:      strncpy(buffer, "Enter", bufSize); return;
        case VK_BACK:        strncpy(buffer, "Backspace", bufSize); return;
        case VK_ESCAPE:      strncpy(buffer, "Escape", bufSize); return;
        case VK_PAUSE:       strncpy(buffer, "Pause", bufSize); return;
        case VK_CAPITAL:     strncpy(buffer, "Caps Lock", bufSize); return;
        case VK_NUMLOCK:     strncpy(buffer, "Num Lock", bufSize); return;
        case VK_SCROLL:      strncpy(buffer, "Scroll Lock", bufSize); return;
        case VK_SNAPSHOT:    strncpy(buffer, "Print Screen", bufSize); return;
        case VK_OEM_1:       strncpy(buffer, ";", bufSize); return;
        case VK_OEM_PLUS:    strncpy(buffer, "=", bufSize); return;
        case VK_OEM_COMMA:   strncpy(buffer, ",", bufSize); return;
        case VK_OEM_MINUS:   strncpy(buffer, "-", bufSize); return;
        case VK_OEM_PERIOD:  strncpy(buffer, ".", bufSize); return;
        case VK_OEM_2:       strncpy(buffer, "/", bufSize); return;
        case VK_OEM_3:       strncpy(buffer, "`", bufSize); return;
        case VK_OEM_4:       strncpy(buffer, "[", bufSize); return;
        case VK_OEM_5:       strncpy(buffer, "\\", bufSize); return;
        case VK_OEM_6:       strncpy(buffer, "]", bufSize); return;
        case VK_OEM_7:       strncpy(buffer, "'", bufSize); return;
    }
    
    // For letter keys (A-Z)
    if (vk >= 'A' && vk <= 'Z') {
        snprintf(buffer, bufSize, "%c", (char)vk);
        return;
    }
    
    // For number keys (0-9)
    if (vk >= '0' && vk <= '9') {
        snprintf(buffer, bufSize, "%c", (char)vk);
        return;
    }
    
    // Fall back to GetKeyNameText
    UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        GetKeyNameTextA(scanCode << 16, buffer, bufSize);
        if (buffer[0] != '\0') return;
    }
    
    // Last resort: show hex code
    snprintf(buffer, bufSize, "Key 0x%02X", vk);
}

/**
 * Populate an audio dropdown with devices.
 * Returns the index that matches the given deviceId, or 0 if not found.
 */
static int PopulateAudioDropdown(HWND comboBox, const AudioDeviceList* devices, const char* selectedDeviceId) {
    if (!comboBox || !devices) return 0;
    
    int selectedIdx = 0;
    int currentIdx = 0;
    
    /* Add "None (Disabled)" option */
    int itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)"None (Disabled)");
    SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)NULL);
    currentIdx++;
    
    /* Add separator for outputs (system audio) */
    itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)"--- System Audio (Loopback) ---");
    SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)NULL);
    currentIdx++;
    
    /* Add output devices */
    for (int i = 0; i < devices->count; i++) {
        if (devices->devices[i].type == AUDIO_DEVICE_OUTPUT) {
            char displayName[160];
            if (devices->devices[i].isDefault) {
                snprintf(displayName, sizeof(displayName), "%s (Default)", devices->devices[i].name);
            } else {
                strncpy(displayName, devices->devices[i].name, sizeof(displayName) - 1);
                displayName[sizeof(displayName) - 1] = '\0';
            }
            itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)displayName);
            SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)devices->devices[i].id);
            
            if (selectedDeviceId && selectedDeviceId[0] != '\0' &&
                strcmp(devices->devices[i].id, selectedDeviceId) == 0) {
                selectedIdx = currentIdx;
            }
            currentIdx++;
        }
    }
    
    /* Add separator for inputs (microphones) */
    itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)"--- Microphones ---");
    SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)NULL);
    currentIdx++;
    
    /* Add input devices */
    for (int i = 0; i < devices->count; i++) {
        if (devices->devices[i].type == AUDIO_DEVICE_INPUT) {
            char displayName[160];
            if (devices->devices[i].isDefault) {
                snprintf(displayName, sizeof(displayName), "%s (Default)", devices->devices[i].name);
            } else {
                strncpy(displayName, devices->devices[i].name, sizeof(displayName) - 1);
                displayName[sizeof(displayName) - 1] = '\0';
            }
            itemIdx = (int)SendMessageA(comboBox, CB_ADDSTRING, 0, (LPARAM)displayName);
            SendMessageA(comboBox, CB_SETITEMDATA, itemIdx, (LPARAM)devices->devices[i].id);
            
            if (selectedDeviceId && selectedDeviceId[0] != '\0' &&
                strcmp(devices->devices[i].id, selectedDeviceId) == 0) {
                selectedIdx = currentIdx;
            }
            currentIdx++;
        }
    }
    
    return selectedIdx;
}

/**
 * Update the RAM estimate display for replay buffer settings.
 */
static void UpdateReplayRAMEstimate(HWND hwnd) {
    int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
    int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
    int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
    int duration = hours * 3600 + mins * 60 + secs;
    if (duration < 1) duration = 1;
    
    int fpsIdx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_FPS), CB_GETCURSEL, 0, 0);
    int fpsValues[] = { 30, 60, 120, 240 };
    int fps = (fpsIdx >= 0 && fpsIdx < 4) ? fpsValues[fpsIdx] : 60;
    
    /* Get resolution based on source selection */
    int srcIdx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SOURCE), CB_GETCURSEL, 0, 0);
    int monCount = GetSystemMetrics(SM_CMONITORS);
    int width = 1920, height = 1080;
    
    if (srcIdx < monCount) {
        /* Get monitor dimensions and apply aspect ratio */
        HWND cmbAspect = GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT);
        int sel = (int)SendMessage(cmbAspect, CB_GETCURSEL, 0, 0);
        int aspectIdx = (int)SendMessage(cmbAspect, CB_GETITEMDATA, sel, 0);
        
        /* Default to primary monitor dimensions */
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        
        /* Apply aspect ratio if not native */
        if (aspectIdx > 0) {
            int ratioW, ratioH;
            GetAspectRatioDimensions(aspectIdx, &ratioW, &ratioH);
            if (ratioW > 0 && ratioH > 0) {
                /* Calculate dimensions maintaining aspect while fitting in monitor */
                if (width * ratioH > height * ratioW) {
                    /* Fit to height */
                    width = (height * ratioW) / ratioH;
                } else {
                    /* Fit to width */
                    height = (width * ratioH) / ratioW;
                }
            }
        }
    }
    
    /* Calculate estimate: ~0.1 bits per pixel for compressed NVENC H.264/H.265
     * This gives roughly 50-60 Mbps for 1080p60, which matches typical recordings */
    double bitsPerPixel = 0.1;
    double bitsPerFrame = (double)(width * height) * bitsPerPixel;
    double bitsPerSecond = bitsPerFrame * fps;
    double totalBits = bitsPerSecond * duration;
    double totalMB = totalBits / 8.0 / 1024.0 / 1024.0;
    
    char ramText[128];
    if (totalMB >= 1024.0) {
        snprintf(ramText, sizeof(ramText), "~%.1f GB RAM", totalMB / 1024.0);
    } else {
        snprintf(ramText, sizeof(ramText), "~%.0f MB RAM", totalMB);
    }
    
    SetWindowTextA(GetDlgItem(hwnd, ID_STATIC_REPLAY_RAM), ramText);
    
    /* Update detailed calculation - show bitrate estimate */
    double mbps = bitsPerSecond / 1000000.0;
    char calcText[256];
    snprintf(calcText, sizeof(calcText), 
             "%dx%d @ %d fps (~%.0f Mbps)", 
             width, height, fps, mbps);
    SetWindowTextA(GetDlgItem(hwnd, ID_STATIC_REPLAY_CALC), calcText);
}

/* ============================================================================
 * SETTINGS LAYOUT HELPERS
 * ============================================================================ */

/* Settings layout parameters */
typedef struct {
    int y;              /* Current Y position */
    int labelX;         /* X position for labels */
    int labelW;         /* Label width */
    int controlX;       /* X position for controls */
    int controlW;       /* Control width */
    int rowH;           /* Row height */
    int contentW;       /* Total content width */
    HFONT font;         /* Standard font */
    HFONT smallFont;    /* Small font */
} SettingsLayout;

/* ============================================================================
 * TAB SWITCHING HELPERS
 * ============================================================================ */

/**
 * Add a control to a section's control array for show/hide management.
 */
static void AddToSection(HWND hwnd, HWND* controls, int* count) {
    LWSR_ASSERT(*count < MAX_SECTION_CONTROLS);
    if (*count < MAX_SECTION_CONTROLS) {
        controls[(*count)++] = hwnd;
    } else {
        Logger_Log("AddToSection: MAX_SECTION_CONTROLS (%d) exceeded; control dropped\n",
                   MAX_SECTION_CONTROLS);
    }
}

/**
 * Show or hide all controls in a section.
 * Uses SetWindowPos with SWP_NOREDRAW to prevent white flash.
 */
static void ShowSection(HWND* controls, int count, BOOL show) {
    for (int i = 0; i < count; i++) {
        if (controls[i]) {
            /* Use SetWindowPos to avoid visual artifacts during show/hide */
            SetWindowPos(controls[i], NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW |
                (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
    }
}

/**
 * Switch to a specific tab, showing its controls and hiding others.
 * Uses deferred redraw to prevent white flash artifacts.
 */
static void SwitchToTab(SettingsTab tab) {
    s_currentTab = tab;
    
    /* Defer painting during control show/hide */
    if (s_settingsWnd) {
        SendMessage(s_settingsWnd, WM_SETREDRAW, FALSE, 0);
    }
    
    /* Hide all sections first */
    ShowSection(s_generalControls, s_generalControlCount, FALSE);
    ShowSection(s_audioControls, s_audioControlCount, FALSE);
    ShowSection(s_videoControls, s_videoControlCount, FALSE);
    ShowSection(s_autoClipControls, s_autoClipControlCount, FALSE);
    
    /* Show the selected section */
    switch (tab) {
        case SETTINGS_TAB_GENERAL:
            ShowSection(s_generalControls, s_generalControlCount, TRUE);
            break;
        case SETTINGS_TAB_AUDIO:
            ShowSection(s_audioControls, s_audioControlCount, TRUE);
            break;
        case SETTINGS_TAB_VIDEO:
            ShowSection(s_videoControls, s_videoControlCount, TRUE);
            break;
        case SETTINGS_TAB_AUTOCLIP:
            ShowSection(s_autoClipControls, s_autoClipControlCount, TRUE);
            break;
    }
    
    /* Re-enable painting and force full redraw */
    if (s_settingsWnd) {
        SendMessage(s_settingsWnd, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(s_settingsWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

/* ============================================================================
 * SECTION CREATORS - Each creates controls for a specific tab
 * ============================================================================ */

/**
 * Create GENERAL tab controls:
 * - Capture mouse cursor checkbox
 * - Show recording border checkbox  
 * - Time limit (enable + h/m/s dropdowns)
 * - Save to (path + browse)
 * - Debug logging checkbox
 */
static void CreateGeneralSection(HWND hwnd, SettingsLayout* layout) {
    s_generalControlCount = 0;
    HWND ctl;
    
    /* Capture mouse cursor */
    ctl = CreateWindowW(L"BUTTON", L"Capture mouse cursor",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX, layout->y, 200, 24, hwnd, (HMENU)ID_CHK_MOUSE, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_MOUSE, g_config.captureMouse ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Show recording border */
    ctl = CreateWindowW(L"BUTTON", L"Show recording border",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX + 220, layout->y, 200, 24, hwnd, (HMENU)ID_CHK_BORDER, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_BORDER, g_config.showRecordingBorder ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += 38;
    
    /* Time limit label */
    ctl = CreateWindowW(L"STATIC", L"Time limit",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Calculate time from seconds */
    int totalSecs = g_config.maxRecordingSeconds;
    if (totalSecs < 1) totalSecs = 60;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;
    
    /* Hours dropdown */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX, layout->y, 55, 300, hwnd, (HMENU)ID_CMB_HOURS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessage(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    for (int i = 0; i <= 24; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(ctl, CB_SETCURSEL, hours, 0);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    ctl = CreateWindowW(L"STATIC", L"h",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 58, layout->y, 15, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Minutes dropdown */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX + 78, layout->y, 55, 300, hwnd, (HMENU)ID_CMB_MINUTES, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessage(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    for (int i = 0; i <= 59; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(ctl, CB_SETCURSEL, mins, 0);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    ctl = CreateWindowW(L"STATIC", L"m",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 136, layout->y, 18, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Seconds dropdown */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX + 158, layout->y, 55, 300, hwnd, (HMENU)ID_CMB_SECONDS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessage(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    for (int i = 0; i <= 59; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(ctl, CB_SETCURSEL, secs, 0);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    ctl = CreateWindowW(L"STATIC", L"s",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 216, layout->y, 15, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH;
    
    /* Save path label */
    ctl = CreateWindowW(L"STATIC", L"Save to",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y + 1, layout->labelW, 22, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Save path edit */
    ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL,
        layout->controlX, layout->y, layout->controlW - 80, 22, hwnd, (HMENU)ID_EDT_PATH, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SetWindowTextA(ctl, g_config.savePath);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Browse button */
    ctl = CreateWindowW(L"BUTTON", L"Browse",
        WS_CHILD | BS_PUSHBUTTON,
        layout->controlX + layout->controlW - 72, layout->y, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH + 12;
    
    /* Separator */
    ctl = CreateWindowW(L"STATIC", L"",
        WS_CHILD | SS_ETCHEDHORZ,
        layout->labelX, layout->y, layout->contentW, 2, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += 14;
    
    /* Marker hotkey label */
    ctl = CreateWindowW(L"STATIC", L"Marker Hotkey",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Marker hotkey button */
    {
        char keyName[64];
        GetKeyNameFromVK(g_config.markerKey, keyName, sizeof(keyName));
        ctl = CreateWindowExA(0, "BUTTON", keyName,
            WS_CHILD | BS_PUSHBUTTON,
            layout->controlX, layout->y, 100, 24, hwnd, (HMENU)ID_BTN_MARKER_HOTKEY, s_hInstance, NULL);
        CHECK_CTL(ctl);
        SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
        AddToSection(ctl, s_generalControls, &s_generalControlCount);
    }
    layout->y += layout->rowH;
    
    /* Debug logging checkbox */
    ctl = CreateWindowW(L"BUTTON", L"Enable debug logging (creates log files in Debug folder)",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX, layout->y, 400, 24, hwnd, (HMENU)ID_CHK_DEBUG_LOGGING, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_DEBUG_LOGGING, g_config.debugLogging ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH + 12;
    
    /* ── Auto-Clip Section ── */
    ctl = CreateWindowW(L"STATIC", L"",
        WS_CHILD | SS_ETCHEDHORZ,
        layout->labelX, layout->y, layout->contentW, 2, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += 14;
    
    /* Enable auto-clip checkbox */
    ctl = CreateWindowW(L"BUTTON", L"Kill feed instant clipping (requires Tesseract OCR)",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX, layout->y, 420, 24, hwnd, (HMENU)ID_CHK_AUTOCLIP_ENABLED, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_AUTOCLIP_ENABLED, g_config.autoClipEnabled ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH;
    
    /* Show regions checkbox (debug overlay) */
    ctl = CreateWindowW(L"BUTTON", L"Show detection regions (calibration)",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX + 16, layout->y, 300, 24, hwnd, (HMENU)ID_CHK_AUTOCLIP_SHOW_REGIONS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_AUTOCLIP_SHOW_REGIONS, g_config.autoClipShowRegions ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH;
    
    /* Debug console checkbox */
    ctl = CreateWindowW(L"BUTTON", L"Debug console (live OCR feed)",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX + 16, layout->y, 300, 24, hwnd, (HMENU)ID_CHK_AUTOCLIP_DEBUG_CONSOLE, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_AUTOCLIP_DEBUG_CONSOLE, DebugConsole_IsOpen() ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH;
    
    /* Cooldown label */
    ctl = CreateWindowW(L"STATIC", L"Cooldown (sec)",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 22, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Cooldown value label */
    {
        char cdBuf[16];
        snprintf(cdBuf, sizeof(cdBuf), "%d", g_config.autoClipCooldownSec);
        ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_NUMBER | ES_CENTER,
            layout->controlX + 155, layout->y + 2, 40, 20, hwnd, (HMENU)ID_LBL_AUTOCLIP_COOLDOWN, s_hInstance, NULL);
        CHECK_CTL(ctl);
        SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
        SetWindowTextA(ctl, cdBuf);
        AddToSection(ctl, s_generalControls, &s_generalControlCount);
    }
    
    /* Cooldown slider */
    ctl = CreateWindowW(TRACKBAR_CLASSW, L"",
        WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        layout->controlX, layout->y, 150, 26, hwnd, (HMENU)ID_SLD_AUTOCLIP_COOLDOWN, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, TBM_SETRANGE, TRUE, MAKELPARAM(AUTOCLIP_COOLDOWN_MIN_SEC, AUTOCLIP_COOLDOWN_MAX_SEC));
    SendMessage(ctl, TBM_SETPOS, TRUE, g_config.autoClipCooldownSec);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH;
    
    /* Save delay label */
    ctl = CreateWindowW(L"STATIC", L"Save delay (sec)",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 22, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    
    /* Save delay value label */
    {
        char dlBuf[16];
        snprintf(dlBuf, sizeof(dlBuf), "%d", g_config.autoClipDelaySec);
        ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_NUMBER | ES_CENTER,
            layout->controlX + 155, layout->y + 2, 40, 20, hwnd, (HMENU)ID_LBL_AUTOCLIP_DELAY, s_hInstance, NULL);
        CHECK_CTL(ctl);
        SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
        SetWindowTextA(ctl, dlBuf);
        AddToSection(ctl, s_generalControls, &s_generalControlCount);
    }
    
    /* Save delay slider */
    ctl = CreateWindowW(TRACKBAR_CLASSW, L"",
        WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        layout->controlX, layout->y, 150, 26, hwnd, (HMENU)ID_SLD_AUTOCLIP_DELAY, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, TBM_SETRANGE, TRUE, MAKELPARAM(AUTOCLIP_DELAY_MIN_SEC, AUTOCLIP_DELAY_MAX_SEC));
    SendMessage(ctl, TBM_SETPOS, TRUE, g_config.autoClipDelaySec);
    AddToSection(ctl, s_generalControls, &s_generalControlCount);
    layout->y += layout->rowH;
}

/**
 * Create VIDEO tab controls:
 * - Output Format dropdown
 * - Quality dropdown
 * - Frame Rate dropdown
 * - Replay Buffer: Enable, Source, Aspect, Duration, RAM, Hotkey
 */
static void CreateVideoSection(HWND hwnd, SettingsLayout* layout) {
    s_videoControlCount = 0;
    HWND ctl;
    
    /* Format label */
    ctl = CreateWindowW(L"STATIC", L"Output Format",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y + 3, layout->labelW, 20, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Format dropdown */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST,
        layout->controlX, layout->y, layout->controlW, 120, hwnd, (HMENU)ID_CMB_FORMAT, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.264) - Best compatibility");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.265) - Smaller files, less compatible");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"AVI - Legacy format");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"WMV - Windows Media");
    SendMessage(ctl, CB_SETCURSEL, g_config.outputFormat, 0);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    layout->y += layout->rowH;
    
    /* Quality label */
    ctl = CreateWindowW(L"STATIC", L"Quality",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y + 3, layout->labelW, 20, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Quality dropdown */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST,
        layout->controlX, layout->y, layout->controlW, 120, hwnd, (HMENU)ID_CMB_QUALITY, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"Good ~60 Mbps (YouTube, TikTok)");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"High ~75 Mbps (Discord, Twitter/X)");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"Ultra ~90 Mbps (Archival, editing)");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"Lossless ~130 Mbps (No artifacts)");
    SendMessage(ctl, CB_SETCURSEL, g_config.quality, 0);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    layout->y += layout->rowH;
    
    /* Frame Rate label */
    ctl = CreateWindowW(L"STATIC", L"Frame Rate",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y + 3, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Frame Rate dropdown */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST,
        layout->controlX, layout->y, 120, 120, hwnd, (HMENU)ID_CMB_REPLAY_FPS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"30 fps");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"60 fps");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"120 fps");
    SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)L"240 fps");
    int fpsIdx = 1; /* Default 60fps */
    int fpsValues[] = { 30, 60, 120, 240 };
    for (int i = 0; i < 4; i++) {
        if (g_config.replayFPS == fpsValues[i]) { fpsIdx = i; break; }
    }
    SendMessage(ctl, CB_SETCURSEL, fpsIdx, 0);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Note about FPS usage */
    ctl = CreateWindowW(L"STATIC", L"(used for replay buffer; regular recording captures at display refresh rate)",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 130, layout->y + 3, 320, 20, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    layout->y += layout->rowH + 8;
    
    /* ========== REPLAY BUFFER SECTION ========== */
    
    /* Enable replay checkbox */
    ctl = CreateWindowW(L"BUTTON", L"Enable replay buffer (records constantly in background)",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX, layout->y, 400, 24, hwnd, (HMENU)ID_CHK_REPLAY_ENABLED, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_REPLAY_ENABLED, g_config.replayEnabled ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    layout->y += 32;
    
    /* Source label */
    ctl = CreateWindowW(L"STATIC", L"Source",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Source dropdown */
    HWND cmbSource = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX, layout->y, 250, 200, hwnd, (HMENU)ID_CMB_REPLAY_SOURCE, s_hInstance, NULL);
    CHECK_CTL(cmbSource);
    SendMessage(cmbSource, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(cmbSource, s_videoControls, &s_videoControlCount);
    
    int monCount = GetSystemMetrics(SM_CMONITORS);
    for (int i = 0; i < monCount; i++) {
        WCHAR buf[64];
        RECT monBounds;
        BOOL isPrimary = FALSE;
        if (Capture_GetMonitorBoundsByIndex(i, &monBounds)) {
            POINT pt = { monBounds.left, monBounds.top };
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (hMon) {
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                    isPrimary = TRUE;
                }
            }
        }
        if (isPrimary) {
            wsprintfW(buf, L"Monitor %d (primary)", i + 1);
        } else {
            wsprintfW(buf, L"Monitor %d", i + 1);
        }
        SendMessageW(cmbSource, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageW(cmbSource, CB_ADDSTRING, 0, (LPARAM)L"Window (click to select)");
    SendMessageW(cmbSource, CB_ADDSTRING, 0, (LPARAM)L"Custom Area");
    
    int srcIdx = 0;
    switch (g_config.replayCaptureSource) {
        case MODE_MONITOR: srcIdx = g_config.replayMonitorIndex; break;
        case MODE_WINDOW:  srcIdx = monCount; break;
        case MODE_AREA:    srcIdx = monCount + 1; break;
    }
    SendMessage(cmbSource, CB_SETCURSEL, srcIdx, 0);
    layout->y += layout->rowH;
    
    /* Aspect Ratio label */
    ctl = CreateWindowW(L"STATIC", L"Aspect Ratio",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Aspect dropdown — item data stores the Util_GetAspectRatioDimensions index */
    HWND cmbAspect = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST,
        layout->controlX, layout->y, 180, 120, hwnd, (HMENU)ID_CMB_REPLAY_ASPECT, s_hInstance, NULL);
    CHECK_CTL(cmbAspect);
    SendMessage(cmbAspect, WM_SETFONT, (WPARAM)layout->font, TRUE);
    
    /* Each entry: {label, aspectIndex matching Util_GetAspectRatioDimensions} */
    struct { const WCHAR* label; int aspectIdx; } aspectItems[] = {
        { L"Native (full screen)", 0 },
        { L"16:9 (centered)",      1 },
        { L"4:3 (centered)",       6 },
        { L"21:9 (ultrawide)",     7 },
    };
    int aspectItemCount = sizeof(aspectItems) / sizeof(aspectItems[0]);
    int selectedComboIdx = 0;
    for (int i = 0; i < aspectItemCount; i++) {
        int idx = (int)SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)aspectItems[i].label);
        SendMessage(cmbAspect, CB_SETITEMDATA, idx, (LPARAM)aspectItems[i].aspectIdx);
        if (aspectItems[i].aspectIdx == g_config.replayAspectRatio) {
            selectedComboIdx = idx;
        }
    }
    SendMessage(cmbAspect, CB_SETCURSEL, selectedComboIdx, 0);
    BOOL enableAspect = (g_config.replayCaptureSource == MODE_MONITOR);
    EnableWindow(cmbAspect, enableAspect);
    AddToSection(cmbAspect, s_videoControls, &s_videoControlCount);
    layout->y += layout->rowH;
    
    /* Duration label */
    ctl = CreateWindowW(L"STATIC", L"Duration",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    int durSecs = g_config.replayDuration;
    if (durSecs < 1) durSecs = 30;
    int durH = durSecs / 3600;
    int durM = (durSecs % 3600) / 60;
    int durS = durSecs % 60;
    
    /* Duration hours */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX, layout->y, 50, 300, hwnd, (HMENU)ID_CMB_REPLAY_HOURS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    for (int i = 0; i <= 1; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(ctl, CB_SETCURSEL, durH > 1 ? 1 : durH, 0);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    ctl = CreateWindowW(L"STATIC", L"h",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 53, layout->y, 15, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Duration minutes */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX + 70, layout->y, 50, 300, hwnd, (HMENU)ID_CMB_REPLAY_MINS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    for (int i = 0; i <= 59; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(ctl, CB_SETCURSEL, durM, 0);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    ctl = CreateWindowW(L"STATIC", L"m",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 123, layout->y, 18, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Duration seconds */
    ctl = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->controlX + 142, layout->y, 50, 300, hwnd, (HMENU)ID_CMB_REPLAY_SECS, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    for (int i = 0; i <= 59; i++) {
        WCHAR buf[8]; wsprintfW(buf, L"%d", i);
        SendMessageW(ctl, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(ctl, CB_SETCURSEL, durS, 0);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    ctl = CreateWindowW(L"STATIC", L"s",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 195, layout->y, 15, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    layout->y += layout->rowH;
    
    /* RAM estimate label */
    ctl = CreateWindowW(L"STATIC", L"RAM Usage",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* RAM value display */
    ctl = CreateWindowW(L"STATIC", L"~0 MB RAM",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX, layout->y, 100, 26, hwnd, (HMENU)ID_STATIC_REPLAY_RAM, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* RAM calculation details */
    ctl = CreateWindowW(L"STATIC", L"",
        WS_CHILD | SS_CENTERIMAGE,
        layout->controlX + 105, layout->y, 250, 26, hwnd, (HMENU)ID_STATIC_REPLAY_CALC, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    layout->y += layout->rowH;
    
    /* Save hotkey label */
    ctl = CreateWindowW(L"STATIC", L"Save Hotkey",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, layout->labelW, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
    
    /* Save hotkey button */
    char keyName[64];
    GetKeyNameFromVK(g_config.replaySaveKey, keyName, sizeof(keyName));
    ctl = CreateWindowExA(0, "BUTTON", keyName,
        WS_CHILD | BS_PUSHBUTTON,
        layout->controlX, layout->y, 100, 24, hwnd, (HMENU)ID_BTN_REPLAY_HOTKEY, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_videoControls, &s_videoControlCount);
}

/**
 * Create AUDIO tab controls:
 * - Enable checkbox
 * - Source 1 dropdown + volume slider + edit
 * - Source 2 dropdown + volume slider + edit
 * - Source 3 dropdown + volume slider + edit
 */
static void CreateAudioSection(HWND hwnd, SettingsLayout* layout) {
    s_audioControlCount = 0;
    HWND ctl;
    
    /* Enable checkbox */
    ctl = CreateWindowW(L"BUTTON", L"Enable audio recording",
        WS_CHILD | BS_AUTOCHECKBOX,
        layout->labelX, layout->y, 250, 24, hwnd, (HMENU)ID_CHK_AUDIO_ENABLED, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    CheckDlgButton(hwnd, ID_CHK_AUDIO_ENABLED, g_config.audioEnabled ? BST_CHECKED : BST_UNCHECKED);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    layout->y += 32;
    
    /* Enumerate audio devices */
    static AudioDeviceList audioDevices;
    AudioDevice_Enumerate(&audioDevices);
    
    /* Audio source 1 */
    ctl = CreateWindowW(L"STATIC", L"Source 1",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, 60, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    HWND cmbAudio1 = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->labelX + 65, layout->y, 330, 200, hwnd, (HMENU)ID_CMB_AUDIO_SOURCE1, s_hInstance, NULL);
    CHECK_CTL(cmbAudio1);
    SendMessage(cmbAudio1, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(cmbAudio1, s_audioControls, &s_audioControlCount);
    
    ctl = CreateWindowW(TRACKBAR_CLASSW, L"",
        WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        layout->labelX + 400, layout->y, 120, 26, hwnd, (HMENU)ID_SLD_AUDIO_VOLUME1, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, TBM_SETRANGE, TRUE, MAKELPARAM(0, AUDIO_VOLUME_MAX));
    SendMessage(ctl, TBM_SETPOS, TRUE, g_config.audioVolume1);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    char volBuf1[16];
    snprintf(volBuf1, sizeof(volBuf1), "%d", g_config.audioVolume1);
    ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_NUMBER | ES_CENTER,
        layout->labelX + 525, layout->y + 2, 40, 20, hwnd, (HMENU)ID_LBL_AUDIO_VOL1, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
    SetWindowTextA(ctl, volBuf1);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    layout->y += 30;
    
    /* Audio source 2 */
    ctl = CreateWindowW(L"STATIC", L"Source 2",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, 60, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    HWND cmbAudio2 = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->labelX + 65, layout->y, 330, 200, hwnd, (HMENU)ID_CMB_AUDIO_SOURCE2, s_hInstance, NULL);
    CHECK_CTL(cmbAudio2);
    SendMessage(cmbAudio2, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(cmbAudio2, s_audioControls, &s_audioControlCount);
    
    ctl = CreateWindowW(TRACKBAR_CLASSW, L"",
        WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        layout->labelX + 400, layout->y, 120, 26, hwnd, (HMENU)ID_SLD_AUDIO_VOLUME2, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, TBM_SETRANGE, TRUE, MAKELPARAM(0, AUDIO_VOLUME_MAX));
    SendMessage(ctl, TBM_SETPOS, TRUE, g_config.audioVolume2);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    char volBuf2[16];
    snprintf(volBuf2, sizeof(volBuf2), "%d", g_config.audioVolume2);
    ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_NUMBER | ES_CENTER,
        layout->labelX + 525, layout->y + 2, 40, 20, hwnd, (HMENU)ID_LBL_AUDIO_VOL2, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
    SetWindowTextA(ctl, volBuf2);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    layout->y += 30;
    
    /* Audio source 3 */
    ctl = CreateWindowW(L"STATIC", L"Source 3",
        WS_CHILD | SS_CENTERIMAGE,
        layout->labelX, layout->y, 60, 26, hwnd, NULL, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    HWND cmbAudio3 = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        layout->labelX + 65, layout->y, 330, 200, hwnd, (HMENU)ID_CMB_AUDIO_SOURCE3, s_hInstance, NULL);
    CHECK_CTL(cmbAudio3);
    SendMessage(cmbAudio3, WM_SETFONT, (WPARAM)layout->font, TRUE);
    AddToSection(cmbAudio3, s_audioControls, &s_audioControlCount);
    
    ctl = CreateWindowW(TRACKBAR_CLASSW, L"",
        WS_CHILD | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
        layout->labelX + 400, layout->y, 120, 26, hwnd, (HMENU)ID_SLD_AUDIO_VOLUME3, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, TBM_SETRANGE, TRUE, MAKELPARAM(0, AUDIO_VOLUME_MAX));
    SendMessage(ctl, TBM_SETPOS, TRUE, g_config.audioVolume3);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    char volBuf3[16];
    snprintf(volBuf3, sizeof(volBuf3), "%d", g_config.audioVolume3);
    ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_NUMBER | ES_CENTER,
        layout->labelX + 525, layout->y + 2, 40, 20, hwnd, (HMENU)ID_LBL_AUDIO_VOL3, s_hInstance, NULL);
    CHECK_CTL(ctl);
    SendMessage(ctl, WM_SETFONT, (WPARAM)layout->smallFont, TRUE);
    SetWindowTextA(ctl, volBuf3);
    AddToSection(ctl, s_audioControls, &s_audioControlCount);
    
    /* Populate audio dropdowns */
    SendMessage(cmbAudio1, CB_SETCURSEL, PopulateAudioDropdown(cmbAudio1, &audioDevices, g_config.audioSource1), 0);
    SendMessage(cmbAudio2, CB_SETCURSEL, PopulateAudioDropdown(cmbAudio2, &audioDevices, g_config.audioSource2), 0);
    SendMessage(cmbAudio3, CB_SETCURSEL, PopulateAudioDropdown(cmbAudio3, &audioDevices, g_config.audioSource3), 0);
}

/* ============================================================================
 * SETTINGS WINDOW PROCEDURE
 * ============================================================================ */

/* Replay-duration apply-on-close: in-place Stop/Start during editing races the
 * buffer thread and leaks the NVENC encoder when Stop times out (see crash
 * 2026-05-27). We now collect the new duration into g_config during editing
 * and perform a single Stop/Start only when the settings dialog closes. The
 * actual Stop/Start runs on a worker thread so the UI does not stall (Stop can
 * block up to 5s waiting for the buffer thread to drain). */
static int s_replayDurationAtOpen = -1;
static LONG s_replayReloadInFlight = 0;

static DWORD WINAPI ReplayReloadWorker(LPVOID param) {
    (void)param;
    Logger_Log("Replay reload worker: starting Stop/Start for duration=%ds\n",
               g_config.replayDuration);
    Logger_ResetHeartbeat(THREAD_BUFFER);
    ReplayBuffer_Stop(&g_replayBuffer);
    ReplayBuffer_Start(&g_replayBuffer, &g_config);
    Logger_Log("Replay reload worker: done\n");
    InterlockedExchange(&s_replayReloadInFlight, 0);
    return 0;
}

/* Schedule an async replay buffer Stop/Start.
 * Used by FPS, audio source 1/2/3, and duration-on-close handlers.
 * In-place Stop/Start on the UI thread races the buffer thread and leaks
 * the NVENC encoder when Stop times out (crash 2026-05-27).
 * The in-flight guard collapses rapid edits into one reload; the worker
 * re-reads g_config when it runs, so the final values win. */
static void ScheduleReplayReload(const char* reason) {
    if (!g_config.replayEnabled || !g_replayBuffer.isBuffering) return;
    if (InterlockedCompareExchange(&s_replayReloadInFlight, 1, 0) != 0) {
        Logger_Log("ScheduleReplayReload(%s): reload already in flight, skipping\n",
                   reason ? reason : "?");
        return;
    }
    Logger_Log("ScheduleReplayReload(%s): dispatching worker (async)\n",
               reason ? reason : "?");
    HANDLE hThr = CreateThread(NULL, 0, ReplayReloadWorker, NULL, 0, NULL);
    if (hThr) {
        CloseHandle(hThr);
    } else {
        Logger_Log("ScheduleReplayReload: CreateThread failed, falling back to inline\n");
        ReplayReloadWorker(NULL);
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            s_replayDurationAtOpen = g_config.replayDuration;
            /* Create fonts (NULL-checked: GDI failures yield NULL handles that
             * silently no-op on WM_SETFONT — log so failures aren't invisible). */
            s_settingsFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            s_settingsSmallFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            s_settingsBgBrush = CreateSolidBrush(RGB(32, 32, 32));
            s_editBrush = CreateSolidBrush(RGB(45, 45, 45));
            if (!s_settingsFont || !s_settingsSmallFont || !s_settingsBgBrush || !s_editBrush) {
                Logger_Log("WM_CREATE: GDI allocation failed (font=%p smallFont=%p bg=%p edit=%p)\n",
                           (void*)s_settingsFont, (void*)s_settingsSmallFont,
                           (void*)s_settingsBgBrush, (void*)s_editBrush);
            }
            
            /* Initialize layout parameters - same starting position for all tabs */
            SettingsLayout layout = {
                .y = 16,
                .labelX = 20,
                .labelW = 100,
                .controlX = 130,
                .controlW = 460,
                .rowH = 32,
                .contentW = 570,
                .font = s_settingsFont,
                .smallFont = s_settingsSmallFont
            };
            
            /* Create all tab sections - each resets layout.y internally */
            /* Controls are created without WS_VISIBLE - SwitchToTab shows them */
            SettingsLayout generalLayout = layout;
            CreateGeneralSection(hwnd, &generalLayout);
            
            SettingsLayout videoLayout = layout;
            CreateVideoSection(hwnd, &videoLayout);
            
            SettingsLayout audioLayout = layout;
            CreateAudioSection(hwnd, &audioLayout);
            
            /* Show default tab (General) and update RAM estimate.
             * SwitchToTab assigns s_currentTab itself; no separate assignment needed. */
            SwitchToTab(SETTINGS_TAB_GENERAL);
            UpdateReplayRAMEstimate(hwnd);
            
            /* Show replay preview if enabled */
            if (g_config.replayEnabled) {
                UpdateReplayPreview();
            }
            
            return 0;
        }
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, s_settingsBgBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)s_settingsBgBrush;
        }
        
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkColor(hdc, RGB(45, 45, 45));
            return (LRESULT)s_editBrush;
        }
        
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_CMB_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.outputFormat = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_FORMAT), CB_GETCURSEL, 0, 0);
                    }
                    break;
                    
                case ID_CMB_QUALITY:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.quality = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_QUALITY), CB_GETCURSEL, 0, 0);
                    }
                    break;
                    
                case ID_CHK_MOUSE:
                    g_config.captureMouse = (IsDlgButtonChecked(hwnd, ID_CHK_MOUSE) == BST_CHECKED);
                    break;
                    
                case ID_CHK_BORDER:
                    g_config.showRecordingBorder = (IsDlgButtonChecked(hwnd, ID_CHK_BORDER) == BST_CHECKED);
                    break;
                    
                case ID_CMB_HOURS:
                case ID_CMB_MINUTES:
                case ID_CMB_SECONDS:
                    /* Time limit dropdowns - just save on close */
                    break;
                    
                case ID_BTN_BROWSE: {
                    BROWSEINFOW bi = { 0 };
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = L"Select save location";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        WCHAR path[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, path)) {
                            char pathA[MAX_PATH];
                            WideCharToMultiByte(CP_ACP, 0, path, -1, pathA, MAX_PATH, NULL, NULL);
                            pathA[MAX_PATH - 1] = '\0';
                            SetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), pathA);
                            strncpy(g_config.savePath, pathA, MAX_PATH - 1);
                            g_config.savePath[MAX_PATH - 1] = '\0';
                        }
                        SAFE_COTASKMEM_FREE(pidl);
                    }
                    break;
                }
                
                case ID_CHK_REPLAY_ENABLED: {
                    BOOL wasEnabled = g_config.replayEnabled;
                    g_config.replayEnabled = IsDlgButtonChecked(hwnd, ID_CHK_REPLAY_ENABLED) == BST_CHECKED;
                    Logger_Log("Replay enabled toggled: %d -> %d\n", wasEnabled, g_config.replayEnabled);
                    
                    if (g_config.replayEnabled && !wasEnabled) {
                        Logger_Log("Starting replay buffer from settings\n");
                        ReplayBuffer_Start(&g_replayBuffer, &g_config);
                        /* CheckAudioError called from overlay.c if needed */
                        BOOL ok = RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
                        Logger_Log("RegisterHotKey from settings: %s (key=0x%02X)\n", ok ? "OK" : "FAILED", g_config.replaySaveKey);
                    } else if (!g_config.replayEnabled && wasEnabled) {
                        Logger_Log("Stopping replay buffer from settings\n");
                        UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
                        ReplayBuffer_Stop(&g_replayBuffer);
                    }
                    break;
                }
                    
                case ID_CMB_REPLAY_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int sel = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SOURCE), CB_GETCURSEL, 0, 0);
                        int monCount = GetSystemMetrics(SM_CMONITORS);
                        
                        if (sel < monCount) {
                            g_config.replayCaptureSource = MODE_MONITOR;
                            g_config.replayMonitorIndex = sel;
                        } else if (sel == monCount) {
                            g_config.replayCaptureSource = MODE_WINDOW;
                        } else {
                            g_config.replayCaptureSource = MODE_AREA;
                        }
                        
                        BOOL enableAspect = (g_config.replayCaptureSource == MODE_MONITOR);
                        EnableWindow(GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT), enableAspect);
                        UpdateReplayPreview();
                    }
                    break;
                    
                case ID_CMB_REPLAY_HOURS:
                case ID_CMB_REPLAY_MINS:
                case ID_CMB_REPLAY_SECS:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int h = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
                        int m = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
                        int s = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
                        int total = h * 3600 + m * 60 + s;
                        if (total < 1) total = 1;
                        g_config.replayDuration = total;
                        UpdateReplayRAMEstimate(hwnd);
                        /* Applied on WM_CLOSE to avoid Stop/Start races. */
                    }
                    break;
                    
                case ID_CMB_REPLAY_ASPECT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        HWND cmbAspect = GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT);
                        int sel = (int)SendMessage(cmbAspect, CB_GETCURSEL, 0, 0);
                        g_config.replayAspectRatio = (int)SendMessage(cmbAspect, CB_GETITEMDATA, sel, 0);
                        
                        if (g_config.replayAspectRatio > 0) {
                            g_config.replayAreaRect.left = 0;
                            g_config.replayAreaRect.top = 0;
                            g_config.replayAreaRect.right = 0;
                            g_config.replayAreaRect.bottom = 0;
                        }
                        
                        UpdateReplayPreview();
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                
                case ID_CMB_REPLAY_FPS:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_FPS), CB_GETCURSEL, 0, 0);
                        int fpsValues[] = { 30, 60, 120, 240 };
                        int newFPS = (idx >= 0 && idx < 4) ? fpsValues[idx] : 60;
                        
                        if (newFPS != g_config.replayFPS) {
                            int oldFPS = g_config.replayFPS;
                            g_config.replayFPS = newFPS;
                            Logger_Log("Replay FPS changed: %d -> %d\n", oldFPS, newFPS);
                            ScheduleReplayReload("FPS change");
                        }
                        
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                    
                case ID_BTN_REPLAY_HOTKEY:
                    s_waitingForHotkey = TRUE;
                    SetWindowTextA(GetDlgItem(hwnd, ID_BTN_REPLAY_HOTKEY), "Press a key...");
                    SetFocus(hwnd);
                    break;
                
                case ID_BTN_MARKER_HOTKEY:
                    s_waitingForMarkerHotkey = TRUE;
                    SetWindowTextA(GetDlgItem(hwnd, ID_BTN_MARKER_HOTKEY), "Press a key...");
                    SetFocus(hwnd);
                    break;
                    
                case ID_CHK_AUDIO_ENABLED:
                    if (IsDlgButtonChecked(hwnd, ID_CHK_AUDIO_ENABLED) == BST_CHECKED) {
                        if (!AACEncoder_IsAvailable()) {
                            MessageBoxA(hwnd,
                                "AAC audio encoder is not available on this system.\n\n"
                                "Audio recording requires the Microsoft AAC encoder which is\n"
                                "included with Windows Media Feature Pack.\n\n"
                                "On Windows N/KN editions, install the Media Feature Pack from:\n"
                                "Settings > Apps > Optional features > Add a feature",
                                "Audio Encoder Not Available",
                                MB_OK | MB_ICONWARNING);
                            SendMessage(GetDlgItem(hwnd, ID_CHK_AUDIO_ENABLED), BM_SETCHECK, BST_UNCHECKED, 0);
                            g_config.audioEnabled = FALSE;
                        } else {
                            g_config.audioEnabled = TRUE;
                        }
                    } else {
                        g_config.audioEnabled = FALSE;
                    }
                    break;
                
                case ID_CHK_DEBUG_LOGGING:
                    g_config.debugLogging = (IsDlgButtonChecked(hwnd, ID_CHK_DEBUG_LOGGING) == BST_CHECKED);
                    if (g_config.debugLogging && !Logger_IsInitialized()) {
                        char exePath[MAX_PATH];
                        char debugFolder[MAX_PATH];
                        char logFilename[MAX_PATH];
                        GetModuleFileNameA(NULL, exePath, MAX_PATH);
                        char* lastSlash = strrchr(exePath, '\\');
                        if (lastSlash) {
                            *lastSlash = '\0';
                            snprintf(debugFolder, sizeof(debugFolder), "%s\\Debug", exePath);
                            CreateDirectoryA(debugFolder, NULL);
                            SYSTEMTIME st;
                            GetLocalTime(&st);
                            snprintf(logFilename, sizeof(logFilename), "%s\\lwsr_log_%04d%02d%02d_%02d%02d%02d.txt",
                                    debugFolder, (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
                            if (!Logger_Init(logFilename, "w")) {
                                MessageBoxA(hwnd,
                                    "Failed to create debug log file. Check write permissions for the Debug folder.",
                                    "Debug Logging", MB_OK | MB_ICONWARNING);
                                g_config.debugLogging = FALSE;
                                CheckDlgButton(hwnd, ID_CHK_DEBUG_LOGGING, BST_UNCHECKED);
                            } else {
                                Logger_Log("Debug logging enabled (live toggle)\n");
                            }
                        }
                    } else if (!g_config.debugLogging && Logger_IsInitialized()) {
                        Logger_Log("Debug logging disabled (live toggle)\n");
                        Logger_Shutdown();
                    }
                    break;
                    
                case ID_CHK_AUTOCLIP_ENABLED:
                    g_config.autoClipEnabled = (IsDlgButtonChecked(hwnd, ID_CHK_AUTOCLIP_ENABLED) == BST_CHECKED);
                    break;
                
                case ID_CHK_AUTOCLIP_SHOW_REGIONS:
                    g_config.autoClipShowRegions = (IsDlgButtonChecked(hwnd, ID_CHK_AUTOCLIP_SHOW_REGIONS) == BST_CHECKED);
                    /* Toggle calibration region overlay */
                    if (g_config.autoClipShowRegions)
                        AutoClipRegionOverlay_Show();
                    else
                        AutoClipRegionOverlay_Hide();
                    break;
                
                case ID_CHK_AUTOCLIP_DEBUG_CONSOLE:
                    if (IsDlgButtonChecked(hwnd, ID_CHK_AUTOCLIP_DEBUG_CONSOLE) == BST_CHECKED)
                        DebugConsole_Open();
                    else
                        DebugConsole_Close();
                    break;
                
                case ID_CMB_AUDIO_SOURCE1:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE1), CB_GETCURSEL, 0, 0);
                        char* deviceId = (char*)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE1), CB_GETITEMDATA, idx, 0);
                        if (deviceId && deviceId != (char*)CB_ERR) {
                            strncpy(g_config.audioSource1, deviceId, sizeof(g_config.audioSource1) - 1);
                            g_config.audioSource1[sizeof(g_config.audioSource1) - 1] = '\0';
                        } else {
                            g_config.audioSource1[0] = '\0';
                        }
                        ScheduleReplayReload("audio source 1");
                    }
                    break;
                    
                case ID_CMB_AUDIO_SOURCE2:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE2), CB_GETCURSEL, 0, 0);
                        char* deviceId = (char*)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE2), CB_GETITEMDATA, idx, 0);
                        if (deviceId && deviceId != (char*)CB_ERR) {
                            strncpy(g_config.audioSource2, deviceId, sizeof(g_config.audioSource2) - 1);
                            g_config.audioSource2[sizeof(g_config.audioSource2) - 1] = '\0';
                        } else {
                            g_config.audioSource2[0] = '\0';
                        }
                        ScheduleReplayReload("audio source 2");
                    }
                    break;
                    
                case ID_CMB_AUDIO_SOURCE3:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE3), CB_GETCURSEL, 0, 0);
                        char* deviceId = (char*)SendMessage(GetDlgItem(hwnd, ID_CMB_AUDIO_SOURCE3), CB_GETITEMDATA, idx, 0);
                        if (deviceId && deviceId != (char*)CB_ERR) {
                            strncpy(g_config.audioSource3, deviceId, sizeof(g_config.audioSource3) - 1);
                            g_config.audioSource3[sizeof(g_config.audioSource3) - 1] = '\0';
                        } else {
                            g_config.audioSource3[0] = '\0';
                        }
                        ScheduleReplayReload("audio source 3");
                    }
                    break;
                
                /* Volume edit boxes - sync to slider on focus loss */
                case ID_LBL_AUDIO_VOL1:
                case ID_LBL_AUDIO_VOL2:
                case ID_LBL_AUDIO_VOL3:
                    if (HIWORD(wParam) == EN_KILLFOCUS) {
                        int ctrlId = LOWORD(wParam);
                        char buf[16];
                        GetWindowTextA(GetDlgItem(hwnd, ctrlId), buf, sizeof(buf));
                        int val = atoi(buf);
                        
                        if (val < 0) val = 0;
                        if (val > AUDIO_VOLUME_MAX) val = AUDIO_VOLUME_MAX;
                        
                        if (ctrlId == ID_LBL_AUDIO_VOL1) {
                            g_config.audioVolume1 = val;
                            SendMessage(GetDlgItem(hwnd, ID_SLD_AUDIO_VOLUME1), TBM_SETPOS, TRUE, val);
                        } else if (ctrlId == ID_LBL_AUDIO_VOL2) {
                            g_config.audioVolume2 = val;
                            SendMessage(GetDlgItem(hwnd, ID_SLD_AUDIO_VOLUME2), TBM_SETPOS, TRUE, val);
                        } else if (ctrlId == ID_LBL_AUDIO_VOL3) {
                            g_config.audioVolume3 = val;
                            SendMessage(GetDlgItem(hwnd, ID_SLD_AUDIO_VOLUME3), TBM_SETPOS, TRUE, val);
                        }
                        
                        snprintf(buf, sizeof(buf), "%d", val);
                        SetWindowTextA(GetDlgItem(hwnd, ctrlId), buf);
                    }
                    break;
            }
            return 0;
        }
        
        case WM_HSCROLL: {
            HWND hSlider = (HWND)lParam;
            int ctrlId = GetDlgCtrlID(hSlider);
            int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
            char buf[16];
            
            if (ctrlId == ID_SLD_AUDIO_VOLUME1) {
                g_config.audioVolume1 = pos;
                snprintf(buf, sizeof(buf), "%d", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUDIO_VOL1), buf);
            } else if (ctrlId == ID_SLD_AUDIO_VOLUME2) {
                g_config.audioVolume2 = pos;
                snprintf(buf, sizeof(buf), "%d", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUDIO_VOL2), buf);
            } else if (ctrlId == ID_SLD_AUDIO_VOLUME3) {
                g_config.audioVolume3 = pos;
                snprintf(buf, sizeof(buf), "%d", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUDIO_VOL3), buf);
            } else if (ctrlId == ID_SLD_AUTOCLIP_COOLDOWN) {
                g_config.autoClipCooldownSec = pos;
                snprintf(buf, sizeof(buf), "%d", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUTOCLIP_COOLDOWN), buf);
            } else if (ctrlId == ID_SLD_AUTOCLIP_DELAY) {
                g_config.autoClipDelaySec = pos;
                snprintf(buf, sizeof(buf), "%d", pos);
                SetWindowTextA(GetDlgItem(hwnd, ID_LBL_AUTOCLIP_DELAY), buf);
            }
            return 0;
        }
        
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (s_waitingForHotkey) {
                int vk = (int)wParam;
                
                /* Ignore modifier keys alone */
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || 
                    vk == VK_LSHIFT || vk == VK_RSHIFT || 
                    vk == VK_LCONTROL || vk == VK_RCONTROL ||
                    vk == VK_LMENU || vk == VK_RMENU) {
                    return 0;
                }
                
                if (g_config.replayEnabled) {
                    Logger_Log("Unregistering old hotkey (key change)\n");
                    UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
                }
                
                g_config.replaySaveKey = vk;
                Logger_Log("Hotkey changed to VK=0x%02X\n", vk);
                
                if (g_config.replayEnabled) {
                    BOOL ok = RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
                    Logger_Log("RegisterHotKey (key change): %s (key=0x%02X)\n", ok ? "OK" : "FAILED", g_config.replaySaveKey);
                }
                
                char keyName[64];
                GetKeyNameFromVK(vk, keyName, sizeof(keyName));
                SetWindowTextA(GetDlgItem(hwnd, ID_BTN_REPLAY_HOTKEY), keyName);
                
                s_waitingForHotkey = FALSE;
                return 0;
            }
            if (s_waitingForMarkerHotkey) {
                int vk = (int)wParam;
                
                /* Ignore modifier keys alone */
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || 
                    vk == VK_LSHIFT || vk == VK_RSHIFT || 
                    vk == VK_LCONTROL || vk == VK_RCONTROL ||
                    vk == VK_LMENU || vk == VK_RMENU) {
                    return 0;
                }
                
                /* Unregister old, update config, re-register */
                UnregisterHotKey(g_controlWnd, HOTKEY_MARKER);
                g_config.markerKey = vk;
                Logger_Log("Marker hotkey changed to VK=0x%02X\n", vk);
                
                BOOL ok = RegisterHotKey(g_controlWnd, HOTKEY_MARKER, 0, g_config.markerKey);
                Logger_Log("RegisterHotKey(HOTKEY_MARKER): %s (key=0x%02X)\n", ok ? "OK" : "FAILED", g_config.markerKey);
                
                char keyName[64];
                GetKeyNameFromVK(vk, keyName, sizeof(keyName));
                SetWindowTextA(GetDlgItem(hwnd, ID_BTN_MARKER_HOTKEY), keyName);
                
                s_waitingForMarkerHotkey = FALSE;
                return 0;
            }
            break;
            
        case WM_TIMER:
            break;

        case WM_CLOSE: {
            /* Apply replay duration change once, on close, to avoid the race
             * where rapid in-place Stop/Start hangs and leaks the encoder.
             * Runs on a worker thread so the UI close stays responsive. */
            if (s_replayDurationAtOpen >= 0 &&
                g_config.replayDuration != s_replayDurationAtOpen) {
                Logger_Log("Scheduling replay duration change on close: %ds -> %ds (async)\n",
                           s_replayDurationAtOpen, g_config.replayDuration);
                ScheduleReplayReload("duration on close");
            }
            s_replayDurationAtOpen = -1;
            SaveAreaSelectorPosition();
            AreaSelector_Hide();
            
            /* Save time limit */
            int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
            int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
            int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
            int total = hours * 3600 + mins * 60 + secs;
            if (total < 1) total = 1;
            g_config.maxRecordingSeconds = total;
            
            /* Save replay duration */
            int rh = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
            int rm = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
            int rs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
            int replayTotal = rh * 3600 + rm * 60 + rs;
            if (replayTotal < 1) replayTotal = 1;
            g_config.replayDuration = replayTotal;
            
            /* Save path */
            GetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), g_config.savePath, MAX_PATH);
            Config_Save(&g_config);
            
            AreaSelector_Shutdown();
            
            /* GDI cleanup happens in WM_DESTROY (single source of truth). */
            DestroyWindow(hwnd);
            s_settingsWnd = NULL;
            if (s_externalHandleRef) *s_externalHandleRef = NULL;
            
            /* Refresh mode buttons and settings button in control panel.
             * IDs imported from overlay.h. */
            if (g_controlWnd) {
                InvalidateRect(GetDlgItem(g_controlWnd, ID_MODE_AREA), NULL, TRUE);
                InvalidateRect(GetDlgItem(g_controlWnd, ID_MODE_WINDOW), NULL, TRUE);
                InvalidateRect(GetDlgItem(g_controlWnd, ID_MODE_MONITOR), NULL, TRUE);
                InvalidateRect(GetDlgItem(g_controlWnd, ID_BTN_SETTINGS), NULL, TRUE);
            }
            return 0;
        }
        
        case WM_DESTROY:
            if (s_settingsFont) { DeleteObject(s_settingsFont); s_settingsFont = NULL; }
            if (s_settingsSmallFont) { DeleteObject(s_settingsSmallFont); s_settingsSmallFont = NULL; }
            if (s_settingsBgBrush) { DeleteObject(s_settingsBgBrush); s_settingsBgBrush = NULL; }
            if (s_editBrush) { DeleteObject(s_editBrush); s_editBrush = NULL; }
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

BOOL SettingsDialog_Register(HINSTANCE hInstance) {
    s_hInstance = hInstance;
    
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = "LWSRSettings";
    /* Load icons from EXE resource for taskbar (NULL falls back to default icon) */
    wc.hIcon = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (!wc.hIcon) Logger_Log("settings_dialog: LoadImageA(hIcon) failed err=%lu\n",
                              (unsigned long)GetLastError());
    wc.hIconSm = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIconSm) Logger_Log("settings_dialog: LoadImageA(hIconSm) failed err=%lu\n",
                                (unsigned long)GetLastError());
    
    return RegisterClassExA(&wc) != 0;
}

HWND SettingsDialog_ShowAt(HINSTANCE hInstance, int x, int y) {
    if (s_settingsWnd) {
        /* Already open - bring to front */
        SetForegroundWindow(s_settingsWnd);
        return s_settingsWnd;
    }
    
    s_hInstance = hInstance;
    
    s_settingsWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_COMPOSITED,
        "LWSRSettings",
        NULL,
        WS_POPUP | WS_VISIBLE | WS_BORDER | WS_CLIPCHILDREN,
        x, y, SETTINGS_WIDTH, SETTINGS_HEIGHT,
        NULL, NULL, hInstance, NULL);
    
    if (s_settingsWnd) {
        if (s_externalHandleRef) *s_externalHandleRef = s_settingsWnd;
        ShowWindow(s_settingsWnd, SW_SHOW);
        UpdateWindow(s_settingsWnd);
    } else {
        Logger_Log("settings_dialog: CreateWindowExA(LWSRSettings) failed err=%lu\n",
                   (unsigned long)GetLastError());
    }
    
    return s_settingsWnd;
}

void SettingsDialog_Close(void) {
    if (s_settingsWnd) {
        SendMessage(s_settingsWnd, WM_CLOSE, 0, 0);
    }
}

HWND SettingsDialog_GetHandle(void) {
    return s_settingsWnd;
}

BOOL SettingsDialog_IsVisible(void) {
    return s_settingsWnd != NULL && IsWindowVisible(s_settingsWnd);
}

void SettingsDialog_SetExternalHandle(HWND* externalRef) {
    s_externalHandleRef = externalRef;
}

SettingsTab SettingsDialog_GetCurrentTab(void) {
    return s_currentTab;
}

void SettingsDialog_SwitchTab(SettingsTab tab) {
    if (s_settingsWnd && tab >= SETTINGS_TAB_GENERAL && tab <= SETTINGS_TAB_AUTOCLIP) {
        SwitchToTab(tab);
        
        /* Update RAM estimate when switching to Video tab (which contains Replay settings) */
        if (tab == SETTINGS_TAB_VIDEO) {
            UpdateReplayRAMEstimate(s_settingsWnd);
        }
    }
}

/* ============================================================================
 * AUTO-CLIP REGION DEBUG OVERLAY
 * ============================================================================
 * Draws outline rectangles over the kill feed and badge regions using
 * per-pixel alpha (UpdateLayeredWindow), same as border.c.
 * Click-through, no collision, just visual debug.
 * ============================================================================ */

#include "layered_window.h"

static HWND s_regionOverlayWnd = NULL;
static const char* REGION_OVERLAY_CLASS = "LWSRRegionOverlay";

/* Draw a 2px outline rectangle into a LayeredBitmap at the given coords */
static void DrawOutlineRect(LayeredBitmap* lb, int rx, int ry, int rw, int rh,
                            BYTE r, BYTE g, BYTE b, BYTE a)
{
    int bw = lb->width;
    int bh = lb->height;
    int thick = 2;
    
    /* Pre-multiplied alpha */
    BYTE pb = (BYTE)(b * a / 255);
    BYTE pg = (BYTE)(g * a / 255);
    BYTE pr = (BYTE)(r * a / 255);
    
    for (int y = ry; y < ry + rh && y < bh; y++) {
        if (y < 0) continue;
        for (int x = rx; x < rx + rw && x < bw; x++) {
            if (x < 0) continue;
            BOOL isBorder = (x < rx + thick || x >= rx + rw - thick ||
                             y < ry + thick || y >= ry + rh - thick);
            if (isBorder) {
                int idx = (y * bw + x) * 4;
                lb->pixels[idx + 0] = pb;
                lb->pixels[idx + 1] = pg;
                lb->pixels[idx + 2] = pr;
                lb->pixels[idx + 3] = a;
            }
        }
    }
}

/* Draw centered text label into a LayeredBitmap rect */
static void DrawLabelOnBitmap(LayeredBitmap* lb, int rx, int ry, int rw, int rh,
                              const char* text, BYTE r, BYTE g, BYTE b)
{
    HFONT font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT oldFont = SelectObject(lb->memDC, font);
    
    SetBkMode(lb->memDC, TRANSPARENT);
    SetTextColor(lb->memDC, RGB(r, g, b));
    
    RECT textRect = { rx + 4, ry + 4, rx + rw - 4, ry + rh - 4 };
    DrawTextA(lb->memDC, text, -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    
    /* Fix pre-multiplied alpha for text pixels — any pixel with color but 0 alpha
       was drawn by GDI which doesn't set alpha. Force alpha to 255. */
    for (int y = ry; y < ry + rh && y < lb->height; y++) {
        if (y < 0) continue;
        for (int x = rx; x < rx + rw && x < lb->width; x++) {
            if (x < 0) continue;
            int idx = (y * lb->width + x) * 4;
            if (lb->pixels[idx + 3] == 0 &&
                (lb->pixels[idx + 0] || lb->pixels[idx + 1] || lb->pixels[idx + 2])) {
                lb->pixels[idx + 3] = 255;
            }
        }
    }
    
    SelectObject(lb->memDC, oldFont);
    DeleteObject(font);
}

static void UpdateRegionOverlayBitmap(void)
{
    if (!s_regionOverlayWnd) return;
    
    int monW = GetSystemMetrics(SM_CXSCREEN);
    int monH = GetSystemMetrics(SM_CYSCREEN);
    
    LayeredBitmap lb = {0};
    if (!LayeredBitmap_Create(&lb, monW, monH)) return;
    
    /* Kill feed region - green outline */
    if (g_config.killfeedWPct > 0.0f) {
        int kx = (int)(g_config.killfeedXPct * monW);
        int ky = (int)(g_config.killfeedYPct * monH);
        int kw = (int)(g_config.killfeedWPct * monW);
        int kh = (int)(g_config.killfeedHPct * monH);
        DrawOutlineRect(&lb, kx, ky, kw, kh, 0, 255, 0, 200);
        DrawLabelOnBitmap(&lb, kx, ky, kw, kh, "KILL FEED", 0, 255, 0);
    }
    
    LayeredBitmap_Apply(&lb, s_regionOverlayWnd, 0, 0);
    LayeredBitmap_Destroy(&lb);
}

static LRESULT CALLBACK RegionOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void AutoClipRegionOverlay_Show(void)
{
    if (s_regionOverlayWnd) {
        UpdateRegionOverlayBitmap();
        ShowWindow(s_regionOverlayWnd, SW_SHOWNA);
        return;
    }
    
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RegionOverlayWndProc;
    wc.hInstance = s_hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = REGION_OVERLAY_CLASS;
    RegisterClassExA(&wc);
    
    int monW = GetSystemMetrics(SM_CXSCREEN);
    int monH = GetSystemMetrics(SM_CYSCREEN);
    
    s_regionOverlayWnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        REGION_OVERLAY_CLASS, NULL,
        WS_POPUP,
        0, 0, monW, monH,
        NULL, NULL, s_hInstance, NULL);
    
    if (!s_regionOverlayWnd) {
        Logger_Log("settings_dialog: CreateWindowExA(%s) failed err=%lu\n",
                   REGION_OVERLAY_CLASS, (unsigned long)GetLastError());
        return;
    }
    
    UpdateRegionOverlayBitmap();
    ShowWindow(s_regionOverlayWnd, SW_SHOWNA);
}

static void AutoClipRegionOverlay_Hide(void)
{
    if (s_regionOverlayWnd) {
        DestroyWindow(s_regionOverlayWnd);
        s_regionOverlayWnd = NULL;
    }
}

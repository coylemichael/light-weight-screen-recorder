/*
 * settings_dialog.h - Settings window UI and control handling
 */

#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include <windows.h>

/* Settings window dimensions */
#define SETTINGS_WIDTH  620
#define SETTINGS_HEIGHT 320

/* Tab indices for settings sections */
typedef enum {
    SETTINGS_TAB_GENERAL = 0,   /* Capture options, time limit, save path, debug */
    SETTINGS_TAB_AUDIO   = 1,   /* Enable, 3 sources with volume sliders */
    SETTINGS_TAB_VIDEO   = 2    /* Format, Quality, FPS, Replay buffer settings */
} SettingsTab;

/* Settings window control IDs - Format/Quality */
#define ID_CMB_FORMAT      1009
#define ID_CMB_QUALITY     1010

/* Settings window control IDs - Capture options */
#define ID_CHK_MOUSE       1007
#define ID_CHK_BORDER      1008

/* Settings window control IDs - Time limit */
#define ID_CMB_HOURS       1016
#define ID_CMB_MINUTES     1017
#define ID_CMB_SECONDS     1018

/* Settings window control IDs - Save path */
#define ID_EDT_PATH        1011
#define ID_BTN_BROWSE      1012

/* Replay buffer settings control IDs */
#define ID_CHK_REPLAY_ENABLED   4001
#define ID_CMB_REPLAY_SOURCE    4002
#define ID_CMB_REPLAY_ASPECT    4003
#define ID_CMB_REPLAY_STORAGE   4004
#define ID_STATIC_REPLAY_INFO   4005
#define ID_BTN_REPLAY_HOTKEY    4006
#define ID_CMB_REPLAY_HOURS     4007
#define ID_CMB_REPLAY_MINS      4008
#define ID_CMB_REPLAY_SECS      4009
#define ID_CMB_REPLAY_FPS       4010
#define ID_STATIC_REPLAY_RAM    4011
#define ID_STATIC_REPLAY_CALC   4012

/* Audio capture settings control IDs */
#define ID_CHK_AUDIO_ENABLED    5001
#define ID_CMB_AUDIO_SOURCE1    5002
#define ID_CMB_AUDIO_SOURCE2    5003
#define ID_CMB_AUDIO_SOURCE3    5004
#define ID_SLD_AUDIO_VOLUME1    5005
#define ID_SLD_AUDIO_VOLUME2    5006
#define ID_SLD_AUDIO_VOLUME3    5007
#define ID_LBL_AUDIO_VOL1       5008
#define ID_LBL_AUDIO_VOL2       5009
#define ID_LBL_AUDIO_VOL3       5010

/* Debug settings control IDs */
#define ID_CHK_DEBUG_LOGGING    6001

/**
 * Register the settings window class.
 * Must be called once during application initialization.
 * Returns TRUE on success.
 */
BOOL SettingsDialog_Register(HINSTANCE hInstance);

/**
 * Show the settings dialog window at a specific position.
 * Creates the window if it doesn't exist, or brings it to front if it does.
 * Returns the settings window handle.
 */
HWND SettingsDialog_ShowAt(HINSTANCE hInstance, int x, int y);

/**
 * Close the settings dialog window if open.
 */
void SettingsDialog_Close(void);

/**
 * Update the settingsWnd reference in the caller.
 * This is for backward compatibility with code that tracks the window handle.
 */
void SettingsDialog_SetExternalHandle(HWND* externalRef);

/**
 * Get the settings window handle (NULL if not open).
 */
HWND SettingsDialog_GetHandle(void);

/**
 * Check if settings dialog is currently visible.
 */
BOOL SettingsDialog_IsVisible(void);

/**
 * Get the currently active settings tab.
 */
SettingsTab SettingsDialog_GetCurrentTab(void);

/**
 * Switch to a specific settings tab.
 * Called from overlay.c when mode buttons are clicked while settings is open.
 */
void SettingsDialog_SwitchTab(SettingsTab tab);

#endif /* SETTINGS_DIALOG_H */

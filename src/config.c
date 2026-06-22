/*
 * config.c - INI file read/write for settings
 */

#include "config.h"
#include "constants.h"
#include <shlobj.h>
#include <stdio.h>

/* Read-only lookup. Indexed by OutputFormat; FORMAT_COUNT entries. */
static const char* const FORMAT_EXTENSIONS[FORMAT_COUNT] = { ".mp4" };

static void Config_GetPath(char* buffer, size_t size) {
    // Preconditions
    LWSR_ASSERT(buffer != NULL);
    LWSR_ASSERT(size > 0);
    
    if (!buffer || size == 0) return;
    
    // Store config next to executable
    DWORD len = GetModuleFileNameA(NULL, buffer, (DWORD)size);
    if (len == 0 || len >= size) {
        // Fallback to current directory on failure or truncation
        strncpy(buffer, "lwsr_config.ini", size - 1);
        buffer[size - 1] = '\0';
        return;
    }
    
    char* lastSlash = strrchr(buffer, '\\');
    if (lastSlash) {
        size_t remaining = size - (size_t)(lastSlash + 1 - buffer);
        if (remaining > sizeof("lwsr_config.ini")) {
            strncpy(lastSlash + 1, "lwsr_config.ini", remaining - 1);
            buffer[size - 1] = '\0';
        } else {
            // Not enough space, use fallback
            strncpy(buffer, "lwsr_config.ini", size - 1);
            buffer[size - 1] = '\0';
        }
    } else {
        // No backslash found, use filename only
        strncpy(buffer, "lwsr_config.ini", size - 1);
        buffer[size - 1] = '\0';
    }
}

void Config_Load(AppConfig* config) {
    // Precondition
    LWSR_ASSERT(config != NULL);
    
    if (!config) return;
    
    char configPath[MAX_PATH];
    Config_GetPath(configPath, MAX_PATH);
    
    // Set defaults - LOSSLESS quality for crystal-clear screen recordings
    config->outputFormat = FORMAT_MP4;
    config->quality = QUALITY_LOSSLESS;
    config->captureMouse = TRUE;
    config->showRecordingBorder = TRUE;
    config->maxRecordingSeconds = 0;
    config->cancelKey = VK_ESCAPE;  // Default cancel key
    
    // Replay buffer defaults
    config->replayEnabled = FALSE;
    config->replayDuration = REPLAY_DURATION_DEFAULT;  // 1 minute default
    config->replayCaptureSource = MODE_MONITOR;
    config->replayMonitorIndex = 0;  // Primary monitor
    config->replaySaveKey = VK_F9;  // F9 to save replay
    
    // Default replay area (will be centered on screen when first used)
    config->replayAreaRect.left = 0;
    config->replayAreaRect.top = 0;
    config->replayAreaRect.right = 0;
    config->replayAreaRect.bottom = 0;
    config->replayAspectRatio = 0;  // Native (no aspect ratio cropping)
    config->replayFPS = DEFAULT_FPS;  // 60 FPS default
    
    // Audio defaults (disabled, no sources selected)
    config->audioEnabled = FALSE;
    config->audioSource1[0] = '\0';
    config->audioSource2[0] = '\0';
    config->audioSource3[0] = '\0';
    config->audioVolume1 = AUDIO_VOLUME_DEFAULT;
    config->audioVolume2 = AUDIO_VOLUME_DEFAULT;
    config->audioVolume3 = AUDIO_VOLUME_DEFAULT;
    
    // Marker settings
    config->markerKey = VK_F6;  // F6 to drop a marker

    // Debug logging (disabled by default)
    config->debugLogging = FALSE;
    
    // Auto-clip defaults (disabled, no player name, no regions)
    config->autoClipEnabled = FALSE;
    config->autoClipShowRegions = FALSE;
    config->autoClipCooldownSec = 10;
    config->autoClipDelaySec = 10;
    
    // Default save path to Videos folder.
    // Note: SHGetFolderPathA is deprecated since Vista in favor of SHGetKnownFolderPath
    // (FOLDERID_Videos). Kept for ANSI simplicity; migrate if Unicode paths become needed.
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, config->savePath))) {
        strncat(config->savePath, "\\Recordings", sizeof(config->savePath) - strlen(config->savePath) - 1);
        config->savePath[sizeof(config->savePath) - 1] = '\0';
    } else {
        strncpy(config->savePath, FALLBACK_RECORDINGS_PATH, sizeof(config->savePath) - 1);
        config->savePath[sizeof(config->savePath) - 1] = '\0';
    }
    
    SetRectEmpty(&config->lastCaptureRect);
    config->lastMode = MODE_AREA;

    // Advanced (INI-only). Default CFR keeps editor/player compatibility; flip to VFR
    // only if you understand the tradeoff (see [Advanced] FrameTiming in lwsr_config.ini).
    config->frameTimingMode = FRAME_TIMING_CFR;

    // Load from INI if exists
    if (GetFileAttributesA(configPath) != INVALID_FILE_ATTRIBUTES) {
        // Note: Format INI key intentionally not read/written. Only FORMAT_MP4 is
        // implemented; outputFormat stays at its default.
        config->quality = (QualityPreset)GetPrivateProfileIntA(
            "Recording", "Quality", QUALITY_LOSSLESS, configPath);
        config->captureMouse = GetPrivateProfileIntA(
            "Recording", "CaptureMouse", TRUE, configPath);
        config->showRecordingBorder = GetPrivateProfileIntA(
            "Recording", "ShowBorder", TRUE, configPath);
        config->maxRecordingSeconds = GetPrivateProfileIntA(
            "Recording", "MaxSeconds", 0, configPath);
        config->cancelKey = GetPrivateProfileIntA(
            "UI", "CancelKey", VK_ESCAPE, configPath);
        
        // Replay buffer settings
        config->replayEnabled = GetPrivateProfileIntA(
            "ReplayBuffer", "Enabled", FALSE, configPath);
        config->replayDuration = GetPrivateProfileIntA(
            "ReplayBuffer", "Duration", REPLAY_DURATION_DEFAULT, configPath);
        config->replayCaptureSource = (CaptureMode)GetPrivateProfileIntA(
            "ReplayBuffer", "CaptureSource", MODE_MONITOR, configPath);
        config->replayMonitorIndex = GetPrivateProfileIntA(
            "ReplayBuffer", "MonitorIndex", 0, configPath);
        config->replaySaveKey = GetPrivateProfileIntA(
            "ReplayBuffer", "SaveKey", VK_F9, configPath);
        config->replayAreaRect.left = GetPrivateProfileIntA(
            "ReplayBuffer", "AreaLeft", 200, configPath);
        config->replayAreaRect.top = GetPrivateProfileIntA(
            "ReplayBuffer", "AreaTop", 200, configPath);
        config->replayAreaRect.right = GetPrivateProfileIntA(
            "ReplayBuffer", "AreaRight", 1000, configPath);
        config->replayAreaRect.bottom = GetPrivateProfileIntA(
            "ReplayBuffer", "AreaBottom", 800, configPath);
        config->replayAspectRatio = GetPrivateProfileIntA(
            "ReplayBuffer", "AspectRatio", 0, configPath);
        config->replayFPS = GetPrivateProfileIntA(
            "ReplayBuffer", "FPS", 60, configPath);
        
        // Audio settings
        config->audioEnabled = GetPrivateProfileIntA(
            "Audio", "Enabled", 0, configPath);
        GetPrivateProfileStringA("Audio", "Source1", "",
            config->audioSource1, sizeof(config->audioSource1), configPath);
        GetPrivateProfileStringA("Audio", "Source2", "",
            config->audioSource2, sizeof(config->audioSource2), configPath);
        GetPrivateProfileStringA("Audio", "Source3", "",
            config->audioSource3, sizeof(config->audioSource3), configPath);
        config->audioVolume1 = GetPrivateProfileIntA("Audio", "Volume1", 100, configPath);
        config->audioVolume2 = GetPrivateProfileIntA("Audio", "Volume2", 100, configPath);
        config->audioVolume3 = GetPrivateProfileIntA("Audio", "Volume3", 100, configPath);
        
        // Marker hotkey
        config->markerKey = GetPrivateProfileIntA(
            "Markers", "Key", VK_F6, configPath);

        // Debug logging
        config->debugLogging = GetPrivateProfileIntA("Debug", "Logging", 0, configPath);
        
        // Auto-clip settings (global; per-game region/cooldown live in
        // [AutoClip.<id>] sections, owned by game_profile.c)
        config->autoClipEnabled = GetPrivateProfileIntA(
            "AutoClip", "Enabled", FALSE, configPath);
        config->autoClipCooldownSec = GetPrivateProfileIntA(
            "AutoClip", "CooldownSec", 10, configPath);
        config->autoClipDelaySec = GetPrivateProfileIntA(
            "AutoClip", "DelaySec", 10, configPath);
        
        GetPrivateProfileStringA("Recording", "SavePath", config->savePath,
            config->savePath, MAX_PATH, configPath);
        
        config->lastCaptureRect.left = GetPrivateProfileIntA(
            "LastCapture", "Left", 0, configPath);
        config->lastCaptureRect.top = GetPrivateProfileIntA(
            "LastCapture", "Top", 0, configPath);
        config->lastCaptureRect.right = GetPrivateProfileIntA(
            "LastCapture", "Right", 0, configPath);
        config->lastCaptureRect.bottom = GetPrivateProfileIntA(
            "LastCapture", "Bottom", 0, configPath);
        config->lastMode = (CaptureMode)GetPrivateProfileIntA(
            "LastCapture", "Mode", MODE_AREA, configPath);

        // Advanced section (no UI). Accept "cfr"/"vfr" (case-insensitive); anything else
        // falls back to the default. Numeric 0/1 also accepted for back-compat with
        // hand-edited INIs.
        char frameTimingStr[16] = "";
        GetPrivateProfileStringA("Advanced", "FrameTiming", "cfr",
            frameTimingStr, sizeof(frameTimingStr), configPath);
        if (_stricmp(frameTimingStr, "vfr") == 0 || strcmp(frameTimingStr, "1") == 0) {
            config->frameTimingMode = FRAME_TIMING_VFR;
        } else {
            config->frameTimingMode = FRAME_TIMING_CFR;
        }

        // Validate/clamp loaded values to prevent corrupted INI from causing issues.
        // Defend at point of use: INI is an untrusted boundary (user-editable).
        if (config->outputFormat < 0 || config->outputFormat >= FORMAT_COUNT)
            config->outputFormat = FORMAT_MP4;
        if (config->quality < QUALITY_LOW || config->quality > QUALITY_LOSSLESS)
            config->quality = QUALITY_HIGH;
        if (config->replayDuration < REPLAY_DURATION_MIN_SECS)
            config->replayDuration = REPLAY_DURATION_MIN_SECS;
        if (config->replayDuration > REPLAY_DURATION_MAX_SECS)
            config->replayDuration = REPLAY_DURATION_MAX_SECS;
        // Supported FPS set: 30/60/120/240 (matches settings dialog combo; see
        // Util_GetAspectRatioDimensions / replay_buffer for downstream assumptions).
        if (config->replayFPS != 30 && config->replayFPS != 60 &&
            config->replayFPS != 120 && config->replayFPS != 240)
            config->replayFPS = DEFAULT_FPS;
        if (config->audioVolume1 < 0) config->audioVolume1 = 0;
        if (config->audioVolume1 > AUDIO_VOLUME_MAX) config->audioVolume1 = AUDIO_VOLUME_MAX;
        if (config->audioVolume2 < 0) config->audioVolume2 = 0;
        if (config->audioVolume2 > AUDIO_VOLUME_MAX) config->audioVolume2 = AUDIO_VOLUME_MAX;
        if (config->audioVolume3 < 0) config->audioVolume3 = 0;
        if (config->audioVolume3 > AUDIO_VOLUME_MAX) config->audioVolume3 = AUDIO_VOLUME_MAX;
        if (config->autoClipCooldownSec < AUTOCLIP_COOLDOWN_MIN_SEC)
            config->autoClipCooldownSec = AUTOCLIP_COOLDOWN_MIN_SEC;
        if (config->autoClipCooldownSec > AUTOCLIP_COOLDOWN_MAX_SEC)
            config->autoClipCooldownSec = AUTOCLIP_COOLDOWN_MAX_SEC;
        if (config->autoClipDelaySec < AUTOCLIP_DELAY_MIN_SEC)
            config->autoClipDelaySec = AUTOCLIP_DELAY_MIN_SEC;
        if (config->autoClipDelaySec > AUTOCLIP_DELAY_MAX_SEC)
            config->autoClipDelaySec = AUTOCLIP_DELAY_MAX_SEC;

        // Hotkey virtual-key codes: valid VK range is 0x01..0xFE (0 = none/disabled).
        // Out-of-range values would silently fail RegisterHotKey later; reset to defaults.
        if (config->cancelKey < 0 || config->cancelKey > 0xFE)
            config->cancelKey = VK_ESCAPE;
        if (config->replaySaveKey < 0 || config->replaySaveKey > 0xFE)
            config->replaySaveKey = VK_F9;
        if (config->markerKey < 0 || config->markerKey > 0xFE)
            config->markerKey = VK_F6;

        if (config->replayMonitorIndex < 0)
            config->replayMonitorIndex = 0;

        // Aspect ratio: 0=Native, 1..8 are defined in Util_GetAspectRatioDimensions.
        if (config->replayAspectRatio < 0 || config->replayAspectRatio > 8)
            config->replayAspectRatio = 0;

        // CaptureMode enum bounds (MODE_NONE..MODE_MONITOR).
        if ((int)config->replayCaptureSource < MODE_NONE ||
            (int)config->replayCaptureSource > MODE_MONITOR)
            config->replayCaptureSource = MODE_MONITOR;
        if ((int)config->lastMode < MODE_NONE || (int)config->lastMode > MODE_MONITOR)
            config->lastMode = MODE_AREA;

        // Degenerate rect (right<=left or bottom<=top) is unusable downstream;
        // reset to the default centered area.
        if (config->replayAreaRect.right <= config->replayAreaRect.left ||
            config->replayAreaRect.bottom <= config->replayAreaRect.top) {
            config->replayAreaRect.left = 200;
            config->replayAreaRect.top = 200;
            config->replayAreaRect.right = 1000;
            config->replayAreaRect.bottom = 800;
        }

        if (config->maxRecordingSeconds < 0)
            config->maxRecordingSeconds = 0;
    }
    
    // Ensure save directory exists (ignore ERROR_ALREADY_EXISTS)
    if (!CreateDirectoryA(config->savePath, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS && err != ERROR_SUCCESS) {
            // Directory creation failed - path may be invalid
            // Fall back to current directory
            strncpy(config->savePath, ".", sizeof(config->savePath) - 1);
        }
    }
}

void Config_Save(const AppConfig* config) {
    // Precondition
    LWSR_ASSERT(config != NULL);
    
    if (!config) return;
    
    char configPath[MAX_PATH];
    Config_GetPath(configPath, MAX_PATH);
    
    char buffer[32];

    // Note: Format INI key intentionally not written. Only FORMAT_MP4 is implemented.
    snprintf(buffer, sizeof(buffer), "%d", config->quality);
    WritePrivateProfileStringA("Recording", "Quality", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->captureMouse);
    WritePrivateProfileStringA("Recording", "CaptureMouse", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->showRecordingBorder);
    WritePrivateProfileStringA("Recording", "ShowBorder", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->maxRecordingSeconds);
    WritePrivateProfileStringA("Recording", "MaxSeconds", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->cancelKey);
    WritePrivateProfileStringA("UI", "CancelKey", buffer, configPath);
    
    // Replay buffer settings
    snprintf(buffer, sizeof(buffer), "%d", config->replayEnabled);
    WritePrivateProfileStringA("ReplayBuffer", "Enabled", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->replayDuration);
    WritePrivateProfileStringA("ReplayBuffer", "Duration", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->replayCaptureSource);
    WritePrivateProfileStringA("ReplayBuffer", "CaptureSource", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->replayMonitorIndex);
    WritePrivateProfileStringA("ReplayBuffer", "MonitorIndex", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->replaySaveKey);
    WritePrivateProfileStringA("ReplayBuffer", "SaveKey", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%ld", config->replayAreaRect.left);
    WritePrivateProfileStringA("ReplayBuffer", "AreaLeft", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%ld", config->replayAreaRect.top);
    WritePrivateProfileStringA("ReplayBuffer", "AreaTop", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%ld", config->replayAreaRect.right);
    WritePrivateProfileStringA("ReplayBuffer", "AreaRight", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%ld", config->replayAreaRect.bottom);
    WritePrivateProfileStringA("ReplayBuffer", "AreaBottom", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->replayAspectRatio);
    WritePrivateProfileStringA("ReplayBuffer", "AspectRatio", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->replayFPS);
    WritePrivateProfileStringA("ReplayBuffer", "FPS", buffer, configPath);
    
    // Audio settings
    snprintf(buffer, sizeof(buffer), "%d", config->audioEnabled);
    WritePrivateProfileStringA("Audio", "Enabled", buffer, configPath);
    WritePrivateProfileStringA("Audio", "Source1", config->audioSource1, configPath);
    WritePrivateProfileStringA("Audio", "Source2", config->audioSource2, configPath);
    WritePrivateProfileStringA("Audio", "Source3", config->audioSource3, configPath);
    snprintf(buffer, sizeof(buffer), "%d", config->audioVolume1);
    WritePrivateProfileStringA("Audio", "Volume1", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%d", config->audioVolume2);
    WritePrivateProfileStringA("Audio", "Volume2", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%d", config->audioVolume3);
    WritePrivateProfileStringA("Audio", "Volume3", buffer, configPath);
    
    // Marker settings
    snprintf(buffer, sizeof(buffer), "%d", config->markerKey);
    WritePrivateProfileStringA("Markers", "Key", buffer, configPath);

    // Debug logging
    snprintf(buffer, sizeof(buffer), "%d", config->debugLogging);
    WritePrivateProfileStringA("Debug", "Logging", buffer, configPath);
    
    // Auto-clip settings (global; per-game region/cooldown is persisted by
    // game_profile.c into [AutoClip.<id>] sections)
    snprintf(buffer, sizeof(buffer), "%d", config->autoClipEnabled);
    WritePrivateProfileStringA("AutoClip", "Enabled", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%d", config->autoClipCooldownSec);
    WritePrivateProfileStringA("AutoClip", "CooldownSec", buffer, configPath);
    snprintf(buffer, sizeof(buffer), "%d", config->autoClipDelaySec);
    WritePrivateProfileStringA("AutoClip", "DelaySec", buffer, configPath);
    
    WritePrivateProfileStringA("Recording", "SavePath", config->savePath, configPath);
    
    snprintf(buffer, sizeof(buffer), "%ld", config->lastCaptureRect.left);
    WritePrivateProfileStringA("LastCapture", "Left", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%ld", config->lastCaptureRect.top);
    WritePrivateProfileStringA("LastCapture", "Top", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%ld", config->lastCaptureRect.right);
    WritePrivateProfileStringA("LastCapture", "Right", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%ld", config->lastCaptureRect.bottom);
    WritePrivateProfileStringA("LastCapture", "Bottom", buffer, configPath);
    
    snprintf(buffer, sizeof(buffer), "%d", config->lastMode);
    WritePrivateProfileStringA("LastCapture", "Mode", buffer, configPath);

    // Advanced (INI-only, no UI). Round-trip as "cfr"/"vfr" so the file stays
    // self-documenting for power users who open it in a text editor.
    WritePrivateProfileStringA("Advanced", "FrameTiming",
        config->frameTimingMode == FRAME_TIMING_VFR ? "vfr" : "cfr", configPath);
}

const char* Config_GetFormatExtension(OutputFormat format) {
    if (format >= 0 && format < FORMAT_COUNT) {
        return FORMAT_EXTENSIONS[format];
    }
    return ".mp4";
}

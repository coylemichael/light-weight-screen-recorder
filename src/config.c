/*
 * Configuration Implementation
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation checks
 * - Uses sensible defaults when config file missing/corrupt
 * - File I/O errors are silent (defaults used instead)
 * - No HRESULT usage - pure Win32 file/registry APIs
 */

#include "config.h"
#include "constants.h"
#include <shlobj.h>
#include <stdio.h>

static const char* FORMAT_EXTENSIONS[] = { ".mp4", ".avi", ".wmv" };
static const char* FORMAT_NAMES[] = { "MP4 (H.264)", "AVI", "WMV" };

void Config_GetPath(char* buffer, size_t size) {
    // Store config next to executable
    GetModuleFileNameA(NULL, buffer, (DWORD)size);
    char* lastSlash = strrchr(buffer, '\\');
    if (lastSlash) {
        size_t remaining = size - (lastSlash + 1 - buffer);
        strncpy(lastSlash + 1, "lwsr_config.ini", remaining - 1);
        buffer[size - 1] = '\0';
    }
}

void Config_Load(AppConfig* config) {
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
    
    // Debug logging (disabled by default)
    config->debugLogging = FALSE;
    
    // Default save path to Videos folder
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, config->savePath))) {
        strncat(config->savePath, "\\Recordings", sizeof(config->savePath) - strlen(config->savePath) - 1);
        config->savePath[sizeof(config->savePath) - 1] = '\0';
    } else {
        strncpy(config->savePath, FALLBACK_RECORDINGS_PATH, sizeof(config->savePath) - 1);
        config->savePath[sizeof(config->savePath) - 1] = '\0';
    }
    
    SetRectEmpty(&config->lastCaptureRect);
    config->lastMode = MODE_AREA;
    
    // Load from INI if exists
    if (GetFileAttributesA(configPath) != INVALID_FILE_ATTRIBUTES) {
        config->outputFormat = (OutputFormat)GetPrivateProfileIntA(
            "Recording", "Format", FORMAT_MP4, configPath);
        config->quality = (QualityPreset)GetPrivateProfileIntA(
            "Recording", "Quality", QUALITY_HIGH, configPath);
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
            "ReplayBuffer", "Duration", 60, configPath);
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
        
        // Debug logging
        config->debugLogging = GetPrivateProfileIntA("Debug", "Logging", 0, configPath);
        
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
    }
    
    // Ensure save directory exists
    CreateDirectoryA(config->savePath, NULL);
}

void Config_Save(const AppConfig* config) {
    char configPath[MAX_PATH];
    Config_GetPath(configPath, MAX_PATH);
    
    char buffer[32];
    
    snprintf(buffer, sizeof(buffer), "%d", config->outputFormat);
    WritePrivateProfileStringA("Recording", "Format", buffer, configPath);
    
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
    
    // Debug logging
    snprintf(buffer, sizeof(buffer), "%d", config->debugLogging);
    WritePrivateProfileStringA("Debug", "Logging", buffer, configPath);
    
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
}

const char* Config_GetFormatExtension(OutputFormat format) {
    if (format >= 0 && format < FORMAT_COUNT) {
        return FORMAT_EXTENSIONS[format];
    }
    return ".mp4";
}

const char* Config_GetFormatName(OutputFormat format) {
    if (format >= 0 && format < FORMAT_COUNT) {
        return FORMAT_NAMES[format];
    }
    return "MP4";
}

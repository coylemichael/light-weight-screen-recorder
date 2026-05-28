/*
 * config.h - INI file read/write for settings
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

// Output formats (only MP4/H.264 is implemented; enum kept for future expansion)
typedef enum {
    FORMAT_MP4 = 0,    // H.264 in MP4 container
    FORMAT_COUNT
} OutputFormat;

// Capture modes
typedef enum {
    MODE_NONE = 0,    // No mode selected (initial state)
    MODE_AREA,
    MODE_WINDOW,
    MODE_MONITOR
} CaptureMode;

// Quality presets
typedef enum {
    QUALITY_LOW = 0,
    QUALITY_MEDIUM,
    QUALITY_HIGH,
    QUALITY_LOSSLESS
} QualityPreset;

typedef struct {
    // Recording settings
    OutputFormat outputFormat;
    QualityPreset quality;
    BOOL captureMouse;
    BOOL showRecordingBorder;
    int maxRecordingSeconds;  // 0 = unlimited
    
    // UI settings
    int cancelKey;  // Virtual key code to close overlay (default: VK_ESCAPE)
    
    // Replay buffer settings (instant replay)
    BOOL replayEnabled;              // Enable replay buffer
    int replayDuration;              // Buffer duration in seconds (REPLAY_DURATION_MIN_SECS..REPLAY_DURATION_MAX_SECS)
    CaptureMode replayCaptureSource; // What to capture for replay
    int replayMonitorIndex;          // Which monitor (if MODE_MONITOR)
    int replaySaveKey;               // Hotkey to save replay (default: F9)
    RECT replayAreaRect;             // Custom area for replay (if MODE_AREA)
    int replayAspectRatio;           // See Util_GetAspectRatioDimensions: 0=Native, 1=16:9, 6=4:3, 7=21:9, etc.
    int replayFPS;                   // 30, 60, 120, or 240
    
    // Audio capture settings
    BOOL audioEnabled;               // Enable audio capture
    char audioSource1[128];          // Device ID for audio source 1 (empty = disabled)
    char audioSource2[128];          // Device ID for audio source 2 (empty = disabled)
    char audioSource3[128];          // Device ID for audio source 3 (empty = disabled)
    int audioVolume1;                // Volume 0-400 for source 1 (100=normal, 400=4x boost)
    int audioVolume2;                // Volume 0-400 for source 2 (100=normal, 400=4x boost)
    int audioVolume3;                // Volume 0-400 for source 3 (100=normal, 400=4x boost)
    
    // Save location
    char savePath[MAX_PATH];
    
    // Marker settings
    int markerKey;                   // Hotkey to drop a marker (default: VK_F6)

    // Debug/logging settings
    BOOL debugLogging;               // Enable debug logging to file (includes leak tracking)
    
    // Auto-clip settings (kill feed detection)
    BOOL autoClipEnabled;            // Enable kill feed instant clipping
    BOOL autoClipShowRegions;        // Draw detection region overlay (debug/calibration)
    int autoClipCooldownSec;         // Minimum seconds between auto-clip saves (5-30)
    int autoClipDelaySec;            // Seconds to wait after kill before saving (0-30)
    float killfeedXPct;              // Kill feed region (screen percentages 0.0-1.0)
    float killfeedYPct;
    float killfeedWPct;
    float killfeedHPct;
    
    // Last capture area (for quick re-record)
    RECT lastCaptureRect;
    CaptureMode lastMode;
    
} AppConfig;

// Load config from INI file.
// Threading: UI thread only (called at startup before worker threads exist).
void Config_Load(AppConfig* config);

// Save config to INI file.
// Threading: UI thread only. All call sites (main.c WM_CLOSE, overlay.c, settings_dialog.c)
// are window procedures on the UI thread. Not reentrant-safe; do not call from worker
// threads or share g_config with mutating threads without external serialization.
void Config_Save(const AppConfig* config);

// Get format extension for the given output format. Returns ".mp4" for unknown values.
const char* Config_GetFormatExtension(OutputFormat format);

#endif // CONFIG_H

/*
 * Ultra Lightweight Screen Recorder
 * Pure Win32 + DXGI Desktop Duplication + Media Foundation
 * Zero external dependencies, maximum performance
 *
 * ============================================================================
 * PROJECT-WIDE ERROR HANDLING STANDARDS
 * ============================================================================
 *
 * This codebase follows these consistent error handling patterns:
 *
 * 1. HRESULT CHECKING:
 *    - ALWAYS use FAILED(hr) or SUCCEEDED(hr) macros
 *    - NEVER compare directly: hr != S_OK, hr == E_FAIL, etc.
 *    - Log the HRESULT value in hex: Logger_Log("...failed: 0x%08X\n", hr);
 *
 * 2. ERROR LOGGING:
 *    - All errors MUST be logged with descriptive context
 *    - Include function name and parameter values when helpful
 *    - Use rate limiting for high-frequency error paths (LOG_RATE_LIMIT)
 *
 * 3. PATTERN SELECTION:
 *    - Goto-cleanup: Functions allocating multiple COM/heap resources
 *    - Early return: Simple validation at function start
 *    - Continue-on-error: Loops processing multiple items (best effort)
 *
 * 4. ERROR PROPAGATION:
 *    - Functions return BOOL (success/fail) or NULL (pointer-returning)
 *    - Callers MUST check return values
 *    - Fatal errors may return special codes (e.g., -1 for device loss)
 *
 * 5. THREAD SAFETY:
 *    - Use InterlockedExchange/InterlockedCompareExchange for flags
 *    - Use CRITICAL_SECTION for shared data structures
 *    - Document thread-safety expectations in comments
 *
 * Each source file documents its specific pattern in the file header comment.
 * See mem_utils.h for SAFE_FREE/SAFE_RELEASE macros and goto-cleanup examples.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

// Forward declarations
#include "config.h"
#include "capture.h"
#include "encoder.h"
#include "overlay.h"
#include "replay_buffer.h"
#include "logger.h"
#include "crash_handler.h"
#include "gdiplus_api.h"
#include "constants.h"

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================
 * Thread access patterns documented for each variable.
 * See app_context.h for architectural overview and migration path.
 */

/*
 * Application configuration loaded from INI file.
 * Thread Access: [Main writes during init/settings, Any thread reads]
 * Lifetime: Program duration
 */
AppConfig g_config;

/*
 * DXGI desktop duplication capture state.
 * Thread Access: [Main thread and Recording thread]
 * Lifetime: Program duration (init at start, shutdown at exit)
 */
CaptureState g_capture;

/*
 * Instant replay circular buffer state.
 * Thread Access: [Main thread for control, Buffer thread for capture]
 * Synchronization: Uses events (hReadyEvent, hSaveRequestEvent, etc.)
 * Lifetime: Program duration
 */
ReplayBufferState g_replayBuffer;

/*
 * Recording active flag.
 * Thread Access: [Any thread - atomic operations only]
 * Use InterlockedExchange/InterlockedCompareExchange to modify.
 */
volatile LONG g_isRecording = FALSE;

/*
 * Area selection mode flag.
 * Thread Access: [Any thread - atomic operations only]
 * Use InterlockedExchange/InterlockedCompareExchange to modify.
 */
volatile LONG g_isSelecting = FALSE;

/*
 * Main overlay window handle.
 * Thread Access: [Main thread only]
 * Lifetime: Created in Overlay_Create, destroyed at shutdown
 */
HWND g_overlayWnd = NULL;

/*
 * Control panel window handle.
 * Thread Access: [Main thread only]
 * Lifetime: Created in Overlay_Create, destroyed at shutdown
 */
HWND g_controlWnd = NULL;

/* Hotkey ID for replay save */
#define HOTKEY_REPLAY_SAVE 1

/*
 * Debug mode flag (enabled via --debug CLI argument).
 * Thread Access: [ReadOnly after ParseCommandLine]
 */
static BOOL g_debugMode = FALSE;

/*
 * Single instance mutex handle.
 * Thread Access: [Main thread only]
 * Lifetime: Created at startup, closed at shutdown
 */
HANDLE g_mutex = NULL;

/*
 * String constants - read-only after compile time.
 */
static const char* const MUTEX_NAME = "LightweightScreenRecorderMutex";
static const char* const WINDOW_CLASS = "LWSROverlay";

// Parse command line for --debug flag
static void ParseCommandLine(LPSTR lpCmdLine) {
    if (lpCmdLine && (strstr(lpCmdLine, "--debug") || strstr(lpCmdLine, "-d"))) {
        g_debugMode = TRUE;
    }
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, 
                   _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    (void)hPrevInstance;
    (void)nCmdShow;
    
    // Initialize crash handler first
    CrashHandler_Init();
    
    // Parse command line arguments
    ParseCommandLine(lpCmdLine);
    
    // Check for existing instance - toggle recording if running
    g_mutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);
    if (g_mutex) {
        // Another instance exists - signal it to stop recording
        HWND existingWnd = FindWindowA(WINDOW_CLASS, NULL);
        if (existingWnd) {
            PostMessage(existingWnd, WM_LWSR_STOP, 0, 0);
        }
        CloseHandle(g_mutex);
        return 0;
    }
    
    // Create mutex for this instance
    g_mutex = CreateMutexA(NULL, TRUE, MUTEX_NAME);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance won the race between OpenMutex and CreateMutex
        if (g_mutex) CloseHandle(g_mutex);
        return 0;
    }
    
    // Initialize COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "Failed to initialize COM", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Set AppUserModelID for taskbar pinning
    SetCurrentProcessExplicitAppUserModelID(L"CarnmorCyber.LightWeightScreenRecorder");
    
    // Initialize Media Foundation
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "Failed to initialize Media Foundation", "Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    
    // Load configuration
    Config_Load(&g_config);
    
    // Initialize shared GDI+ (used by overlay and action_toolbar)
    if (!GdiplusAPI_Init(&g_gdip)) {
        MessageBoxA(NULL, "Failed to initialize GDI+", "Error", MB_OK | MB_ICONERROR);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    
    // Initialize capture system
    if (!Capture_Init(&g_capture)) {
        MessageBoxA(NULL, "Failed to initialize screen capture", "Error", MB_OK | MB_ICONERROR);
        GdiplusAPI_Shutdown(&g_gdip);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    
    // Create and show overlay
    if (!Overlay_Create(hInstance)) {
        MessageBoxA(NULL, "Failed to create overlay", "Error", MB_OK | MB_ICONERROR);
        Capture_Shutdown(&g_capture);
        GdiplusAPI_Shutdown(&g_gdip);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    
    // Initialize replay buffer
    ReplayBuffer_Init(&g_replayBuffer);
    
    // Initialize logger only if debug logging is enabled in config
    if (g_config.debugLogging) {
        char exePath[MAX_PATH];
        char debugFolder[MAX_PATH];
        char logFilename[MAX_PATH];
        
        // Get exe directory and create Debug subfolder
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            snprintf(debugFolder, sizeof(debugFolder), "%s\\Debug", exePath);
            CreateDirectoryA(debugFolder, NULL);  // Create Debug folder if it doesn't exist
            
            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(logFilename, sizeof(logFilename), "%s\\lwsr_log_%04d%02d%02d_%02d%02d%02d.txt",
                    debugFolder, (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
            Logger_Init(logFilename, "w");
        } else {
            // Fallback: use current directory
            Logger_Init("lwsr_log.txt", "w");
        }
        Logger_Log("Debug logging enabled\n");
        Logger_Log("Debug mode: %s\n", g_debugMode ? "YES" : "NO");
    }
    
    // Start replay buffer if enabled in config
    if (g_config.replayEnabled) {
        Logger_Log("Starting replay buffer (enabled in config)\n");
        ReplayBuffer_Start(&g_replayBuffer, &g_config);
        // Register global hotkey for saving replay
        BOOL hotkeyOk = RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
        Logger_Log("RegisterHotKey(HOTKEY_REPLAY_SAVE, key=0x%02X): %s\n", 
                   g_config.replaySaveKey, hotkeyOk ? "SUCCESS" : "FAILED");
        if (!hotkeyOk) {
            Logger_Log("  GetLastError: %lu\n", GetLastError());
        }
    } else {
        Logger_Log("Replay buffer disabled in config\n");
    }
    
    // Start watchdog for hang detection (optional - monitors for frozen app)
    CrashHandler_StartWatchdog();
    
    // Message loop with heartbeat tracking
    MSG msg;
    DWORD msgCount = 0;
    DWORD hotkeyCount = 0;
    
    Logger_Log("Entering message loop\n");
    
    while (GetMessage(&msg, NULL, 0, 0)) {
        // Heartbeat to logger and crash handler
        Logger_Heartbeat(THREAD_MAIN);
        CrashHandler_Heartbeat();
        msgCount++;
        
        // Track WM_HOTKEY messages
        if (msg.message == WM_HOTKEY) {
            hotkeyCount++;
            Logger_Log("WM_HOTKEY received: wParam=%llu, hwnd=%p, total_hotkeys=%lu\n", 
                       (unsigned long long)msg.wParam, (void*)msg.hwnd, hotkeyCount);
        }
        
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Stop watchdog before cleanup
    CrashHandler_StopWatchdog();
    
    // Cleanup
    Logger_Log("Shutting down (msgs=%lu, hotkeys=%lu)\n", msgCount, hotkeyCount);
    UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
    ReplayBuffer_Shutdown(&g_replayBuffer);
    Logger_Flush();
    Logger_Shutdown();
    Config_Save(&g_config);
    GdiplusAPI_Shutdown(&g_gdip);
    Capture_Shutdown(&g_capture);
    MFShutdown();
    CoUninitialize();
    
    // Shutdown crash handler last
    CrashHandler_Shutdown();
    
    if (g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
    }
    
    return (int)msg.wParam;
}

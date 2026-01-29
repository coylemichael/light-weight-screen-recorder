/*
 * Replay Buffer - ShadowPlay-style instant replay
 * 
 * Architecture: State machine with event-based synchronization
 * - Clear lifecycle states prevent race conditions
 * - Windows events for cross-thread coordination (not just flags)
 * - Minimum buffer requirement before saves allowed
 */

#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include <windows.h>
#include "config.h"
#include "aac_encoder.h"  // For AACEncoderError

// Maximum encoded audio samples to store
#define MAX_AUDIO_SAMPLES 16384

// Minimum frames required before save is allowed (1 second worth)
#define MIN_FRAMES_FOR_SAVE 30

// Replay buffer lifecycle states
typedef enum {
    REPLAY_STATE_UNINITIALIZED,
    REPLAY_STATE_STARTING,      // Thread created, initializing components
    REPLAY_STATE_CAPTURING,     // Actively capturing frames, ready for saves
    REPLAY_STATE_SAVING,        // Save in progress
    REPLAY_STATE_STOPPING,      // Shutdown requested
    REPLAY_STATE_ERROR,         // Fatal error occurred
    REPLAY_STATE_RECOVERING     // HealthMonitor recovery in progress (renamed from STALLED)
} ReplayStateEnum;

typedef struct {
    BOOL enabled;
    int durationSeconds;
    CaptureMode captureSource;
    int monitorIndex;
    
    // State machine (use InterlockedExchange to modify)
    volatile LONG state;        // ReplayStateEnum
    volatile LONG framesCaptured; // Frames successfully encoded
    
    // Thread management
    HANDLE bufferThread;
    
    // Event-based synchronization (proper cross-thread coordination)
    HANDLE hReadyEvent;         // Signaled when capture loop is running and has frames
    HANDLE hSaveRequestEvent;   // Signaled by UI to request save
    HANDLE hSaveCompleteEvent;  // Signaled by buffer thread when save done
    HANDLE hStopEvent;          // Signaled to request shutdown
    
    // Save parameters (protected by hSaveRequestEvent sequencing)
    char savePath[MAX_PATH];
    volatile BOOL saveSuccess;  // Result of last save
    
    // Legacy compatibility
    BOOL isBuffering;
    volatile BOOL bufferReady;
    
    int frameWidth;
    int frameHeight;
    
    // Audio state
    BOOL audioEnabled;
    char audioSource1[256];
    char audioSource2[256];
    char audioSource3[256];
    int audioVolume1;
    int audioVolume2;
    int audioVolume3;
    volatile LONG audioError;  // AACEncoderError if audio init failed
    
    // Async save completion notification
    HWND notifyWindow;          // Window to receive WM_REPLAY_SAVE_COMPLETE
    UINT notifyMessage;         // Custom message ID (WM_USER + N)
} ReplayBufferState;

BOOL ReplayBuffer_Init(ReplayBufferState* state);
void ReplayBuffer_Shutdown(ReplayBufferState* state);
BOOL ReplayBuffer_Start(ReplayBufferState* state, const AppConfig* config);
void ReplayBuffer_Stop(ReplayBufferState* state);

// Synchronous save (blocks until complete - use for testing only)
BOOL ReplayBuffer_Save(ReplayBufferState* state, const char* outputPath);

// Asynchronous save (returns immediately, posts notifyMessage when done)
// wParam = success (BOOL), lParam = 0
// Returns FALSE if save cannot be started (not ready, already saving)
BOOL ReplayBuffer_SaveAsync(ReplayBufferState* state, const char* outputPath,
                            HWND notifyWindow, UINT notifyMessage);

// Check if a save is currently in progress
BOOL ReplayBuffer_IsSaving(ReplayBufferState* state);

int ReplayBuffer_EstimateRAMUsage(int durationSeconds, int width, int height, int fps, QualityPreset quality);
void ReplayBuffer_GetStatus(ReplayBufferState* state, char* buffer, int bufferSize);

#endif

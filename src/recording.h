/*
 * recording.h - Traditional recording lifecycle management
 * 
 * Owns the recording thread, capture loop, and encoder coordination.
 * Symmetric with replay_buffer.h - both manage their own lifecycles.
 */

#ifndef RECORDING_H
#define RECORDING_H

#include <windows.h>
#include "config.h"
#include "encoder.h"
#include "capture.h"

// Recording lifecycle states (matches ReplayStateEnum pattern)
typedef enum {
    RECORDING_STATE_IDLE,       // Not recording
    RECORDING_STATE_STARTING,   // Initializing encoder/capture
    RECORDING_STATE_ACTIVE,     // Recording in progress
    RECORDING_STATE_STOPPING,   // Finalizing
    RECORDING_STATE_ERROR       // Failed
} RecordingStateEnum;

typedef struct {
    // State machine (use InterlockedExchange to modify)
    volatile LONG state;            // RecordingStateEnum
    
    // Thread management
    HANDLE thread;
    volatile LONG stopRequested;    // Thread-safe stop flag
    
    // Encoder (owned)
    EncoderState encoder;
    
    // Capture reference (borrowed from caller)
    CaptureState* capture;
    
    // Timing
    ULONGLONG startTime;            // GetTickCount64 when started
    int fps;                        // Actual recording FPS
    
    // Output
    char outputPath[MAX_PATH];
    
    // Callbacks for UI notification (optional)
    HWND notifyWindow;
    UINT notifyMessage;             // Posted when recording stops
} RecordingState;

// Initialize recording state (call once at startup)
void Recording_Init(RecordingState* state);

// Start recording to file
// capture: Initialized capture state with region already set
// config: App config for format/quality settings
// Returns FALSE if already recording or initialization fails
BOOL Recording_Start(RecordingState* state, CaptureState* capture, 
                     const AppConfig* config, const char* outputPath);

// Stop recording (blocks until thread exits and file is finalized)
void Recording_Stop(RecordingState* state);

// Check if currently recording (thread-safe)
BOOL Recording_IsActive(RecordingState* state);

// Get elapsed time in milliseconds since recording started
ULONGLONG Recording_GetElapsedMs(RecordingState* state);

// Get current state (thread-safe)
RecordingStateEnum Recording_GetState(RecordingState* state);

// Cleanup (call at shutdown)
void Recording_Shutdown(RecordingState* state);

#endif // RECORDING_H

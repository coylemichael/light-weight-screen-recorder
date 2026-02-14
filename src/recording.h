/*
 * recording.h - Traditional Recording (NVENC Hardware Path)
 * 
 * USES: nvenc_encoder, gpu_converter, mp4_muxer (streaming)
 * 
 * Direct-to-disk recording; writes frames as they arrive.
 * Symmetric with replay_buffer.h - both use same encoding modules.
 * 
 * TODO: Add audio support (see replay_buffer.c for pattern)
 */

#ifndef RECORDING_H
#define RECORDING_H

#include <windows.h>
#include "config.h"
#include "capture.h"
#include "nvenc_encoder.h"
#include "gpu_converter.h"
#include "mp4_muxer.h"
#include "constants.h"

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
    
    // Video pipeline (owned)
    NVENCEncoder* encoder;          // NVENC hardware encoder
    GPUConverter gpuConverter;      // BGRA→NV12 GPU conversion
    StreamingMuxer* muxer;          // MP4 streaming writer
    BYTE seqHeader[MAX_SEQ_HEADER_SIZE];  // HEVC VPS/SPS/PPS
    DWORD seqHeaderSize;
    
    // Capture reference (borrowed from caller)
    CaptureState* capture;
    
    // Timing
    ULONGLONG startTime;            // GetTickCount64 when started
    int fps;                        // Actual recording FPS
    int width;
    int height;
    
    // Output
    char outputPath[MAX_PATH];
    
    // Stats
    volatile LONG framesCaptured;
    volatile LONG framesEncoded;
    
    // Callbacks for UI notification (optional)
    HWND notifyWindow;
    UINT notifyMessage;             // Posted when recording stops
} RecordingState;

// Initialize recording state (call once at startup)
void Recording_Init(RecordingState* state);

// Start recording to file
// capture: Initialized capture state with region already set
// config: App config for format/quality/audio settings
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

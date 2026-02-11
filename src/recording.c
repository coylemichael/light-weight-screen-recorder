/*
 * recording.c - Traditional recording lifecycle management
 * 
 * Owns the recording thread, capture loop, and encoder coordination.
 * Symmetric architecture with replay_buffer.c.
 */

#include <windows.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include "recording.h"
#include "logger.h"

#pragma comment(lib, "winmm.lib")

// Forward declarations
static DWORD WINAPI RecordingThread(LPVOID param);

void Recording_Init(RecordingState* state) {
    ZeroMemory(state, sizeof(RecordingState));
    state->state = RECORDING_STATE_IDLE;
}

BOOL Recording_Start(RecordingState* state, CaptureState* capture, 
                     const AppConfig* config, const char* outputPath) {
    // Thread-safe state check
    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    if (currentState != RECORDING_STATE_IDLE) {
        Logger_Log("Recording_Start: Already recording or in transition (state=%d)\n", currentState);
        return FALSE;
    }
    
    // Validate capture
    if (!capture || !capture->initialized) {
        Logger_Log("Recording_Start: Capture not initialized\n");
        return FALSE;
    }
    
    if (capture->captureWidth < 16 || capture->captureHeight < 16) {
        Logger_Log("Recording_Start: Capture area too small (%dx%d)\n", 
                   capture->captureWidth, capture->captureHeight);
        return FALSE;
    }
    
    // Transition to STARTING
    InterlockedExchange(&state->state, RECORDING_STATE_STARTING);
    
    // Store references
    state->capture = capture;
    strncpy(state->outputPath, outputPath, MAX_PATH - 1);
    state->outputPath[MAX_PATH - 1] = '\0';
    
    // Determine FPS (cap at 60 for MF encoder compatibility)
    state->fps = Capture_GetRefreshRate(capture);
    if (state->fps > 60) state->fps = 60;
    if (state->fps < 1) state->fps = 60;  // Fallback
    
    // Initialize encoder
    if (!Encoder_Init(&state->encoder, outputPath,
                      capture->captureWidth, capture->captureHeight,
                      state->fps, config->outputFormat, config->quality)) {
        Logger_Log("Recording_Start: Encoder init failed\n");
        InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
        return FALSE;
    }
    
    // Reset stop flag
    InterlockedExchange(&state->stopRequested, FALSE);
    
    // Record start time
    state->startTime = GetTickCount64();
    
    // Create recording thread
    state->thread = CreateThread(NULL, 0, RecordingThread, state, 0, NULL);
    if (!state->thread) {
        Logger_Log("Recording_Start: CreateThread failed (error=%lu)\n", GetLastError());
        Encoder_Finalize(&state->encoder);
        InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
        return FALSE;
    }
    
    // Transition to ACTIVE
    InterlockedExchange(&state->state, RECORDING_STATE_ACTIVE);
    Logger_Log("Recording_Start: Started recording to %s (%dx%d @ %d fps)\n",
               outputPath, capture->captureWidth, capture->captureHeight, state->fps);
    
    return TRUE;
}

void Recording_Stop(RecordingState* state) {
    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    if (currentState != RECORDING_STATE_ACTIVE) {
        return;  // Not recording
    }
    
    // Transition to STOPPING
    InterlockedExchange(&state->state, RECORDING_STATE_STOPPING);
    
    // Signal thread to stop
    InterlockedExchange(&state->stopRequested, TRUE);
    
    // Wait for thread
    if (state->thread) {
        DWORD waitResult = WaitForSingleObject(state->thread, 5000);
        if (waitResult == WAIT_TIMEOUT) {
            Logger_Log("Recording_Stop: Thread did not exit in 5 seconds\n");
        }
        CloseHandle(state->thread);
        state->thread = NULL;
    }
    
    // Finalize encoder
    Encoder_Finalize(&state->encoder);
    
    // Clear capture reference
    state->capture = NULL;
    
    // Transition to IDLE
    InterlockedExchange(&state->state, RECORDING_STATE_IDLE);
    Logger_Log("Recording_Stop: Recording stopped\n");
    
    // Notify UI if callback set
    if (state->notifyWindow && state->notifyMessage) {
        PostMessage(state->notifyWindow, state->notifyMessage, 0, 0);
    }
}

BOOL Recording_IsActive(RecordingState* state) {
    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    return (currentState == RECORDING_STATE_ACTIVE);
}

ULONGLONG Recording_GetElapsedMs(RecordingState* state) {
    if (!Recording_IsActive(state)) return 0;
    return GetTickCount64() - state->startTime;
}

RecordingStateEnum Recording_GetState(RecordingState* state) {
    return (RecordingStateEnum)InterlockedCompareExchange(&state->state, 0, 0);
}

void Recording_Shutdown(RecordingState* state) {
    Recording_Stop(state);
    ZeroMemory(state, sizeof(RecordingState));
}

/*
 * Recording thread - captures frames and writes to encoder
 * Uses CFR (constant frame rate) with synthetic timestamps
 */
static DWORD WINAPI RecordingThread(LPVOID param) {
    RecordingState* state = (RecordingState*)param;
    
    // Request high-resolution timer (1ms instead of 15.6ms)
    timeBeginPeriod(1);
    
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    
    int fps = state->fps;
    
    // CFR timing: use frame count for consistent timestamps
    // This avoids VFR stutter from wall-clock jitter (v1.3.2 bug fix)
    UINT64 frameCount = 0;
    UINT64 frameDuration100ns = 10000000ULL / fps;  // 100-nanosecond units for MF
    double frameIntervalSec = 1.0 / fps;
    
    // Thread-safe loop: read stop flag atomically
    while (!InterlockedCompareExchange(&state->stopRequested, 0, 0)) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        double targetTime = frameCount * frameIntervalSec;
        
        if (elapsed >= targetTime) {
            // Use frame-based timestamp for CFR playback
            UINT64 timestamp = frameCount * frameDuration100ns;
            BYTE* frame = Capture_GetFrame(state->capture, NULL);
            
            if (frame) {
                Encoder_WriteFrame(&state->encoder, frame, timestamp);
            }
            
            frameCount++;
            
            // Drop frames if falling behind (maintain real-time)
            double newElapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
            while ((frameCount * frameIntervalSec) < newElapsed - frameIntervalSec) {
                frameCount++;
            }
        } else {
            // Sleep until next frame time
            double sleepTime = (targetTime - elapsed) * 1000.0;
            if (sleepTime > 2.0) {
                Sleep((DWORD)(sleepTime - 1.5));
            } else if (sleepTime > 0.5) {
                Sleep(1);
            }
            // Busy-wait for sub-millisecond precision
        }
    }
    
    timeEndPeriod(1);
    return 0;
}

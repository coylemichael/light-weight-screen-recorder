/*
 * recording.c - Traditional Recording (NVENC Hardware Path)
 * 
 * USES: nvenc_encoder.c, gpu_converter.c, mp4_muxer.c (streaming)
 * 
 * Direct-to-disk recording using NVENC hardware encoding.
 * Writes frames to MP4 as they arrive (no buffering like replay).
 * Pipeline: DXGI capture → GPU color convert → NVENC → StreamingMuxer
 * 
 * Symmetric architecture with replay_buffer.c - both use same encoding modules.
 * 
 * TODO: Add audio support (see replay_buffer.c for pattern)
 */

#include <windows.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include "recording.h"
#include "logger.h"

#pragma comment(lib, "winmm.lib")

/* Alias for logging */
#define RecLog Logger_Log

/* Context passed to NVENC callback */
typedef struct {
    StreamingMuxer* muxer;
    volatile LONG* framesEncoded;
    LONGLONG frameDuration;  // 100-ns units
} RecordingEncoderContext;

/* Forward declarations */
static DWORD WINAPI RecordingThread(LPVOID param);
static void EncoderCallback(EncodedFrame* frame, void* userData);

/* Global encoder context (set during recording) */
static RecordingEncoderContext g_encoderCtx = {0};

void Recording_Init(RecordingState* state) {
    ZeroMemory(state, sizeof(RecordingState));
    state->state = RECORDING_STATE_IDLE;
}

BOOL Recording_Start(RecordingState* state, CaptureState* capture, 
                     const AppConfig* config, const char* outputPath) {
    // Thread-safe state check
    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    if (currentState != RECORDING_STATE_IDLE) {
        RecLog("Recording_Start: Already recording or in transition (state=%d)\n", currentState);
        return FALSE;
    }
    
    // Validate capture
    if (!capture || !capture->initialized) {
        RecLog("Recording_Start: Capture not initialized\n");
        return FALSE;
    }
    
    if (capture->captureWidth < 16 || capture->captureHeight < 16) {
        RecLog("Recording_Start: Capture area too small (%dx%d)\n", 
               capture->captureWidth, capture->captureHeight);
        return FALSE;
    }
    
    // Transition to STARTING
    InterlockedExchange(&state->state, RECORDING_STATE_STARTING);
    
    // Store references
    state->capture = capture;
    state->width = capture->captureWidth;
    state->height = capture->captureHeight;
    strncpy(state->outputPath, outputPath, MAX_PATH - 1);
    state->outputPath[MAX_PATH - 1] = '\0';
    
    // Determine FPS from config or monitor refresh rate
    // Recording uses same FPS setting as replay buffer
    state->fps = config->replayFPS;
    if (state->fps <= 0) {
        state->fps = Capture_GetRefreshRate(capture);
    }
    if (state->fps > 240) state->fps = 240;
    if (state->fps < 1) state->fps = 60;  // Fallback
    
    RecLog("Recording_Start: Initializing NVENC pipeline (%dx%d @ %d fps)\n",
           state->width, state->height, state->fps);
    
    // Initialize GPU color converter (BGRA → NV12)
    if (!GPUConverter_Init(&state->gpuConverter, capture->device, state->width, state->height)) {
        RecLog("Recording_Start: GPUConverter_Init failed\n");
        InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
        return FALSE;
    }
    RecLog("Recording_Start: GPU color converter initialized\n");
    
    // Initialize NVENC encoder
    state->encoder = NVENCEncoder_Create(capture->device, state->width, state->height, 
                                          state->fps, config->quality);
    if (!state->encoder) {
        RecLog("Recording_Start: NVENCEncoder_Create failed - NVIDIA GPU required\n");
        GPUConverter_Shutdown(&state->gpuConverter);
        InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
        return FALSE;
    }
    RecLog("Recording_Start: NVENC HEVC encoder initialized\n");
    
    // Get sequence header (VPS/SPS/PPS for HEVC)
    if (!NVENCEncoder_GetSequenceHeader(state->encoder, state->seqHeader, 
                                         sizeof(state->seqHeader), &state->seqHeaderSize)) {
        RecLog("Recording_Start: WARNING - Failed to get sequence header\n");
        state->seqHeaderSize = 0;
    } else {
        RecLog("Recording_Start: HEVC sequence header extracted (%u bytes)\n", state->seqHeaderSize);
    }
    
    // Configure muxer (video only for now)
    MuxerConfig muxConfig = {
        .width = state->width,
        .height = state->height,
        .fps = state->fps,
        .quality = config->quality,
        .seqHeader = state->seqHeader,
        .seqHeaderSize = state->seqHeaderSize
    };
    
    // Create streaming muxer (video only)
    state->muxer = StreamingMuxer_Create(outputPath, &muxConfig);
    
    if (!state->muxer) {
        RecLog("Recording_Start: StreamingMuxer_Create failed\n");
        NVENCEncoder_Destroy(state->encoder);
        GPUConverter_Shutdown(&state->gpuConverter);
        state->encoder = NULL;
        InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
        return FALSE;
    }
    RecLog("Recording_Start: Streaming muxer ready\n");
    
    // Set up encoder callback context
    LONGLONG frameDuration = MF_UNITS_PER_SECOND / state->fps;
    g_encoderCtx.muxer = state->muxer;
    g_encoderCtx.framesEncoded = &state->framesEncoded;
    g_encoderCtx.frameDuration = frameDuration;
    
    // Set encoder callback for async frame delivery
    NVENCEncoder_SetCallback(state->encoder, EncoderCallback, &g_encoderCtx);
    
    // Reset counters and stop flag
    InterlockedExchange(&state->framesCaptured, 0);
    InterlockedExchange(&state->framesEncoded, 0);
    InterlockedExchange(&state->stopRequested, FALSE);
    
    // Record start time
    state->startTime = GetTickCount64();
    
    // Create recording thread
    state->thread = CreateThread(NULL, 0, RecordingThread, state, 0, NULL);
    if (!state->thread) {
        RecLog("Recording_Start: CreateThread failed (error=%lu)\n", GetLastError());
        StreamingMuxer_Abort(state->muxer);
        NVENCEncoder_Destroy(state->encoder);
        GPUConverter_Shutdown(&state->gpuConverter);
        state->muxer = NULL;
        state->encoder = NULL;
        InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
        return FALSE;
    }
    
    // Transition to ACTIVE
    InterlockedExchange(&state->state, RECORDING_STATE_ACTIVE);
    RecLog("Recording_Start: Recording started to %s\n", outputPath);
    
    return TRUE;
}

void Recording_Stop(RecordingState* state) {
    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    if (currentState != RECORDING_STATE_ACTIVE) {
        return;  // Not recording
    }
    
    // Transition to STOPPING
    InterlockedExchange(&state->state, RECORDING_STATE_STOPPING);
    RecLog("Recording_Stop: Stopping recording...\n");
    
    // Signal thread to stop
    InterlockedExchange(&state->stopRequested, TRUE);
    
    // Wait for thread
    if (state->thread) {
        DWORD waitResult = WaitForSingleObject(state->thread, 10000);
        if (waitResult == WAIT_TIMEOUT) {
            RecLog("Recording_Stop: Thread did not exit in 10 seconds\n");
        }
        CloseHandle(state->thread);
        state->thread = NULL;
    }
    
    // Flush NVENC (get remaining frames)
    if (state->encoder) {
        EncodedFrame flushed;
        while (NVENCEncoder_Flush(state->encoder, &flushed)) {
            // Manually invoke callback - flush doesn't trigger async callback
            EncoderCallback(&flushed, &g_encoderCtx);
        }
        NVENCEncoder_Destroy(state->encoder);
        state->encoder = NULL;
    }
    
    // Shutdown GPU converter
    GPUConverter_Shutdown(&state->gpuConverter);
    
    // Close muxer (finalizes MP4)
    if (state->muxer) {
        BOOL muxOk = StreamingMuxer_Close(state->muxer);
        state->muxer = NULL;
        RecLog("Recording_Stop: Muxer finalized %s\n", muxOk ? "OK" : "FAILED");
    }
    
    // Clear encoder context
    ZeroMemory(&g_encoderCtx, sizeof(g_encoderCtx));
    
    // Clear capture reference
    state->capture = NULL;
    
    // Transition to IDLE
    InterlockedExchange(&state->state, RECORDING_STATE_IDLE);
    
    LONG captured = InterlockedCompareExchange(&state->framesCaptured, 0, 0);
    LONG encoded = InterlockedCompareExchange(&state->framesEncoded, 0, 0);
    RecLog("Recording_Stop: Complete (captured=%d, encoded=%d)\n", captured, encoded);
    
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
 * Encoder callback - called from NVENC output thread when frame is ready
 * Writes encoded frame directly to streaming muxer
 */
static void EncoderCallback(EncodedFrame* frame, void* userData) {
    RecordingEncoderContext* ctx = (RecordingEncoderContext*)userData;
    if (!frame || !frame->data || !ctx || !ctx->muxer) return;
    
    MuxerSample sample = {
        .data = frame->data,
        .size = frame->size,
        .timestamp = frame->timestamp,
        .duration = ctx->frameDuration,
        .isKeyframe = frame->isKeyframe
    };
    
    if (StreamingMuxer_WriteVideo(ctx->muxer, &sample)) {
        InterlockedIncrement(ctx->framesEncoded);
    }
}

/*
 * Recording thread - captures frames and submits to NVENC
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
    if (fps < 1) fps = 60;  // Guard against division by zero
    LONGLONG frameDuration100ns = MF_UNITS_PER_SECOND / fps;
    double frameIntervalMs = 1000.0 / fps;
    
    UINT64 frameCount = 0;
    
    RecLog("RecordingThread: Started (fps=%d, interval=%.2fms)\n", fps, frameIntervalMs);
    
    // Main capture loop
    while (!InterlockedCompareExchange(&state->stopRequested, 0, 0)) {
        QueryPerformanceCounter(&now);
        double elapsedMs = (double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        double targetMs = (double)frameCount * frameIntervalMs;
        
        if (elapsedMs >= targetMs) {
            // Synthetic timestamp for CFR
            LONGLONG timestamp = (LONGLONG)frameCount * frameDuration100ns;
            
            // Capture frame as GPU texture (stays on GPU)
            ID3D11Texture2D* bgraTexture = Capture_GetFrameTexture(state->capture, NULL);
            
            if (bgraTexture) {
                // Convert BGRA to NV12 on GPU
                ID3D11Texture2D* nv12Texture = GPUConverter_Convert(&state->gpuConverter, bgraTexture);
                
                if (nv12Texture) {
                    // Submit to NVENC (async - callback will write to muxer)
                    int result = NVENCEncoder_SubmitTexture(state->encoder, nv12Texture, timestamp);
                    
                    if (result == 1) {
                        InterlockedIncrement(&state->framesCaptured);
                    } else if (result == -1) {
                        // Device lost - exit
                        RecLog("RecordingThread: NVENC device lost, exiting\n");
                        break;
                    }
                }
            }
            
            frameCount++;
            
            // Skip frames if falling behind
            double newElapsedMs = (double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
            while (((double)frameCount * frameIntervalMs) < newElapsedMs - frameIntervalMs) {
                frameCount++;  // Skip
            }
        } else {
            // Sleep until next frame
            double sleepMs = targetMs - elapsedMs;
            if (sleepMs > 2.0) {
                Sleep((DWORD)(sleepMs - 1.5));
            } else if (sleepMs > 0.5) {
                Sleep(1);
            }
            // Busy-wait for sub-ms precision
        }
    }
    
    timeEndPeriod(1);
    
    LONG captured = InterlockedCompareExchange(&state->framesCaptured, 0, 0);
    RecLog("RecordingThread: Exiting after %d frames\n", captured);
    
    return 0;
}

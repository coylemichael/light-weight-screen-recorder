/*
 * recording.c - Direct-to-disk NVENC recording (thread, start/stop, state machine)
 *
 * USES: nvenc_encoder.c, gpu_converter.c, mp4_muxer.c (streaming)
 *
 * Direct-to-disk recording using NVENC hardware encoding.
 * Writes frames to MP4 as they arrive (no buffering like replay).
 * Pipeline: DXGI capture → GPU color convert → NVENC → StreamingMuxer
 *
 * Symmetric architecture with replay_buffer.c - both use same encoding modules.
 */

#include <windows.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include "recording.h"
#include "replay_buffer.h"
#include "mem_utils.h"
#include "logger.h"

#pragma comment(lib, "winmm.lib")

/* Alias for logging */
#define RecLog Logger_Log

/* Global replay buffer (defined in main.c) — checked at Recording_Start to
 * enforce the "never start recording while replay buffer is running" rule. */
extern ReplayBufferState g_replayBuffer;

/* Context passed to NVENC callback */
typedef struct {
    StreamingMuxer* muxer;
    volatile LONG* framesEncoded;
    LONGLONG frameDuration;  // 100-ns units
} RecordingEncoderContext;

/* Forward declarations */
static DWORD WINAPI RecordingThread(LPVOID param);
static void EncoderCallback(EncodedFrame* frame, void* userData);

/*
 * Module-level singleton: encoder context for the in-flight recording.
 *
 * Ownership: Recording_Start populates it before the encoder begins emitting
 * frames; Recording_Stop zeroes it after the encoder is destroyed and the
 * flush loop has drained. Because it is a single shared instance, callers
 * MUST serialize Recording_Start / Recording_Stop (the state machine guards
 * this for normal flows). The MemoryBarrier() in Recording_Start ensures the
 * NVENC output thread observes initialized fields on weakly-ordered targets
 * (ARM64); on x86/x64 the ordering is implicit.
 */
static RecordingEncoderContext g_encoderCtx = {0};

void Recording_Init(RecordingState* state) {
    if (!state) return;
    ZeroMemory(state, sizeof(RecordingState));
    state->state = RECORDING_STATE_IDLE;
    /* Auto-reset event so each Start/Stop cycle reuses the same handle. */
    state->hStopEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
}

BOOL Recording_Start(RecordingState* state, CaptureState* capture,
                     const AppConfig* config, const char* outputPath) {
    BOOL gpuConverterInit = FALSE;
    BOOL success = FALSE;

    /* Defend at point of use */
    if (!state) {
        RecLog("Recording_Start: NULL state\n");
        return FALSE;
    }
    if (!outputPath) {
        RecLog("Recording_Start: NULL outputPath\n");
        return FALSE;
    }

    /* Critical rule: never start recording while replay buffer is running
     * (shared NVENC singleton causes deadlock). */
    if (ReplayBuffer_IsActive(&g_replayBuffer)) {
        RecLog("Recording_Start: Replay buffer is active - refusing to start (deadlock guard)\n");
        return FALSE;
    }

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
        goto cleanup;
    }
    gpuConverterInit = TRUE;
    RecLog("Recording_Start: GPU color converter initialized\n");

    // Initialize NVENC encoder
    state->encoder = NVENCEncoder_Create(capture->device, state->width, state->height,
                                          state->fps, config->quality);
    if (!state->encoder) {
        RecLog("Recording_Start: NVENCEncoder_Create failed - NVIDIA GPU required\n");
        goto cleanup;
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
        goto cleanup;
    }
    RecLog("Recording_Start: Streaming muxer ready\n");

    // Set up encoder callback context
    LONGLONG frameDuration = MF_UNITS_PER_SECOND / state->fps;
    g_encoderCtx.muxer = state->muxer;
    g_encoderCtx.framesEncoded = &state->framesEncoded;
    g_encoderCtx.frameDuration = frameDuration;

    // Ensure all g_encoderCtx writes are visible before encoder thread reads them.
    // Safe on x86 (strong ordering) but required for ARM64 Windows correctness.
    MemoryBarrier();

    // Set encoder callback for async frame delivery
    NVENCEncoder_SetCallback(state->encoder, EncoderCallback, &g_encoderCtx);

    // Reset counters and stop signaling
    InterlockedExchange(&state->framesCaptured, 0);
    InterlockedExchange(&state->framesEncoded, 0);
    InterlockedExchange(&state->stopRequested, FALSE);
    if (state->hStopEvent) ResetEvent(state->hStopEvent);

    // Record start time
    state->startTime = GetTickCount64();

    // Create recording thread
    state->thread = CreateThread(NULL, 0, RecordingThread, state, 0, NULL);
    if (!state->thread) {
        RecLog("Recording_Start: CreateThread failed (error=%lu)\n", GetLastError());
        StreamingMuxer_Abort(state->muxer);
        state->muxer = NULL;
        goto cleanup;
    }

    // Transition to ACTIVE
    InterlockedExchange(&state->state, RECORDING_STATE_ACTIVE);
    RecLog("Recording_Start: Recording started to %s\n", outputPath);
    success = TRUE;
    return TRUE;

cleanup:
    if (state->muxer) {
        StreamingMuxer_Abort(state->muxer);
        state->muxer = NULL;
    }
    if (state->encoder) {
        NVENCEncoder_Destroy(state->encoder);
        state->encoder = NULL;
    }
    if (gpuConverterInit) {
        GPUConverter_Shutdown(&state->gpuConverter);
    }
    InterlockedExchange(&state->state, RECORDING_STATE_ERROR);
    return success;
}

void Recording_Stop(RecordingState* state) {
    if (!state) return;

    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    if (currentState != RECORDING_STATE_ACTIVE) {
        return;  // Not recording
    }

    // Transition to STOPPING
    InterlockedExchange(&state->state, RECORDING_STATE_STOPPING);
    RecLog("Recording_Stop: Stopping recording...\n");

    // Signal thread to stop (event + flag for belt-and-braces)
    InterlockedExchange(&state->stopRequested, TRUE);
    if (state->hStopEvent) SetEvent(state->hStopEvent);

    // Wait for thread
    if (state->thread) {
        DWORD waitResult = WaitForSingleObject(state->thread, 10000);
        if (waitResult == WAIT_TIMEOUT) {
            RecLog("Recording_Stop: Thread did not exit in 10 seconds\n");
        }
        SAFE_CLOSE_HANDLE(state->thread);
    }

    // Destroy NVENC encoder (sync mode + frameIntervalP=1 has no queued
    // frames to drain; Destroy sends EOS internally).
    if (state->encoder) {
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
}

BOOL Recording_IsActive(RecordingState* state) {
    if (!state) return FALSE;
    LONG currentState = InterlockedCompareExchange(&state->state, 0, 0);
    return (currentState == RECORDING_STATE_ACTIVE);
}

ULONGLONG Recording_GetElapsedMs(RecordingState* state) {
    if (!Recording_IsActive(state)) return 0;
    return GetTickCount64() - state->startTime;
}

RecordingStateEnum Recording_GetState(RecordingState* state) {
    if (!state) return RECORDING_STATE_IDLE;
    return (RecordingStateEnum)InterlockedCompareExchange(&state->state, 0, 0);
}

void Recording_Shutdown(RecordingState* state) {
    if (!state) return;
    Recording_Stop(state);
    SAFE_CLOSE_HANDLE(state->hStopEvent);
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
 * Recording thread - captures frames and submits to NVENC.
 * Uses CFR (constant frame rate) with synthetic timestamps:
 *   timestamp = frameNumber * frameInterval
 * This is the v1.3.2 lesson — wall-clock QPC PTS produced VFR stutter and
 * must NOT be reintroduced. See docs/rules/coding-rules.md "Lessons Learned".
 * Stop signaling uses an auto-reset event (state->hStopEvent), not a polled flag.
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
    double frameIntervalMs = 1000.0 / fps;
    /* Synthetic CFR PTS step in 100-ns units. Computed once to avoid
     * repeated multiply/divide and the integer overflow window the
     * previous QPC-based formula had on long uptimes. */
    LONGLONG frameInterval = (LONGLONG)MF_UNITS_PER_SECOND / fps;

    UINT64 frameCount = 0;

    RecLog("RecordingThread: Started (fps=%d, interval=%.2fms)\n", fps, frameIntervalMs);

    // Main capture loop — wait on hStopEvent instead of polling a flag
    for (;;) {
        QueryPerformanceCounter(&now);
        double elapsedMs = (double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        double targetMs = (double)frameCount * frameIntervalMs;

        if (elapsedMs >= targetMs) {
            /* Synthetic CFR timestamp: monotonic by construction. */
            LONGLONG timestamp = (LONGLONG)frameCount * frameInterval;

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

            /* Check stop event without sleeping (zero timeout). */
            if (state->hStopEvent &&
                WaitForSingleObject(state->hStopEvent, 0) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            // Wait until next frame OR stop signal, whichever comes first
            double sleepMs = targetMs - elapsedMs;
            DWORD waitMs = (sleepMs > 1.0) ? (DWORD)(sleepMs) : 1;
            if (state->hStopEvent) {
                if (WaitForSingleObject(state->hStopEvent, waitMs) == WAIT_OBJECT_0) {
                    break;
                }
            } else {
                Sleep(waitMs);
            }
        }
    }

    timeEndPeriod(1);

    LONG captured = InterlockedCompareExchange(&state->framesCaptured, 0, 0);
    RecLog("RecordingThread: Exiting after %d frames\n", captured);

    return 0;
}


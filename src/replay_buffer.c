/*
 * Replay Buffer - ShadowPlay-style instant replay
 * 
 * Uses RAM-based circular buffer of encoded HEVC samples.
 * On save: muxes buffered samples to MP4 (no re-encoding).
 * Full GPU pipeline: DXGI capture → GPU color convert → NVENC (native API)
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation/precondition checks
 * - HRESULT checks use FAILED()/SUCCEEDED() macros exclusively
 * - Thread-safe state checks using InterlockedCompareExchange
 * - All errors logged; non-critical errors allow graceful degradation
 * - Returns BOOL to propagate errors; callers must check
 */

#include "replay_buffer.h"
#include "nvenc_encoder.h"
#include "sample_buffer.h"
#include "capture.h"
#include "config.h"
#include "util.h"
#include "logger.h"
#include "audio_capture.h"
#include "aac_encoder.h"
#include "mp4_muxer.h"
#include "gpu_converter.h"
#include "constants.h"
#include <stdio.h>
#include <mmsystem.h>  /* For timeBeginPeriod/timeEndPeriod */
#include <objbase.h>   /* For CoInitializeEx/CoUninitialize */

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

/* ============================================================================
 * INTERNAL STATE STRUCTURES
 * ============================================================================
 * These structs consolidate related state that was previously scattered
 * across individual static globals. Passed to thread proc as context.
 */

/*
 * ReplayVideoState - Video encoding pipeline state
 * Owned by: Buffer thread
 * Lifetime: Created in BufferThreadProc, destroyed on thread exit
 */
typedef struct ReplayVideoState {
    NVENCEncoder* encoder;              /* NVENC hardware encoder instance */
    SampleBuffer sampleBuffer;          /* Circular buffer of encoded frames */
    BYTE seqHeader[MAX_SEQ_HEADER_SIZE];/* HEVC VPS/SPS/PPS for muxing */
    DWORD seqHeaderSize;                /* Size of sequence header */
} ReplayVideoState;

/*
 * ReplayAudioState - Audio encoding pipeline state
 * Owned by: Buffer thread, callbacks protected by lock
 * Lifetime: Created in BufferThreadProc, destroyed on thread exit
 */
typedef struct ReplayAudioState {
    AudioCaptureContext* capture;       /* WASAPI capture context */
    AACEncoder* encoder;                /* AAC MFT encoder */
    MuxerAudioSample* samples;          /* Circular buffer of AAC samples */
    int sampleCount;                    /* Current sample count */
    int sampleCapacity;                 /* Allocated capacity */
    BYTE* configData;                   /* AAC AudioSpecificConfig */
    int configSize;                     /* Size of configData */
    LONGLONG maxDuration;               /* Max buffer duration (100-ns units) */
    CRITICAL_SECTION lock;              /* Protects samples array */
    BOOL lockInitialized;               /* Track CS initialization */
} ReplayAudioState;

/*
 * ReplayInternalState - Combined internal state for buffer thread
 * Single instance, passed to BufferThreadProc
 */
typedef struct ReplayInternalState {
    ReplayVideoState video;
    ReplayAudioState audio;
} ReplayInternalState;

/* ============================================================================
 * MODULE STATE
 * ============================================================================
 * Single instance of internal state. Pointer used by callbacks.
 */
static ReplayInternalState g_internal = {0};

/* ---- External References ---- */
extern CaptureState g_capture;      /* From main.c - DXGI capture */
extern AppConfig g_config;          /* From main.c - Application config */

static DWORD WINAPI BufferThreadProc(LPVOID param);

/* Alias for logging */
#define ReplayLog Logger_Log

/* Callback for draining completed encoded frames into sample buffer */
/* Called from NVENC output thread - must be thread-safe */
static void DrainCallback(EncodedFrame* frame, void* userData) {
    SampleBuffer* buffer = (SampleBuffer*)userData;
    if (frame && frame->data && buffer) {
        SampleBuffer_Add(buffer, frame);
    }
}

/* Audio callback - stores encoded AAC samples */
/* Called from audio mixer thread - protected by audio.lock */
static void AudioEncoderCallback(const AACSample* sample, void* userData) {
    ReplayAudioState* audio = (ReplayAudioState*)userData;
    if (!sample || !sample->data || sample->size <= 0 || !audio) return;
    
    EnterCriticalSection(&audio->lock);
    
    /* Time-based eviction: remove samples older than max duration */
    if (audio->sampleCount > 0 && audio->maxDuration > 0) {
        int evicted = 0;
        while (audio->sampleCount > 0) {
            LONGLONG oldest = audio->samples[0].timestamp;
            LONGLONG span = sample->timestamp - oldest;
            
            if (span <= audio->maxDuration) {
                break;  /* Within duration limit */
            }
            
            /* Evict oldest sample */
            if (audio->samples[0].data) {
                free(audio->samples[0].data);
            }
            memmove(audio->samples, audio->samples + 1, 
                    (audio->sampleCount - 1) * sizeof(MuxerAudioSample));
            audio->sampleCount--;
            evicted++;
        }
        
        /* Log eviction periodically */
        static int audioEvictLogCounter = 0;
        audioEvictLogCounter++;
        if (evicted > 0 && (audioEvictLogCounter % AUDIO_EVICT_LOG_INTERVAL) == 0) {
            double spanSec = 0;
            if (audio->sampleCount > 0) {
                spanSec = (sample->timestamp - audio->samples[0].timestamp) / (double)MF_UNITS_PER_SECOND;
            }
            ReplayLog("Audio eviction: removed %d samples, count=%d, span=%.2fs\n",
                      evicted, audio->sampleCount, spanSec);
        }
    }
    
    /* Grow array if needed (capacity-based) */
    if (audio->sampleCount >= audio->sampleCapacity) {
        int newCapacity = audio->sampleCapacity == 0 ? INITIAL_AUDIO_CAPACITY : audio->sampleCapacity * AUDIO_CAPACITY_GROWTH_FACTOR;
        if (newCapacity > MAX_AUDIO_SAMPLES) newCapacity = MAX_AUDIO_SAMPLES;
        
        if (audio->sampleCount >= newCapacity) {
            /* Still full after time eviction - emergency capacity eviction */
            int toKeep = (int)(newCapacity * EMERGENCY_KEEP_FRACTION);
            int toRemove = audio->sampleCount - toKeep;
            
            for (int i = 0; i < toRemove && i < audio->sampleCount; i++) {
                if (audio->samples[i].data) {
                    free(audio->samples[i].data);
                }
            }
            
            memmove(audio->samples, audio->samples + toRemove, 
                    toKeep * sizeof(MuxerAudioSample));
            audio->sampleCount = toKeep;
        } else {
            MuxerAudioSample* newArr = realloc(audio->samples, 
                                                newCapacity * sizeof(MuxerAudioSample));
            if (newArr) {
                /* Zero-initialize new capacity */
                if (audio->sampleCapacity < newCapacity) {
                    memset(newArr + audio->sampleCapacity, 0, 
                           (newCapacity - audio->sampleCapacity) * sizeof(MuxerAudioSample));
                }
                audio->samples = newArr;
                audio->sampleCapacity = newCapacity;
            } else {
                static int reallocFailCount = 0;
                if (++reallocFailCount <= MAX_REALLOC_FAIL_LOGS) {
                    ReplayLog("WARNING: Audio buffer realloc failed (count=%d, capacity=%d)\n", 
                              audio->sampleCount, newCapacity);
                }
            }
        }
    }
    
    if (audio->sampleCount < audio->sampleCapacity) {
        MuxerAudioSample* dst = &audio->samples[audio->sampleCount];
        dst->data = (BYTE*)malloc(sample->size);
        if (dst->data) {
            memcpy(dst->data, sample->data, sample->size);
            dst->size = sample->size;
            dst->timestamp = sample->timestamp;
            dst->duration = sample->duration;
            audio->sampleCount++;
        }
    }
    
    LeaveCriticalSection(&audio->lock);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

BOOL ReplayBuffer_Init(ReplayBufferState* state) {
    /* Precondition */
    LWSR_ASSERT(state != NULL);
    
    if (!state) return FALSE;
    ZeroMemory(state, sizeof(ReplayBufferState));
    
    /* Initialize internal state structure */
    ZeroMemory(&g_internal, sizeof(g_internal));
    
    /* Create synchronization events */
    state->hReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  /* Manual reset */
    state->hSaveRequestEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  /* Auto reset */
    state->hSaveCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  /* Auto reset */
    state->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  /* Manual reset */
    
    if (!state->hReadyEvent || !state->hSaveRequestEvent || 
        !state->hSaveCompleteEvent || !state->hStopEvent) {
        ReplayLog("Failed to create synchronization events\n");
        if (state->hReadyEvent) CloseHandle(state->hReadyEvent);
        if (state->hSaveRequestEvent) CloseHandle(state->hSaveRequestEvent);
        if (state->hSaveCompleteEvent) CloseHandle(state->hSaveCompleteEvent);
        if (state->hStopEvent) CloseHandle(state->hStopEvent);
        return FALSE;
    }
    
    /* Initialize audio critical section */
    state->state = REPLAY_STATE_UNINITIALIZED;
    InitializeCriticalSection(&g_internal.audio.lock);
    g_internal.audio.lockInitialized = TRUE;
    return TRUE;
}

void ReplayBuffer_Shutdown(ReplayBufferState* state) {
    if (!state) return;
    ReplayBuffer_Stop(state);
    
    /* Close event handles */
    if (state->hReadyEvent) CloseHandle(state->hReadyEvent);
    if (state->hSaveRequestEvent) CloseHandle(state->hSaveRequestEvent);
    if (state->hSaveCompleteEvent) CloseHandle(state->hSaveCompleteEvent);
    if (state->hStopEvent) CloseHandle(state->hStopEvent);
    state->hReadyEvent = NULL;
    state->hSaveRequestEvent = NULL;
    state->hSaveCompleteEvent = NULL;
    state->hStopEvent = NULL;
    
    /* Clean up audio samples - must be done BEFORE deleting critical section */
    ReplayAudioState* audio = &g_internal.audio;
    if (audio->lockInitialized) {
        EnterCriticalSection(&audio->lock);
        if (audio->samples) {
            for (int i = 0; i < audio->sampleCount; i++) {
                if (audio->samples[i].data) free(audio->samples[i].data);
            }
            free(audio->samples);
            audio->samples = NULL;
        }
        audio->sampleCount = 0;
        audio->sampleCapacity = 0;
        audio->maxDuration = 0;
        LeaveCriticalSection(&audio->lock);
        
        DeleteCriticalSection(&audio->lock);
        audio->lockInitialized = FALSE;
    }
    
    /* Logger cleanup is handled by Logger_Shutdown in main.c */
}

BOOL ReplayBuffer_Start(ReplayBufferState* state, const AppConfig* config) {
    /* Preconditions */
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(config != NULL);
    
    if (!state || !config) return FALSE;
    if (state->isBuffering) return TRUE;
    
    state->enabled = config->replayEnabled;
    state->durationSeconds = config->replayDuration;
    state->captureSource = config->replayCaptureSource;
    state->monitorIndex = config->replayMonitorIndex;
    
    /* Copy audio settings */
    state->audioEnabled = config->audioEnabled;
    strncpy(state->audioSource1, config->audioSource1, sizeof(state->audioSource1) - 1);
    strncpy(state->audioSource2, config->audioSource2, sizeof(state->audioSource2) - 1);
    strncpy(state->audioSource3, config->audioSource3, sizeof(state->audioSource3) - 1);
    state->audioVolume1 = config->audioVolume1;
    state->audioVolume2 = config->audioVolume2;
    state->audioVolume3 = config->audioVolume3;
    
    if (!state->enabled) return FALSE;
    
    /* Reset state machine */
    InterlockedExchange(&state->state, REPLAY_STATE_STARTING);
    InterlockedExchange(&state->framesCaptured, 0);
    state->saveSuccess = FALSE;
    state->savePath[0] = '\0';
    
    /* Reset events */
    ResetEvent(state->hReadyEvent);
    ResetEvent(state->hSaveCompleteEvent);
    ResetEvent(state->hStopEvent);
    
    /* Legacy flags */
    state->bufferReady = FALSE;
    
    /* Reset audio buffer using new struct */
    ReplayAudioState* audio = &g_internal.audio;
    EnterCriticalSection(&audio->lock);
    for (int i = 0; i < audio->sampleCount; i++) {
        if (audio->samples[i].data) {
            free(audio->samples[i].data);
            audio->samples[i].data = NULL;
        }
    }
    audio->sampleCount = 0;
    audio->maxDuration = 0;
    LeaveCriticalSection(&audio->lock);
    
    state->bufferThread = CreateThread(NULL, 0, BufferThreadProc, state, 0, NULL);
    state->isBuffering = (state->bufferThread != NULL);
    
    if (!state->isBuffering) {
        InterlockedExchange(&state->state, REPLAY_STATE_ERROR);
        return FALSE;
    }
    
    /* Wait up to 5 seconds for the buffer thread to become ready */
    DWORD waitResult = WaitForSingleObject(state->hReadyEvent, 5000);
    if (waitResult != WAIT_OBJECT_0) {
        ReplayLog("Timeout waiting for buffer thread to become ready\n");
        /* Don't fail - thread is running, just not ready yet */
    }
    
    return state->isBuffering;
}

void ReplayBuffer_Stop(ReplayBufferState* state) {
    if (!state || !state->isBuffering) return;
    
    /* Signal stop via event (proper cross-thread communication) */
    (void)InterlockedExchange(&state->state, REPLAY_STATE_STOPPING);
    SetEvent(state->hStopEvent);
    
    if (state->bufferThread) {
        DWORD waitResult = WaitForSingleObject(state->bufferThread, 5000);
        
        if (waitResult == WAIT_TIMEOUT) {
            /* Thread is hung! We can't safely clean up the encoder from here
             * because the hung thread may still be using it.
             * Log it and leak the resources - safer than crashing. */
            ReplayLog("WARNING: Buffer thread hung (5s timeout) - forcing cleanup\n");
            
            /* Try to terminate the hung thread (dangerous but necessary) */
            TerminateThread(state->bufferThread, 1);
            
            /* Give terminated thread a moment to die */
            WaitForSingleObject(state->bufferThread, 1000);
            
            /* Force destroy the encoder since the thread won't do it */
            ReplayVideoState* video = &g_internal.video;
            if (video->encoder) {
                ReplayLog("Force-destroying hung encoder...\n");
                /* Don't call NVENCEncoder_Destroy - it tries to stop output thread
                 * which is also likely hung. Just leak it. */
                video->encoder = NULL;
            }
        }
        
        CloseHandle(state->bufferThread);
        state->bufferThread = NULL;
    }
    state->isBuffering = FALSE;
}

BOOL ReplayBuffer_Save(ReplayBufferState* state, const char* outputPath) {
    // Preconditions
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(outputPath != NULL);
    
    if (!state || !outputPath || !state->isBuffering) {
        ReplayLog("Save rejected: state=%p, path=%s, buffering=%d\n", 
                  state, outputPath ? outputPath : "NULL", state ? state->isBuffering : 0);
        return FALSE;
    }
    
    // Check state machine - must be in CAPTURING state
    LONG currentState = InterlockedCompareExchange(&state->state, REPLAY_STATE_CAPTURING, REPLAY_STATE_CAPTURING);
    if (currentState != REPLAY_STATE_CAPTURING) {
        ReplayLog("Save rejected: state=%d (expected CAPTURING=%d)\n", 
                  currentState, REPLAY_STATE_CAPTURING);
        return FALSE;
    }
    
    // Check minimum frames requirement
    LONG frames = InterlockedCompareExchange(&state->framesCaptured, 0, 0);
    if (frames < MIN_FRAMES_FOR_SAVE) {
        ReplayLog("Save rejected: only %d frames captured (need %d)\n", frames, MIN_FRAMES_FOR_SAVE);
        return FALSE;
    }
    
    // Set up save parameters
    strncpy(state->savePath, outputPath, MAX_PATH - 1);
    state->savePath[MAX_PATH - 1] = '\0';
    state->saveSuccess = FALSE;
    
    // Signal save request via event (proper synchronization)
    ResetEvent(state->hSaveCompleteEvent);
    SetEvent(state->hSaveRequestEvent);
    
    // Wait for completion (max 30 sec for muxing large buffers)
    DWORD waitResult = WaitForSingleObject(state->hSaveCompleteEvent, 30000);
    
    if (waitResult != WAIT_OBJECT_0) {
        ReplayLog("Save timeout after 30 seconds\n");
        return FALSE;
    }
    
    return state->saveSuccess;
}

void ReplayBuffer_GetStatus(ReplayBufferState* state, char* buffer, int bufferSize) {
    /* Preconditions */
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(buffer != NULL);
    LWSR_ASSERT(bufferSize > 0);
    
    if (!state || !buffer || bufferSize < 1) return;
    
    if (state->isBuffering) {
        ReplayVideoState* video = &g_internal.video;
        double duration = SampleBuffer_GetDuration(&video->sampleBuffer);
        size_t memMB = SampleBuffer_GetMemoryUsage(&video->sampleBuffer) / (1024 * 1024);
        snprintf(buffer, bufferSize, "Replay: %.0fs (%zuMB)", duration, memMB);
    } else {
        strncpy(buffer, "Replay: OFF", bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
    }
}

int ReplayBuffer_EstimateRAMUsage(int durationSec, int w, int h, int fps) {
    /* Preconditions */
    LWSR_ASSERT(durationSec > 0);
    LWSR_ASSERT(w > 0);
    LWSR_ASSERT(h > 0);
    LWSR_ASSERT(fps > 0);
    
    /* Estimate based on bitrate
     * At 90 Mbps, 60 sec = 90 * 60 / 8 = 675 MB */
    float baseMbps = 75.0f;
    float megapixels = (float)((size_t)w * (size_t)h) / 1000000.0f;
    float resScale = megapixels / 3.7f;
    if (resScale < 0.5f) resScale = 0.5f;
    if (resScale > 2.5f) resScale = 2.5f;
    float fpsScale = (float)fps / 60.0f;
    if (fpsScale < 0.5f) fpsScale = 0.5f;
    if (fpsScale > 2.0f) fpsScale = 2.0f;
    
    float mbps = baseMbps * resScale * fpsScale;
    float totalMB = (mbps * durationSec) / 8.0f;
    
    return (int)totalMB;
}

/* ============================================================================
 * CAPTURE THREAD
 * ============================================================================ */

static DWORD WINAPI BufferThreadProc(LPVOID param) {
    ReplayBufferState* state = (ReplayBufferState*)param;
    if (!state) return 1;
    
    /* Reset internal state at start of each run to prevent stale state
     * NOTE: We only zero the video state, not audio state.
     * The audio critical section was initialized in ReplayBuffer_Init
     * and CRITICAL_SECTION cannot be copied by value - it must remain in place. */
    ZeroMemory(&g_internal.video, sizeof(g_internal.video));
    /* Audio state (including its samples array) is cleaned up by ReplayBuffer_Start
     * before creating this thread, so we don't need to touch it here */
    
    ReplayLog("BufferThread started (ShadowPlay RAM mode)\n");
    
    ReplayLog("Config: replayEnabled=%d, duration=%d, captureSource=%d, monitorIndex=%d\n",
              g_config.replayEnabled, g_config.replayDuration, 
              g_config.replayCaptureSource, g_config.replayMonitorIndex);
              
    ReplayLog("Config: replayFPS=%d, replayAspectRatio=%d, quality=%d\n",
              g_config.replayFPS, g_config.replayAspectRatio, g_config.quality);
    
    // Setup capture
    CaptureState* capture = &g_capture;
    RECT rect = {0};
    
    if (state->captureSource == MODE_ALL_MONITORS) {
        Capture_GetAllMonitorsBounds(&rect);
        Capture_SetAllMonitors(capture);
    } else {
        if (!Capture_GetMonitorBoundsByIndex(state->monitorIndex, &rect)) {
            POINT pt = {0, 0};
            Capture_GetMonitorFromPoint(pt, &rect, NULL);
        }
        Capture_SetMonitor(capture, state->monitorIndex);
    }
    
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    
    ReplayLog("Raw monitor bounds: %dx%d (rect: %d,%d,%d,%d)\n", 
              width, height, rect.left, rect.top, rect.right, rect.bottom);
    
    // Apply aspect ratio adjustment if set
    if (g_config.replayAspectRatio > 0) {
        int ratioW = 0, ratioH = 0;
        Util_GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
        
        if (ratioW > 0 && ratioH > 0) {
            int oldW = width, oldH = height;
            
            // Use utility to calculate aspect-corrected rect
            rect = Util_CalculateAspectRect(rect, ratioW, ratioH);
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            
            ReplayLog("Aspect ratio %d:%d applied: %dx%d -> %dx%d\n",
                      ratioW, ratioH, oldW, oldH, width, height);
        }
    }
    
    if (width <= 0 || height <= 0) {
        ReplayLog("Invalid capture size: %dx%d\n", width, height);
        return 1;
    }
    
    // Update capture to use cropped region
    if (!Capture_SetRegion(capture, rect)) {
        ReplayLog("Capture_SetRegion failed - cannot capture region %d,%d,%d,%d\n",
                  rect.left, rect.top, rect.right, rect.bottom);
        return 1;
    }
    
    state->frameWidth = width;
    state->frameHeight = height;
    
    int fps = g_config.replayFPS;
    if (fps < 30) fps = 30;
    if (fps > 120) fps = 120;
    
    ReplayLog("Final capture params: %dx%d @ %d FPS, duration=%ds, quality=%d\n", 
              width, height, fps, g_config.replayDuration, g_config.quality);
    
    // Initialize GPU color converter (BGRA → NV12 on GPU)
    GPUConverter gpuConverter = {0};
    if (!GPUConverter_Init(&gpuConverter, capture->device, width, height)) {
        ReplayLog("GPUConverter_Init failed - GPU color conversion required!\n");
        return 1;
    }
    ReplayLog("GPU color converter initialized (D3D11 Video Processor)\n");
    
    /* Initialize NVENC HEVC encoder with D3D11 device (native API) */
    ReplayVideoState* video = &g_internal.video;
    ReplayLog("Creating NVENCEncoder (%dx%d @ %d fps, quality=%d)...\n", width, height, fps, g_config.quality);
    video->encoder = NVENCEncoder_Create(capture->device, width, height, fps, g_config.quality);
    if (!video->encoder) {
        ReplayLog("NVENCEncoder_Create failed - NVIDIA GPU with NVENC required!\n");
        GPUConverter_Shutdown(&gpuConverter);
        return 1;
    }
    ReplayLog("NVENC HEVC hardware encoder initialized (native API)\n");
    
    /* Extract HEVC sequence header (VPS/SPS/PPS) for MP4 muxing */
    if (NVENCEncoder_GetSequenceHeader(video->encoder, video->seqHeader, sizeof(video->seqHeader), &video->seqHeaderSize)) {
        ReplayLog("HEVC sequence header extracted (%u bytes)\n", video->seqHeaderSize);
    } else {
        ReplayLog("WARNING: Failed to get HEVC sequence header - muxing may fail!\n");
        video->seqHeaderSize = 0;
    };
    
    /* Initialize sample buffer BEFORE setting encoder callback
     * (callback needs valid buffer pointer) */
    if (!SampleBuffer_Init(&video->sampleBuffer, g_config.replayDuration, fps, 
                           width, height, g_config.quality)) {
        ReplayLog("SampleBuffer_Init failed\n");
        NVENCEncoder_Destroy(video->encoder);
        video->encoder = NULL;
        GPUConverter_Shutdown(&gpuConverter);
        return 1;
    }
    
    /* Set encoder callback to receive completed frames (async mode)
     * The output thread will call DrainCallback when frames complete */
    NVENCEncoder_SetCallback(video->encoder, DrainCallback, &video->sampleBuffer);
    
    /* Pass sequence header to sample buffer for video-only saves */
    if (video->seqHeaderSize > 0) {
        SampleBuffer_SetSequenceHeader(&video->sampleBuffer, video->seqHeader, video->seqHeaderSize);
    }
    
    ReplayLog("Sample buffer initialized (max %ds)\n", g_config.replayDuration);
    
    /* Initialize audio capture if enabled */
    ReplayAudioState* audio = &g_internal.audio;
    BOOL audioActive = FALSE;
    if (state->audioEnabled && (state->audioSource1[0] || state->audioSource2[0] || state->audioSource3[0])) {
        ReplayLog("Audio capture enabled, sources: [%s] [%s] [%s]\n",
                  state->audioSource1[0] ? state->audioSource1 : "none",
                  state->audioSource2[0] ? state->audioSource2 : "none",
                  state->audioSource3[0] ? state->audioSource3 : "none");
        
        audio->capture = AudioCapture_Create(
            state->audioSource1, state->audioVolume1,
            state->audioSource2, state->audioVolume2,
            state->audioSource3, state->audioVolume3
        );
        
        if (audio->capture) {
            audio->encoder = AACEncoder_Create();
            if (audio->encoder) {
                AACEncoder_SetCallback(audio->encoder, AudioEncoderCallback, audio);
                
                /* Set audio max duration to match video buffer (in 100-ns units) */
                audio->maxDuration = (LONGLONG)g_config.replayDuration * 10000000LL;
                ReplayLog("Audio eviction enabled: max duration = %ds\n", g_config.replayDuration);
                
                /* Get AAC config for muxer */
                AACEncoder_GetConfig(audio->encoder, &audio->configData, &audio->configSize);
                
                if (AudioCapture_Start(audio->capture)) {
                    audioActive = TRUE;
                    ReplayLog("Audio capture started successfully\n");
                } else {
                    ReplayLog("AudioCapture_Start failed\n");
                }
            } else {
                ReplayLog("AACEncoder_Create failed\n");
            }
        } else {
            ReplayLog("AudioCapture_Create failed\n");
        }
    }
    
    // Timing - use floating point for precise frame intervals
    double frameIntervalMs = 1000.0 / (double)fps;  // 16.667ms for 60fps
    ReplayLog("Frame interval: %.4f ms (target fps=%d)\n", frameIntervalMs, fps);
    
    // Request high-resolution timer (1ms precision)
    timeBeginPeriod(1);
    
    LARGE_INTEGER perfFreq, lastFrameTime, captureStartTime;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastFrameTime);
    captureStartTime = lastFrameTime;
    
    int frameCount = 0;
    int lastLogFrame = 0;
    
    // Diagnostic counters (reset each run)
    int attemptCount = 0;
    int captureNullCount = 0;
    int convertNullCount = 0;
    int encodeFailCount = 0;
    double totalCaptureMs = 0, totalConvertMs = 0, totalSubmitMs = 0;
    int timingCount = 0;
    
    // Transition to CAPTURING state and signal ready
    // (but don't signal hReadyEvent until we have frames)
    InterlockedExchange(&state->state, REPLAY_STATE_CAPTURING);
    state->bufferReady = TRUE;  // Legacy flag
    ReplayLog("Buffer thread ready, entering capture loop\n");
    
    // Build wait handle array for event-driven loop
    HANDLE waitHandles[2] = { state->hStopEvent, state->hSaveRequestEvent };
    
    while (InterlockedCompareExchange(&state->state, 0, 0) == REPLAY_STATE_CAPTURING) {
        // Heartbeat every iteration (non-blocking)
        Logger_Heartbeat(THREAD_BUFFER);
        
        // Check for stop/save events with 1ms timeout (allows frame timing)
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 1);
        
        if (waitResult == WAIT_OBJECT_0) {
            // Stop event signaled
            ReplayLog("Stop event received\n");
            break;
        }
        
        if (waitResult == WAIT_OBJECT_0 + 1) {
            /* Save request event signaled */
            double duration = SampleBuffer_GetDuration(&video->sampleBuffer);
            int count = SampleBuffer_GetCount(&video->sampleBuffer);
            
            /* Calculate actual capture stats for diagnostics */
            LARGE_INTEGER nowTime;
            QueryPerformanceCounter(&nowTime);
            double realElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            double actualFPS = (realElapsedSec > 0) ? frameCount / realElapsedSec : 0;
            
            ReplayLog("SAVE REQUEST: %d video samples (%.2fs), %d audio samples, after %.2fs real time\n", 
                      count, duration, audio->sampleCount, realElapsedSec);
            ReplayLog("  Actual capture rate: %.2f fps (target: %d fps)\n", actualFPS, fps);
            ReplayLog("  Output path: %s\n", state->savePath);
            
            /* Write buffer to file (with audio if available) */
            BOOL ok = FALSE;
            
            EnterCriticalSection(&audio->lock);
            int audioCount = audio->sampleCount;
            MuxerAudioSample* audioCopy = NULL;
            
            if (audioCount > 0 && audio->samples && audio->configData && audio->configSize > 0) {
                /* Deep copy audio samples with overflow check */
                size_t allocSize = (size_t)audioCount * sizeof(MuxerAudioSample);
                if (allocSize / sizeof(MuxerAudioSample) != (size_t)audioCount) {
                    ReplayLog("WARNING: Audio allocation overflow, skipping audio\n");
                    audioCount = 0;
                } else {
                    audioCopy = (MuxerAudioSample*)malloc(allocSize);
                }
                if (audioCopy) {
                    LONGLONG firstAudioTs = audio->samples[0].timestamp;
                    int copiedCount = 0;
                    BOOL copyFailed = FALSE;
                    
                    for (int i = 0; i < audioCount; i++) {
                        audioCopy[i].data = (BYTE*)malloc(audio->samples[i].size);
                        if (audioCopy[i].data) {
                            memcpy(audioCopy[i].data, audio->samples[i].data, audio->samples[i].size);
                            audioCopy[i].size = audio->samples[i].size;
                            audioCopy[i].timestamp = audio->samples[i].timestamp - firstAudioTs;
                            audioCopy[i].duration = audio->samples[i].duration;
                            copiedCount++;
                        } else {
                            /* malloc failed - free all previous copies and abort */
                            ReplayLog("WARNING: Audio copy malloc failed at sample %d/%d\n", i, audioCount);
                            for (int j = 0; j < i; j++) {
                                if (audioCopy[j].data) free(audioCopy[j].data);
                            }
                            free(audioCopy);
                            audioCopy = NULL;
                            copyFailed = TRUE;
                            break;
                        }
                    }
                    
                    /* Update audioCount to reflect actual copied samples */
                    if (!copyFailed) {
                        audioCount = copiedCount;
                    } else {
                        audioCount = 0;
                    }
                }
            }
            LeaveCriticalSection(&audio->lock);
            
            /* Get video samples */
            MuxerSample* videoSamples = NULL;
            int videoCount = 0;
            if (SampleBuffer_GetSamplesForMuxing(&video->sampleBuffer, &videoSamples, &videoCount)) {
                /* Build video config */
                MuxerConfig videoConfig;
                videoConfig.width = video->sampleBuffer.width;
                videoConfig.height = video->sampleBuffer.height;
                videoConfig.fps = video->sampleBuffer.fps;
                videoConfig.quality = video->sampleBuffer.quality;
                videoConfig.seqHeader = video->sampleBuffer.seqHeaderSize > 0 ? video->sampleBuffer.seqHeader : NULL;
                videoConfig.seqHeaderSize = video->sampleBuffer.seqHeaderSize;
                
                if (audioCopy && audioCount > 0) {
                    /* Mux with audio */
                    ReplayLog("  Starting save (audio+video path, %d audio samples)...\n", audioCount);
                    MuxerAudioConfig audioConfig;
                    audioConfig.sampleRate = AAC_SAMPLE_RATE;
                    audioConfig.channels = AAC_CHANNELS;
                    audioConfig.bitrate = AAC_BITRATE;
                    audioConfig.configData = audio->configData;
                    audioConfig.configSize = audio->configSize;
                    
                    ok = MP4Muxer_WriteFileWithAudio(state->savePath, 
                                                     videoSamples, videoCount, &videoConfig,
                                                     audioCopy, audioCount, &audioConfig);
                } else {
                    /* Video only */
                    ReplayLog("  Starting save (video-only path)...\n");
                    ok = MP4Muxer_WriteFile(state->savePath, videoSamples, videoCount, &videoConfig);
                }
                
                /* Free video samples */
                for (int i = 0; i < videoCount; i++) {
                    if (videoSamples[i].data) free(videoSamples[i].data);
                }
                free(videoSamples);
            } else {
                ReplayLog("  SampleBuffer_GetSamplesForMuxing failed\n");
            }
            
            /* Free audio copy */
            if (audioCopy) {
                for (int i = 0; i < audioCount; i++) {
                    if (audioCopy[i].data) free(audioCopy[i].data);
                }
                free(audioCopy);
            }
            
            ReplayLog("SAVE %s\n", ok ? "OK" : "FAILED");
            
            state->saveSuccess = ok;
            SetEvent(state->hSaveCompleteEvent);
            continue;  /* Skip frame capture this iteration */
        }
        
        /* === AUDIO CAPTURE === */
        if (audioActive && audio->capture && audio->encoder) {
            BYTE audioPcmBuf[8192];
            LONGLONG audioTs = 0;
            int audioBytes = AudioCapture_Read(audio->capture, audioPcmBuf, sizeof(audioPcmBuf), &audioTs);
            if (audioBytes > 0) {
                AACEncoder_Feed(audio->encoder, audioPcmBuf, audioBytes, audioTs);
            }
        }
        
        /* === FRAME CAPTURE (GPU PATH) === */
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        double frameElapsedMs = (double)(currentTime.QuadPart - lastFrameTime.QuadPart) * 1000.0 / perfFreq.QuadPart;
        
        if (frameElapsedMs >= frameIntervalMs) {
            // Use ideal next frame time (prevents drift accumulation)
            lastFrameTime.QuadPart += (LONGLONG)(frameIntervalMs * perfFreq.QuadPart / 1000.0);
            
            // But if we're way behind, reset to now (prevents "catch up" burst)
            if ((currentTime.QuadPart - lastFrameTime.QuadPart) * 1000.0 / perfFreq.QuadPart > frameIntervalMs * 2) {
                lastFrameTime = currentTime;
            }
            
            attemptCount++;
            
            // Calculate real wall-clock timestamp for this frame (100-ns units)
            double realElapsedSec = (double)(currentTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            UINT64 realTimestamp = (UINT64)(realElapsedSec * 10000000.0);
            
            // GPU path: capture → color convert → NVENC (all on GPU)
            
            // Pipeline timing for diagnostics
            LARGE_INTEGER t1, t2, t3, t4;
            
            if (gpuConverter.initialized && video->encoder) {
                QueryPerformanceCounter(&t1);
                ID3D11Texture2D* bgraTexture = Capture_GetFrameTexture(capture, NULL);
                QueryPerformanceCounter(&t2);
                
                if (bgraTexture) {
                    ID3D11Texture2D* nv12Texture = GPUConverter_Convert(&gpuConverter, bgraTexture);
                    QueryPerformanceCounter(&t3);
                    
                    if (nv12Texture) {
                        /* Async API: Submit frame (fast, non-blocking)
                         * Output thread will call DrainCallback when frame completes
                         * Returns: 1=success, 0=transient failure, -1=device lost */
                        int submitResult = NVENCEncoder_SubmitTexture(video->encoder, nv12Texture, realTimestamp);
                        QueryPerformanceCounter(&t4);
                        
                        if (submitResult == 1) {
                            frameCount++;  // Count submissions (frames delivered via callback)
                            encodeFailCount = 0;  // Reset consecutive failure counter
                            
                            // Update state machine frame count
                            LONG newCount = InterlockedIncrement(&state->framesCaptured);
                            
                            // Signal ready event once we have enough frames
                            if (newCount == MIN_FRAMES_FOR_SAVE) {
                                SetEvent(state->hReadyEvent);
                                ReplayLog("Minimum frames captured (%d), ready for saves\n", MIN_FRAMES_FOR_SAVE);
                            }
                            
                            // Accumulate timing stats (submit should be <1ms in async mode)
                            totalCaptureMs += (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / perfFreq.QuadPart;
                            totalConvertMs += (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / perfFreq.QuadPart;
                            totalSubmitMs += (double)(t4.QuadPart - t3.QuadPart) * 1000.0 / perfFreq.QuadPart;
                            timingCount++;
                        } else if (submitResult == -1) {
                            // Device lost - must restart the entire pipeline
                            ReplayLog("NVENC DEVICE LOST - buffer needs restart\n");
                            InterlockedExchange(&state->state, REPLAY_STATE_STALLED);
                            break;  // Exit capture loop immediately
                        } else {
                            // submitResult == 0: Transient failure
                            encodeFailCount++;
                            
                            // If NVENC fails for 5+ seconds straight, pipeline is stalled
                            // This can happen when GPU goes to sleep or device is removed
                            if (encodeFailCount >= fps * 5) {
                                ReplayLog("NVENC pipeline stalled (%d consecutive failures) - restarting buffer\n", encodeFailCount);
                                // Signal ourselves to restart by breaking out of the loop
                                // The buffer will need to be stopped and restarted by the app
                                InterlockedExchange(&state->state, REPLAY_STATE_STALLED);
                                break;  // Exit capture loop - app should restart buffer
                            }
                        }
                    } else {
                        convertNullCount++;
                    }
                } else {
                    captureNullCount++;
                    
                    // Check if access was lost (monitor sleep, resolution change, etc.)
                    if (capture->accessLost) {
                        ReplayLog("DXGI access lost detected - reinitializing duplication...\n");
                        
                        // Wait for desktop to stabilize, but check stop/save events
                        DWORD stabilizeWait = WaitForMultipleObjects(2, waitHandles, FALSE, 500);
                        if (stabilizeWait == WAIT_OBJECT_0) {
                            ReplayLog("Stop event during reinit wait\n");
                            break;  // Exit capture loop
                        }
                        if (stabilizeWait == WAIT_OBJECT_0 + 1) {
                            // Save requested during reinit - can't save with stale data
                            ReplayLog("Save requested during reinit - rejecting (access lost)\n");
                            state->saveSuccess = FALSE;
                            SetEvent(state->hSaveCompleteEvent);
                        }
                        
                        if (Capture_ReinitDuplication(capture)) {
                            ReplayLog("Duplication reinitialized successfully\n");
                            captureNullCount = 0;  // Reset counter after reinit
                        } else {
                            ReplayLog("WARNING: Failed to reinit duplication, will retry...\n");
                            // Longer wait before retry, but still check events
                            WaitForMultipleObjects(2, waitHandles, FALSE, 1000);
                        }
                    }
                }
            }
            
            // Early failure detection - if first 60 attempts all fail, log warning
            if (attemptCount == 60 && frameCount == 0) {
                ReplayLog("WARNING: First 60 capture attempts all failed! Check capture source.\n");
                ReplayLog("  capture=%d, convert=%d, encode=%d\n",
                          captureNullCount, convertNullCount, encodeFailCount);
            }
            
            // Log failures and timing periodically (every 10 seconds worth of attempts)
            if (attemptCount % (fps * 10) == 0 && attemptCount > 0) {
                if (timingCount > 0) {
                    ReplayLog("Pipeline timing (avg): capture=%.2fms, convert=%.2fms, submit=%.2fms, total=%.2fms\n",
                              totalCaptureMs / timingCount, totalConvertMs / timingCount, 
                              totalSubmitMs / timingCount,
                              (totalCaptureMs + totalConvertMs + totalSubmitMs) / timingCount);
                }
                if (captureNullCount + convertNullCount + encodeFailCount > 0) {
                    ReplayLog("Frame stats: attempts=%d, success=%d, failures: capture=%d, convert=%d, encode=%d\n",
                              attemptCount, frameCount, captureNullCount, convertNullCount, encodeFailCount);
                }
            }
            
            /* Periodic log with actual FPS calculation */
            if (frameCount - lastLogFrame >= fps * 5) {
                LARGE_INTEGER nowTime;
                QueryPerformanceCounter(&nowTime);
                double logElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
                double actualFPS = frameCount / logElapsedSec;
                double attemptFPS = attemptCount / logElapsedSec;
                
                /* Get encoder stats */
                int encFrames = 0;
                double avgEncMs = 0;
                NVENCEncoder_GetStats(video->encoder, &encFrames, &avgEncMs);
                
                double duration = SampleBuffer_GetDuration(&video->sampleBuffer);
                int bufCount = SampleBuffer_GetCount(&video->sampleBuffer);
                size_t memMB = SampleBuffer_GetMemoryUsage(&video->sampleBuffer) / (1024 * 1024);
                size_t memKB = SampleBuffer_GetMemoryUsage(&video->sampleBuffer) / 1024;
                int avgKBPerFrame = bufCount > 0 ? (int)(memKB / bufCount) : 0;
                ReplayLog("Status: %d/%d frames in %.1fs (encode=%.1f fps, attempt=%.1f fps, target=%d fps), buffer=%.1fs (%d samples, %zu MB, %d KB/frame)\n", 
                          frameCount, attemptCount, realElapsedSec, actualFPS, attemptFPS, fps, duration, bufCount, memMB, avgKBPerFrame);
                
                /* Log failure breakdown if any */
                if (captureNullCount + convertNullCount + encodeFailCount > 0) {
                    ReplayLog("  Failures: capture=%d, convert=%d, encode=%d\n",
                              captureNullCount, convertNullCount, encodeFailCount);
                }
                
                lastLogFrame = frameCount;
            }
        }
        /* No Sleep() needed - WaitForMultipleObjects provides timing */
    }
    
    /* Cleanup */
    ReplayLog("Shutting down (state=%d)...\n", InterlockedCompareExchange(&state->state, 0, 0));
    
    /* Restore timer resolution */
    timeEndPeriod(1);
    
    /* Shutdown GPU converter */
    GPUConverter_Shutdown(&gpuConverter);
    
    /* Stop audio capture */
    if (audioActive) {
        if (audio->capture) {
            AudioCapture_Stop(audio->capture);
            AudioCapture_Destroy(audio->capture);
            audio->capture = NULL;
        }
        if (audio->encoder) {
            AACEncoder_Destroy(audio->encoder);
            audio->encoder = NULL;
        }
        ReplayLog("Audio capture stopped\n");
    }
    
    /* Flush encoder */
    if (video->encoder) {
        EncodedFrame flushed = {0};
        while (NVENCEncoder_Flush(video->encoder, &flushed)) {
            SampleBuffer_Add(&video->sampleBuffer, &flushed);
        }
        
        NVENCEncoder_Destroy(video->encoder);
        video->encoder = NULL;
    }
    SampleBuffer_Shutdown(&video->sampleBuffer);
    
    ReplayLog("BufferThread exit\n");
    return 0;
}

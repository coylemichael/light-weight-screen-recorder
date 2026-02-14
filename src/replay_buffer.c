/*
 * replay_buffer.c - Instant Replay (ShadowPlay-style)
 * 
 * USES: nvenc_encoder.c, gpu_converter.c, frame_buffer.c, mp4_muxer.c (batch)
 * 
 * Continuously captures to RAM-based circular buffer of encoded HEVC frames.
 * On save: muxes buffered frames to MP4 (no re-encoding needed).
 * Pipeline: DXGI capture → GPU color convert → NVENC → FrameBuffer ring
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation/precondition checks
 * - HRESULT checks use FAILED()/SUCCEEDED() macros exclusively
 * - Thread-safe state checks using InterlockedCompareExchange
 * - All errors logged; non-critical errors allow graceful degradation
 * - Returns BOOL to propagate errors; callers must check
 *
 * THREAD HANG BEHAVIOR:
 * If the buffer thread hangs (NVENC/D3D11 bad state, GPU sleep, driver crash),
 * we log the hang and leak resources safely. TerminateThread was attempted but
 * cannot safely kill NVENC threads from outside - the approach doesn't work.
 */

#include "replay_buffer.h"
#include "nvenc_encoder.h"
#include "frame_buffer.h"
#include "capture.h"
#include "config.h"
#include "util.h"
#include "logger.h"
#include "audio_capture.h"
#include "aac_encoder.h"
#include "mp4_muxer.h"
#include "gpu_converter.h"
#include "constants.h"
#include "leak_tracker.h"
#include "mem_utils.h"
#include <mmsystem.h>  /* For timeBeginPeriod/timeEndPeriod */

#pragma comment(lib, "winmm.lib")

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
    FrameBuffer frameBuffer;            /* Circular buffer of encoded frames */
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
    FrameBuffer* buffer = (FrameBuffer*)userData;
    if (frame && frame->data && buffer) {
        FrameBuffer_Add(buffer, frame);
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
                LEAK_TRACK_AAC_SAMPLE_FREE();
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
                    LEAK_TRACK_AAC_SAMPLE_FREE();
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
            LEAK_TRACK_AAC_SAMPLE_ALLOC();
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

/*
 * MULTI-RESOURCE FUNCTION: ReplayBuffer_Init
 * Resources: 4 event handles + 1 critical section
 * Pattern: goto-cleanup with SAFE_CLOSE_HANDLE
 * Init: ZeroMemory ensures NULL initialization
 */
BOOL ReplayBuffer_Init(ReplayBufferState* state) {
    /* Precondition */
    LWSR_ASSERT(state != NULL);
    
    if (!state) return FALSE;
    ZeroMemory(state, sizeof(ReplayBufferState));
    
    /* Initialize internal state structure */
    ZeroMemory(&g_internal, sizeof(g_internal));
    
    /* Create synchronization events */
    state->hReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  /* Manual reset */
    if (!state->hReadyEvent) goto cleanup;
    
    state->hSaveRequestEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  /* Auto reset */
    if (!state->hSaveRequestEvent) goto cleanup;
    
    state->hSaveCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  /* Auto reset */
    if (!state->hSaveCompleteEvent) goto cleanup;
    
    state->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  /* Manual reset */
    if (!state->hStopEvent) goto cleanup;
    
    /* Initialize audio critical section */
    state->state = REPLAY_STATE_UNINITIALIZED;
    InitializeCriticalSection(&g_internal.audio.lock);
    g_internal.audio.lockInitialized = TRUE;
    return TRUE;
    
cleanup:
    ReplayLog("Failed to create synchronization events\n");
    SAFE_CLOSE_HANDLE(state->hReadyEvent);
    SAFE_CLOSE_HANDLE(state->hSaveRequestEvent);
    SAFE_CLOSE_HANDLE(state->hSaveCompleteEvent);
    SAFE_CLOSE_HANDLE(state->hStopEvent);
    return FALSE;
}

void ReplayBuffer_Shutdown(ReplayBufferState* state) {
    if (!state) return;
    ReplayBuffer_Stop(state);
    
    /* Close event handles - use SAFE_CLOSE_HANDLE for NULL-check and NULL-set */
    SAFE_CLOSE_HANDLE(state->hReadyEvent);
    SAFE_CLOSE_HANDLE(state->hSaveRequestEvent);
    SAFE_CLOSE_HANDLE(state->hSaveCompleteEvent);
    SAFE_CLOSE_HANDLE(state->hStopEvent);
    
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
    
    /* Validate duration */
    if (state->durationSeconds <= 0) {
        ReplayLog("ReplayBuffer_Start: Invalid duration %d seconds\n", state->durationSeconds);
        return FALSE;
    }
    
    /* Reset state machine */
    InterlockedExchange(&state->state, REPLAY_STATE_STARTING);
    InterlockedExchange(&state->framesCaptured, 0);
    InterlockedExchange(&state->audioError, AAC_OK);  // Reset audio error
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
    
    /* Log final leak tracker status before shutdown */
    LeakTracker_LogStatusForced();
    
    /* Signal stop via event (proper cross-thread communication) */
    (void)InterlockedExchange(&state->state, REPLAY_STATE_STOPPING);
    SetEvent(state->hStopEvent);
    
    if (state->bufferThread) {
        DWORD waitResult = WaitForSingleObject(state->bufferThread, 5000);
        
        if (waitResult == WAIT_TIMEOUT) {
            /* Thread is hung - leak resources safely (TerminateThread doesn't work
             * for NVENC threads). The thread handle is closed below but the thread
             * itself and its resources (encoder, textures, etc.) are leaked. */
            ReplayLog("WARNING: Buffer thread hung (5s timeout), leaking resources\n");
            
            /* Mark encoder as leaked so session can be recovered later */
            ReplayVideoState* video = &g_internal.video;
            if (video->encoder) {
                NVENCEncoder_MarkLeaked(video->encoder);
                video->encoder = NULL;
            }
        }
        
        CloseHandle(state->bufferThread);
        state->bufferThread = NULL;
    }
    state->isBuffering = FALSE;
}

BOOL ReplayBuffer_SaveAsync(ReplayBufferState* state, const char* outputPath,
                            HWND notifyWindow, UINT notifyMessage) {
    // Preconditions
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(outputPath != NULL);
    
    if (!state || !outputPath || !state->isBuffering) {
        ReplayLog("SaveAsync rejected: state=%p, path=%s, buffering=%d\n", 
                  state, outputPath ? outputPath : "NULL", state ? state->isBuffering : 0);
        return FALSE;
    }
    
    // Check state machine - must be in CAPTURING state
    LONG currentState = InterlockedCompareExchange(&state->state, REPLAY_STATE_CAPTURING, REPLAY_STATE_CAPTURING);
    if (currentState != REPLAY_STATE_CAPTURING) {
        ReplayLog("SaveAsync rejected: state=%d (expected CAPTURING=%d)\n", 
                  currentState, REPLAY_STATE_CAPTURING);
        return FALSE;
    }
    
    // Check minimum frames requirement
    LONG frames = InterlockedCompareExchange(&state->framesCaptured, 0, 0);
    if (frames < MIN_FRAMES_FOR_SAVE) {
        ReplayLog("SaveAsync rejected: only %d frames captured (need %d)\n", frames, MIN_FRAMES_FOR_SAVE);
        return FALSE;
    }
    
    // Set up save parameters and notification target
    strncpy(state->savePath, outputPath, MAX_PATH - 1);
    state->savePath[MAX_PATH - 1] = '\0';
    state->saveSuccess = FALSE;
    state->notifyWindow = notifyWindow;
    state->notifyMessage = notifyMessage;
    
    // Signal save request - buffer thread will handle it and post notification when done
    SetEvent(state->hSaveRequestEvent);
    
    ReplayLog("SaveAsync: Save requested, will notify hwnd=%p msg=%u\n", 
              (void*)notifyWindow, notifyMessage);
    return TRUE;
}

int ReplayBuffer_EstimateRAMUsage(int durationSec, int w, int h, int fps, QualityPreset quality) {
    /* Preconditions */
    LWSR_ASSERT(durationSec > 0);
    LWSR_ASSERT(w > 0);
    LWSR_ASSERT(h > 0);
    LWSR_ASSERT(fps > 0);
    
    /* Get base bitrate from quality preset (same as encoder) */
    float baseMbps;
    switch (quality) {
        case QUALITY_LOW:      baseMbps = BITRATE_LOW_MBPS;      break;
        case QUALITY_MEDIUM:   baseMbps = BITRATE_MEDIUM_MBPS;   break;
        case QUALITY_HIGH:     baseMbps = BITRATE_HIGH_MBPS;     break;
        case QUALITY_LOSSLESS: baseMbps = BITRATE_LOSSLESS_MBPS; break;
        default:               baseMbps = BITRATE_MEDIUM_MBPS;   break;
    }
    
    float megapixels = (float)((size_t)w * (size_t)h) / 1000000.0f;
    float resScale = megapixels / 3.7f;
    if (resScale < 0.5f) resScale = 0.5f;
    if (resScale > 2.5f) resScale = 2.5f;
    float fpsScale = (float)fps / 60.0f;
    if (fpsScale < 0.5f) fpsScale = 0.5f;
    if (fpsScale > 4.0f) fpsScale = 4.0f;  // Support up to 240fps
    
    float mbps = baseMbps * resScale * fpsScale;
    float totalMB = (mbps * durationSec) / 8.0f;
    
    return (int)totalMB;
}

/* ============================================================================
 * BUFFER THREAD HELPER FUNCTIONS
 * ============================================================================ */

/**
 * Initialize capture region based on replay buffer configuration.
 * Sets up monitor bounds as configured.
 * 
 * @param state    Replay buffer state with capture source config
 * @param capture  Capture state to configure
 * @param outRect  Output rectangle for capture bounds
 * @return TRUE if capture region was configured successfully
 */
static BOOL InitCaptureRegion(ReplayBufferState* state, CaptureState* capture, RECT* outRect) {
    SetRectEmpty(outRect);
    
    // Configure monitor capture
    if (!Capture_GetMonitorBoundsByIndex(state->monitorIndex, outRect)) {
        POINT pt = {0, 0};
        Capture_GetMonitorFromPoint(pt, outRect, NULL);
    }
    Capture_SetMonitor(capture, state->monitorIndex);
    
    return !IsRectEmpty(outRect);
}

/**
 * Apply aspect ratio adjustment to capture rectangle.
 * Crops the capture region to match the configured aspect ratio.
 * 
 * @param rect   Rectangle to adjust (in/out)
 * @param width  Width to update (in/out)
 * @param height Height to update (in/out)
 */
static void ApplyAspectRatioAdjustment(RECT* rect, int* width, int* height) {
    if (g_config.replayAspectRatio <= 0) return;
    
    int ratioW = 0, ratioH = 0;
    Util_GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
    
    if (ratioW > 0 && ratioH > 0) {
        int oldW = *width, oldH = *height;
        
        /* Use utility to calculate aspect-corrected rect */
        *rect = Util_CalculateAspectRect(*rect, ratioW, ratioH);
        *width = rect->right - rect->left;
        *height = rect->bottom - rect->top;
        
        ReplayLog("Aspect ratio %d:%d applied: %dx%d -> %dx%d\n",
                  ratioW, ratioH, oldW, oldH, *width, *height);
    }
}

/**
 * Initialize video encoding pipeline (GPU converter + NVENC).
 * Creates the color converter and hardware encoder.
 * 
 * @param capture      Capture state with D3D11 device
 * @param video        Video state to initialize
 * @param gpuConverter GPU converter to initialize
 * @param width        Frame width
 * @param height       Frame height
 * @param fps          Target frame rate
 * @return TRUE if pipeline initialized successfully
 */
static BOOL InitVideoPipeline(CaptureState* capture, ReplayVideoState* video, 
                               GPUConverter* gpuConverter, int width, int height, int fps) {
    /* Initialize GPU color converter (BGRA → NV12 on GPU) */
    if (!GPUConverter_Init(gpuConverter, capture->device, width, height)) {
        ReplayLog("GPUConverter_Init failed - GPU color conversion required!\n");
        return FALSE;
    }
    ReplayLog("GPU color converter initialized (D3D11 Video Processor)\n");
    
    /* Initialize NVENC HEVC encoder with D3D11 device (native API) */
    ReplayLog("Creating NVENCEncoder (%dx%d @ %d fps, quality=%d)...\n", 
              width, height, fps, g_config.quality);
    video->encoder = NVENCEncoder_Create(capture->device, width, height, fps, g_config.quality);
    if (!video->encoder) {
        ReplayLog("NVENCEncoder_Create failed - NVIDIA GPU with NVENC required!\n");
        GPUConverter_Shutdown(gpuConverter);
        return FALSE;
    }
    ReplayLog("NVENC HEVC hardware encoder initialized (native API)\n");
    
    /* Extract HEVC sequence header (VPS/SPS/PPS) for MP4 muxing */
    if (NVENCEncoder_GetSequenceHeader(video->encoder, video->seqHeader, 
                                        sizeof(video->seqHeader), &video->seqHeaderSize)) {
        ReplayLog("HEVC sequence header extracted (%u bytes)\n", video->seqHeaderSize);
    } else {
        ReplayLog("WARNING: Failed to get HEVC sequence header - muxing may fail!\n");
        video->seqHeaderSize = 0;
    }
    
    /* Initialize frame buffer */
    if (!FrameBuffer_Init(&video->frameBuffer, g_config.replayDuration, fps, 
                          width, height, g_config.quality)) {
        ReplayLog("FrameBuffer_Init failed\n");
        NVENCEncoder_Destroy(video->encoder);
        video->encoder = NULL;
        GPUConverter_Shutdown(gpuConverter);
        return FALSE;
    }
    
    /* Set encoder callback for async mode */
    NVENCEncoder_SetCallback(video->encoder, DrainCallback, &video->frameBuffer);
    
    /* Pass sequence header to frame buffer */
    if (video->seqHeaderSize > 0) {
        FrameBuffer_SetSequenceHeader(&video->frameBuffer, video->seqHeader, video->seqHeaderSize);
    }
    
    ReplayLog("Frame buffer initialized (max %ds)\n", g_config.replayDuration);
    return TRUE;
}

/**
 * Initialize audio capture pipeline (WASAPI + AAC encoder).
 * Creates audio capture and encoder if audio sources are configured.
 * 
 * @param state Replay buffer state with audio configuration
 * @param audio Audio state to initialize
 * @param outAudioError [out] Receives AAC encoder error code if encoder fails (may be NULL)
 * @return TRUE if audio is active and ready
 */
static BOOL InitAudioPipeline(ReplayBufferState* state, ReplayAudioState* audio, AACEncoderError* outAudioError) {
    if (outAudioError) *outAudioError = AAC_OK;
    
    if (!state->audioEnabled) return FALSE;
    if (!state->audioSource1[0] && !state->audioSource2[0] && !state->audioSource3[0]) return FALSE;
    
    ReplayLog("Audio capture enabled, sources: [%s] [%s] [%s]\n",
              state->audioSource1[0] ? state->audioSource1 : "none",
              state->audioSource2[0] ? state->audioSource2 : "none",
              state->audioSource3[0] ? state->audioSource3 : "none");
    
    audio->capture = AudioCapture_Create(
        state->audioSource1, state->audioVolume1,
        state->audioSource2, state->audioVolume2,
        state->audioSource3, state->audioVolume3
    );
    
    if (!audio->capture) {
        ReplayLog("AudioCapture_Create failed\n");
        return FALSE;
    }
    
    AACEncoderError aacErr = AAC_OK;
    audio->encoder = AACEncoder_CreateEx(&aacErr);
    if (!audio->encoder) {
        ReplayLog("AACEncoder_Create failed (error=%d)\n", (int)aacErr);
        if (outAudioError) *outAudioError = aacErr;
        AudioCapture_Destroy(audio->capture);
        audio->capture = NULL;
        return FALSE;
    }
    
    AACEncoder_SetCallback(audio->encoder, AudioEncoderCallback, audio);
    
    /* Set audio max duration to match video buffer (in 100-ns units) */
    audio->maxDuration = (LONGLONG)g_config.replayDuration * 10000000LL;
    ReplayLog("Audio eviction enabled: max duration = %ds\n", g_config.replayDuration);
    
    /* Get AAC config for muxer */
    AACEncoder_GetConfig(audio->encoder, &audio->configData, &audio->configSize);
    
    if (!AudioCapture_Start(audio->capture)) {
        ReplayLog("AudioCapture_Start failed\n");
        AACEncoder_Destroy(audio->encoder);
        AudioCapture_Destroy(audio->capture);
        audio->encoder = NULL;
        audio->capture = NULL;
        return FALSE;
    }
    
    ReplayLog("Audio capture started successfully\n");
    return TRUE;
}

/**
 * Shutdown audio pipeline and release resources.
 * 
 * @param audio Audio state to shut down
 */
static void ShutdownAudioPipeline(ReplayAudioState* audio) {
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

/**
 * Shutdown video pipeline and release resources.
 * 
 * @param video        Video state to shut down
 * @param gpuConverter GPU converter to shut down
 */
static void ShutdownVideoPipeline(ReplayVideoState* video, GPUConverter* gpuConverter) {
    GPUConverter_Shutdown(gpuConverter);
    
    if (video->encoder) {
        /* Flush remaining frames */
        EncodedFrame flushed = {0};
        while (NVENCEncoder_Flush(video->encoder, &flushed)) {
            FrameBuffer_Add(&video->frameBuffer, &flushed);
        }
        
        NVENCEncoder_Destroy(video->encoder);
        video->encoder = NULL;
    }
    FrameBuffer_Shutdown(&video->frameBuffer);
}

/**
 * Deep copy audio samples for muxing.
 * Creates independent copies of audio data that can be freed after muxing.
 * 
 * @param audio      Audio state to copy from (locked externally)
 * @param outCopy    Output array pointer
 * @param outCount   Output sample count
 * @return TRUE if copy succeeded (may be 0 samples if none available)
 */
static BOOL CopyAudioSamplesForMuxing(ReplayAudioState* audio, MuxerAudioSample** outCopy, int* outCount) {
    *outCopy = NULL;
    *outCount = 0;
    
    if (audio->sampleCount <= 0 || !audio->samples || 
        !audio->configData || audio->configSize <= 0) {
        return TRUE;  /* No audio - not an error */
    }
    
    int audioCount = audio->sampleCount;
    
    /* Overflow check */
    size_t allocSize = (size_t)audioCount * sizeof(MuxerAudioSample);
    if (allocSize / sizeof(MuxerAudioSample) != (size_t)audioCount) {
        ReplayLog("WARNING: Audio allocation overflow, skipping audio\n");
        return TRUE;
    }
    
    MuxerAudioSample* audioCopy = (MuxerAudioSample*)malloc(allocSize);
    if (!audioCopy) return TRUE;
    
    /* Zero-initialize to ensure safe cleanup on partial allocation failure */
    memset(audioCopy, 0, allocSize);
    
    LONGLONG firstAudioTs = audio->samples[0].timestamp;
    int copiedCount = 0;
    
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
            return TRUE;  /* Continue without audio */
        }
    }
    
    *outCopy = audioCopy;
    *outCount = copiedCount;
    return TRUE;
}

/**
 * Free audio sample copies created by CopyAudioSamplesForMuxing.
 * 
 * @param samples   Array to free
 * @param count     Number of samples
 */
static void FreeAudioSampleCopies(MuxerAudioSample* samples, int count) {
    if (!samples) return;
    for (int i = 0; i < count; i++) {
        if (samples[i].data) free(samples[i].data);
    }
    free(samples);
}

/**
 * Handle save request - muxes buffered video and audio to file.
 * Called when save event is signaled during capture loop.
 * 
 * @param state      Replay buffer state with output path
 * @param video      Video state with sample buffer
 * @param audio      Audio state with samples
 * @return TRUE if save succeeded
 */
static BOOL HandleSaveRequest(ReplayBufferState* state, ReplayVideoState* video, 
                               ReplayAudioState* audio) {
    BOOL ok = FALSE;
    MuxerAudioSample* audioCopy = NULL;
    int audioCount = 0;
    
    /* Copy audio samples while holding lock */
    EnterCriticalSection(&audio->lock);
    CopyAudioSamplesForMuxing(audio, &audioCopy, &audioCount);
    LeaveCriticalSection(&audio->lock);
    
    /* Get video samples */
    MuxerSample* videoSamples = NULL;
    int videoCount = 0;
    
    if (!FrameBuffer_GetFramesForMuxing(&video->frameBuffer, &videoSamples, &videoCount)) {
        ReplayLog("  FrameBuffer_GetFramesForMuxing failed\n");
        FreeAudioSampleCopies(audioCopy, audioCount);
        return FALSE;
    }
    
    /* Build video config */
    MuxerConfig videoConfig;
    videoConfig.width = video->frameBuffer.width;
    videoConfig.height = video->frameBuffer.height;
    videoConfig.fps = video->frameBuffer.fps;
    videoConfig.quality = video->frameBuffer.quality;
    videoConfig.seqHeader = video->frameBuffer.seqHeaderSize > 0 ? video->frameBuffer.seqHeader : NULL;
    videoConfig.seqHeaderSize = video->frameBuffer.seqHeaderSize;
    
    /* Mux to file */
    if (audioCopy && audioCount > 0) {
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
        ReplayLog("  Starting save (video-only path)...\n");
        ok = MP4Muxer_WriteFile(state->savePath, videoSamples, videoCount, &videoConfig);
    }
    
    /* Free video samples */
    for (int i = 0; i < videoCount; i++) {
        if (videoSamples[i].data) free(videoSamples[i].data);
    }
    free(videoSamples);
    
    /* Free audio copy */
    FreeAudioSampleCopies(audioCopy, audioCount);
    
    ReplayLog("SAVE %s\n", ok ? "OK" : "FAILED");
    return ok;
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
    
    /* Setup capture region */
    CaptureState* capture = &g_capture;
    RECT rect = {0};
    
    if (!InitCaptureRegion(state, capture, &rect)) {
        ReplayLog("InitCaptureRegion failed\n");
        InterlockedExchange(&state->state, REPLAY_STATE_ERROR);
        return 1;
    }
    
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    
    ReplayLog("Raw monitor bounds: %dx%d (rect: %d,%d,%d,%d)\n", 
              width, height, rect.left, rect.top, rect.right, rect.bottom);
    
    /* Apply aspect ratio adjustment if configured */
    ApplyAspectRatioAdjustment(&rect, &width, &height);
    
    if (width <= 0 || height <= 0) {
        ReplayLog("Invalid capture size: %dx%d\n", width, height);
        InterlockedExchange(&state->state, REPLAY_STATE_ERROR);
        return 1;
    }
    
    // Update capture to use cropped region
    if (!Capture_SetRegion(capture, rect)) {
        ReplayLog("Capture_SetRegion failed - cannot capture region %d,%d,%d,%d\n",
                  rect.left, rect.top, rect.right, rect.bottom);
        InterlockedExchange(&state->state, REPLAY_STATE_ERROR);
        return 1;
    }
    
    state->frameWidth = width;
    state->frameHeight = height;
    
    int fps = g_config.replayFPS;
    if (fps < MIN_FPS) fps = MIN_FPS;
    if (fps > MAX_FPS) fps = MAX_FPS;
    
    ReplayLog("Final capture params: %dx%d @ %d FPS, duration=%ds, quality=%d\n", 
              width, height, fps, g_config.replayDuration, g_config.quality);
    
    /* Initialize video encoding pipeline */
    GPUConverter gpuConverter = {0};
    ReplayVideoState* video = &g_internal.video;
    
    if (!InitVideoPipeline(capture, video, &gpuConverter, width, height, fps)) {
        InterlockedExchange(&state->state, REPLAY_STATE_ERROR);
        return 1;
    }
    
    /* Initialize audio capture if enabled */
    ReplayAudioState* audio = &g_internal.audio;
    AACEncoderError audioErr = AAC_OK;
    BOOL audioActive = InitAudioPipeline(state, audio, &audioErr);
    
    /* Store audio error for caller to check */
    if (state->audioEnabled && !audioActive && audioErr != AAC_OK) {
        InterlockedExchange(&state->audioError, (LONG)audioErr);
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
    int encodeFailCount = 0;  // Diagnostic counter only (no threshold logic)
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
            double duration = FrameBuffer_GetDuration(&video->frameBuffer);
            int count = FrameBuffer_GetCount(&video->frameBuffer);
            
            /* Calculate actual capture stats for diagnostics */
            LARGE_INTEGER nowTime = {0};
            QueryPerformanceCounter(&nowTime);
            double realElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            double actualFPS = (realElapsedSec > 0) ? frameCount / realElapsedSec : 0;
            
            ReplayLog("SAVE REQUEST: %d video samples (%.2fs), %d audio samples, after %.2fs real time\n", 
                      count, duration, audio->sampleCount, realElapsedSec);
            ReplayLog("  Actual capture rate: %.2f fps (target: %d fps)\n", actualFPS, fps);
            ReplayLog("  Output path: %s\n", state->savePath);
            
            /* Use helper function for save operation */
            state->saveSuccess = HandleSaveRequest(state, video, audio);
            
            /* Signal completion event (for sync API) */
            SetEvent(state->hSaveCompleteEvent);
            
            /* Post async notification if window was specified */
            if (state->notifyWindow && state->notifyMessage) {
                PostMessage(state->notifyWindow, state->notifyMessage, 
                           (WPARAM)state->saveSuccess, 0);
                ReplayLog("Posted save completion to hwnd=%p msg=%u success=%d\n",
                          (void*)state->notifyWindow, state->notifyMessage, state->saveSuccess);
                /* Clear notification target to prevent duplicate posts */
                state->notifyWindow = NULL;
                state->notifyMessage = 0;
            }
            
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
        LARGE_INTEGER currentTime = {0};
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
            
            // Synthetic timestamp: frameCount * interval (like OBS)
            // This produces smooth playback regardless of actual capture jitter
            // frameIntervalMs is target interval (e.g., 8.33ms for 120fps)
            // Convert to 100-ns units: ms * 10000 = 100-ns
            UINT64 syntheticTimestamp = (UINT64)(frameCount * frameIntervalMs * 10000.0);
            
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
                        int submitResult = NVENCEncoder_SubmitTexture(video->encoder, nv12Texture, syntheticTimestamp);
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
                            // Device lost - exit cleanly, HealthMonitor will detect and recover
                            ReplayLog("NVENC DEVICE LOST - exiting for HealthMonitor recovery\n");
                            break;
                        } else {
                            // submitResult == 0: Transient failure (frame dropped)
                            encodeFailCount++;  // Track for diagnostics
                            // Don't try to self-recover - HealthMonitor will detect if we get stuck
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
                LARGE_INTEGER nowTime = {0};
                QueryPerformanceCounter(&nowTime);
                double logElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
                double actualFPS = frameCount / logElapsedSec;
                double attemptFPS = attemptCount / logElapsedSec;
                
                /* Get encoder stats */
                int encFrames = 0;
                double avgEncMs = 0;
                NVENCEncoder_GetStats(video->encoder, &encFrames, &avgEncMs);
                int currentQP = NVENCEncoder_GetQP(video->encoder);
                
                /* Get frame size stats to detect quality drift */
                UINT32 lastFrameSize = 0, minFrameSize = 0, maxFrameSize = 0, avgFrameSize = 0;
                NVENCEncoder_GetFrameSizeStats(video->encoder, &lastFrameSize, &minFrameSize, &maxFrameSize, &avgFrameSize);
                
                double duration = FrameBuffer_GetDuration(&video->frameBuffer);
                int bufCount = FrameBuffer_GetCount(&video->frameBuffer);
                size_t memMB = FrameBuffer_GetMemoryUsage(&video->frameBuffer) / (1024 * 1024);
                size_t memKB = FrameBuffer_GetMemoryUsage(&video->frameBuffer) / 1024;
                int avgKBPerFrame = bufCount > 0 ? (int)(memKB / bufCount) : 0;
                ReplayLog("Status: %d/%d frames in %.1fs (encode=%.1f fps, attempt=%.1f fps, target=%d fps), buffer=%.1fs (%d samples, %zu MB, %d KB/frame, QP=%d)\n", 
                          frameCount, attemptCount, logElapsedSec, actualFPS, attemptFPS, fps, duration, bufCount, memMB, avgKBPerFrame, currentQP);
                
                /* Log leak tracker status if enabled (rate-limited internally) */
                LeakTracker_LogStatus();
                ReplayLog("  Frame sizes: last=%u, min=%u, max=%u, avg=%u bytes\n",
                          lastFrameSize, minFrameSize, maxFrameSize, avgFrameSize);
                
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
    
    /* Shutdown pipelines using helper functions */
    if (audioActive) {
        ShutdownAudioPipeline(audio);
    }
    ShutdownVideoPipeline(video, &gpuConverter);
    
    ReplayLog("BufferThread exit\n");
    return 0;
}

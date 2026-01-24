/*
 * app_context.h - Centralized Application State Management
 * ============================================================================
 * 
 * PURPOSE:
 * This header consolidates related global state that was previously scattered
 * across multiple source files into coherent context structures. This improves:
 * - Thread safety documentation
 * - State lifetime management
 * - Testability (contexts can be passed as parameters)
 * - Code clarity about which state is related
 *
 * ARCHITECTURE:
 * The application uses several context structures:
 * 
 * 1. AppContext - Top-level application state
 *    - Owns configuration, capture state, and replay buffer
 *    - Initialized once at startup, lives until shutdown
 *    - Thread access: Main thread owns, others read config
 *
 * 2. RecordingContext - Active recording session state
 *    - Created when recording starts, destroyed when it ends
 *    - Contains encoder, audio, timing for one recording
 *    - Thread access: Recording thread owns
 *
 * 3. Module-specific contexts (already exist):
 *    - CaptureState: DXGI desktop duplication resources
 *    - ReplayBufferState: Circular buffer and events
 *    - EncoderState: Media Foundation encoder session
 *    - AudioCaptureContext: WASAPI capture resources
 *
 * THREAD SAFETY DOCUMENTATION:
 * Each field documents its thread access pattern:
 * - [Main]: Only accessed from main/UI thread
 * - [Any]: Can be accessed from any thread (typically atomic)
 * - [Recording]: Only accessed from recording thread
 * - [Locked]: Protected by a specific lock
 * - [ReadOnly]: Set once during init, read-only after
 *
 * MIGRATION STRATEGY:
 * This file is introduced to document existing patterns and provide
 * a path forward. Existing code continues to work with extern globals.
 * New code should prefer passing contexts as parameters.
 */

#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <windows.h>
#include "config.h"
#include "capture.h"
#include "replay_buffer.h"
#include "encoder.h"

/* ============================================================================
 * RECORDING CONTEXT - Per-recording session state
 * ============================================================================
 * Created at recording start, destroyed at recording end.
 * Consolidates what was previously separate globals in overlay.c/replay_buffer.c
 */
typedef struct RecordingContext {
    /* Encoder for traditional recording (not replay buffer) */
    EncoderState encoder;           /* [Recording] MF sink writer */
    
    /* Recording control */
    volatile LONG stopRequested;    /* [Any] Atomic flag to stop recording */
    
    /* Timing */
    ULONGLONG startTimeTicks;       /* [Recording] GetTickCount64 at start */
    UINT64 frameCount;              /* [Recording] Frames encoded */
    
    /* Output path */
    char outputPath[MAX_PATH];      /* [Recording] Where to save */
    
} RecordingContext;

/* ============================================================================
 * REPLAY AUDIO CONTEXT - Audio state for replay buffer
 * ============================================================================
 * Groups audio-related globals from replay_buffer.c that logically belong together.
 * Protected by g_audioLock critical section.
 */
typedef struct ReplayAudioContext {
    /* Capture and encoding */
    void* audioCapture;             /* AudioCaptureContext* */
    void* aacEncoder;               /* AACEncoder* */
    
    /* Buffered AAC samples for muxing */
    void* samples;                  /* MuxerAudioSample* array */
    int sampleCount;                /* Current number of samples */
    int sampleCapacity;             /* Allocated capacity */
    
    /* AAC decoder config for MP4 */
    BYTE* configData;               /* AudioSpecificConfig bytes */
    int configSize;                 /* Size of configData */
    
    /* Eviction timing */
    LONGLONG maxDuration;           /* Max buffer duration in 100ns units */
    
    /* Thread safety */
    CRITICAL_SECTION lock;          /* Protects all above fields */
    BOOL lockInitialized;           /* Track if CS initialized */
    
} ReplayAudioContext;

/* ============================================================================
 * REPLAY VIDEO CONTEXT - Video state for replay buffer  
 * ============================================================================
 * Groups video-related globals from replay_buffer.c.
 */
typedef struct ReplayVideoContext {
    /* NVENC encoder */
    void* encoder;                  /* NVENCEncoder* */
    
    /* Frame buffer (circular buffer of encoded frames) */
    void* frameBuffer;              /* FrameBuffer* */
    
    /* HEVC sequence headers */
    BYTE seqHeader[256];            /* VPS/SPS/PPS data */
    DWORD seqHeaderSize;            /* Size of seqHeader */
    
} ReplayVideoContext;

/* ============================================================================
 * FORWARD COMPATIBILITY: Future unified context
 * ============================================================================
 * This struct shows the target architecture where all app state is owned
 * by a single context. Currently, code uses the existing extern globals,
 * but new features should aim to use passed contexts.
 */
typedef struct AppContext {
    /* Configuration - loaded at startup */
    AppConfig config;               /* [Main writes, Any reads] */
    
    /* Capture system - DXGI desktop duplication */
    CaptureState capture;           /* [Main/Recording] */
    
    /* Replay buffer - background instant replay */
    ReplayBufferState replayBuffer; /* [Main/BufferThread via events] */
    
    /* Application state */
    volatile LONG isRecording;      /* [Any] Atomic recording flag */
    volatile LONG isSelecting;      /* [Any] Atomic selection mode flag */
    
    /* Windows */
    HWND overlayWnd;                /* [Main] Overlay window handle */
    HWND controlWnd;                /* [Main] Control panel window */
    
    /* Single instance */
    HANDLE mutex;                   /* [Main] Single instance mutex */
    
    /* Debug mode */
    BOOL debugMode;                 /* [ReadOnly] --debug CLI flag */
    
} AppContext;

/* ============================================================================
 * EXISTING EXTERN GLOBALS (for backward compatibility)
 * ============================================================================
 * These are the existing globals defined in main.c. Code can continue to
 * use these, but should document thread access in comments.
 */

/* Defined in main.c */
extern AppConfig g_config;          /* [Main writes, Any reads] Config from INI */
extern CaptureState g_capture;      /* [Main/Recording] DXGI capture state */
extern ReplayBufferState g_replayBuffer; /* [Main/BufferThread] Replay state */
extern volatile LONG g_isRecording; /* [Any] TRUE while recording */
extern volatile LONG g_isSelecting; /* [Any] TRUE during area selection */
extern HWND g_overlayWnd;           /* [Main] Overlay window */
extern HWND g_controlWnd;           /* [Main] Control window */
extern HANDLE g_mutex;              /* [Main] Single instance mutex */

#endif /* APP_CONTEXT_H */

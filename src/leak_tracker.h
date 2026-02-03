/*
 * leak_tracker.h - Runtime-Controlled Allocation Counter Tracking
 * ============================================================================
 * 
 * Provides lightweight allocation/free counters for detecting memory leaks
 * during long recording sessions. Unlike heavy-weight memory profilers,
 * these counters have minimal overhead (~10ns per operation when enabled,
 * ~1ns branch when disabled).
 * 
 * USAGE:
 *   - Enabled automatically when g_config.debugLogging is set
 *   - Counters track alloc/free balance for each category
 *   - If (allocs - frees) grows unbounded over time → leak detected
 *   - Call LeakTracker_LogStatus() periodically to log current state
 * 
 * CATEGORIES TRACKED:
 *   - nvencFrame: Encoded video frames from NVENC output thread
 *   - aacSample: Encoded AAC audio samples
 *   - frameBuffer: Video frames stored in replay buffer
 *   - mfSample: Media Foundation IMFSample objects
 *   - mfBuffer: Media Foundation IMFMediaBuffer objects
 * 
 * THREAD SAFETY:
 *   All counters use InterlockedIncrement for thread-safe updates.
 *   Can be called from any thread without synchronization.
 */

#ifndef LEAK_TRACKER_H
#define LEAK_TRACKER_H

#include <windows.h>
#include "config.h"      /* For AppConfig struct definition */
#include "app_context.h" /* For extern AppConfig g_config declaration */

/* ============================================================================
 * COUNTER STRUCTURE
 * ============================================================================
 * All counters are volatile LONG for use with Interlocked* functions.
 * Grouped by subsystem for easy interpretation.
 */

typedef struct LeakCounters {
    /* NVENC Video Encoder */
    volatile LONG nvencFrameAllocs;     /* Frames allocated in output thread */
    volatile LONG nvencFrameFrees;      /* Frames freed (callback took ownership or explicit free) */
    
    /* AAC Audio Encoder */
    volatile LONG aacSampleAllocs;      /* AAC samples allocated */
    volatile LONG aacSampleFrees;       /* AAC samples freed during eviction */
    
    /* Frame Buffer (Replay) */
    volatile LONG frameBufferAllocs;    /* Frame data allocations */
    volatile LONG frameBufferFrees;     /* Frame data frees (eviction/shutdown) */
    
    /* Media Foundation (encoder.c) */
    volatile LONG mfSampleCreates;      /* MFCreateSample calls */
    volatile LONG mfSampleReleases;     /* IMFSample::Release calls */
    volatile LONG mfBufferCreates;      /* MFCreateMemoryBuffer calls */
    volatile LONG mfBufferReleases;     /* IMFMediaBuffer::Release calls */
    
    /* Timing for periodic reports */
    DWORD lastReportTime;               /* GetTickCount of last report */
} LeakCounters;

/* Global counter instance - defined in leak_tracker.c */
extern LeakCounters g_leakCounters;

/* ============================================================================
 * TRACKING MACROS
 * ============================================================================
 * These macros check g_config.debugLogging at runtime.
 * When disabled, cost is just a single predictable branch (~1ns).
 * When enabled, adds InterlockedIncrement (~10ns).
 * 
 * Note: g_config is declared extern in config.h which is included above.
 */

#define LEAK_TRACK_NVENC_FRAME_ALLOC() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.nvencFrameAllocs); } while(0)

#define LEAK_TRACK_NVENC_FRAME_FREE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.nvencFrameFrees); } while(0)

#define LEAK_TRACK_AAC_SAMPLE_ALLOC() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.aacSampleAllocs); } while(0)

#define LEAK_TRACK_AAC_SAMPLE_FREE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.aacSampleFrees); } while(0)

#define LEAK_TRACK_FRAME_BUFFER_ALLOC() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.frameBufferAllocs); } while(0)

#define LEAK_TRACK_FRAME_BUFFER_FREE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.frameBufferFrees); } while(0)

#define LEAK_TRACK_MF_SAMPLE_CREATE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.mfSampleCreates); } while(0)

#define LEAK_TRACK_MF_SAMPLE_RELEASE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.mfSampleReleases); } while(0)

#define LEAK_TRACK_MF_BUFFER_CREATE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.mfBufferCreates); } while(0)

#define LEAK_TRACK_MF_BUFFER_RELEASE() \
    do { if (g_config.debugLogging) InterlockedIncrement(&g_leakCounters.mfBufferReleases); } while(0)

/* ============================================================================
 * API FUNCTIONS
 * ============================================================================ */

/**
 * Initialize leak tracking counters to zero.
 * Call once at application startup.
 */
void LeakTracker_Init(void);

/**
 * Log current counter status if tracking is enabled.
 * Rate-limited to once per LEAK_REPORT_INTERVAL_MS (default 60 seconds).
 * Safe to call frequently - will no-op if interval hasn't elapsed.
 */
void LeakTracker_LogStatus(void);

/**
 * Force log counter status regardless of interval.
 * Useful at recording stop/shutdown.
 */
void LeakTracker_LogStatusForced(void);

/* Report interval in milliseconds (default: 60 seconds) */
#define LEAK_REPORT_INTERVAL_MS 60000

#endif /* LEAK_TRACKER_H */

/*
 * leak_tracker.c - Runtime-Controlled Allocation Counter Implementation
 * 
 * See leak_tracker.h for documentation.
 */

#include "leak_tracker.h"
#include "config.h"
#include "logger.h"
#include <string.h>

/* Global counter instance */
LeakCounters g_leakCounters = {0};

void LeakTracker_Init(void) {
    ZeroMemory(&g_leakCounters, sizeof(g_leakCounters));
    g_leakCounters.lastReportTime = GetTickCount();
}

void LeakTracker_LogStatusForced(void) {
    if (!g_config.debugLogging) return;
    
    /* Read all counters (non-atomic reads are fine for logging) */
    LONG nvencAlloc = g_leakCounters.nvencFrameAllocs;
    LONG nvencFree = g_leakCounters.nvencFrameFrees;
    LONG aacAlloc = g_leakCounters.aacSampleAllocs;
    LONG aacFree = g_leakCounters.aacSampleFrees;
    LONG fbAlloc = g_leakCounters.frameBufferAllocs;
    LONG fbFree = g_leakCounters.frameBufferFrees;
    LONG mfSampleCreate = g_leakCounters.mfSampleCreates;
    LONG mfSampleRelease = g_leakCounters.mfSampleReleases;
    LONG mfBufCreate = g_leakCounters.mfBufferCreates;
    LONG mfBufRelease = g_leakCounters.mfBufferReleases;
    
    Logger_Log("=== LEAK TRACKER STATUS ===\n");
    Logger_Log("  NVENC frames:    alloc=%ld, free=%ld, delta=%ld\n",
               nvencAlloc, nvencFree, nvencAlloc - nvencFree);
    Logger_Log("  AAC samples:     alloc=%ld, free=%ld, delta=%ld\n",
               aacAlloc, aacFree, aacAlloc - aacFree);
    Logger_Log("  FrameBuffer:     alloc=%ld, free=%ld, delta=%ld\n",
               fbAlloc, fbFree, fbAlloc - fbFree);
    Logger_Log("  MF Samples:      create=%ld, release=%ld, delta=%ld\n",
               mfSampleCreate, mfSampleRelease, mfSampleCreate - mfSampleRelease);
    Logger_Log("  MF Buffers:      create=%ld, release=%ld, delta=%ld\n",
               mfBufCreate, mfBufRelease, mfBufCreate - mfBufRelease);
    Logger_Log("===========================\n");
    
    g_leakCounters.lastReportTime = GetTickCount();
}

void LeakTracker_LogStatus(void) {
    if (!g_config.debugLogging) return;
    
    /* Rate limit to avoid log spam */
    DWORD now = GetTickCount();
    if (now - g_leakCounters.lastReportTime < LEAK_REPORT_INTERVAL_MS) {
        return;
    }
    
    LeakTracker_LogStatusForced();
}

/*
 * Health Monitor Implementation
 * 
 * See health_monitor.h for architecture overview.
 */

#include "health_monitor.h"
#include "logger.h"
#include "replay_buffer.h"
#include "leak_tracker.h"
#include <stdio.h>

/* ============================================================================
 * MODULE STATE
 * ============================================================================ */

/* Monitor thread state */
static HANDLE g_monitorThread = NULL;
static volatile LONG g_monitorRunning = FALSE;
static volatile LONG g_monitorEnabled = TRUE;
static volatile HWND g_hwndNotify = NULL;

/* Restart tracking to prevent false positives */
static volatile LONG g_restartInProgress = FALSE;
static volatile DWORD g_lastRestartTime = 0;

/* Grace period after restart before stall detection resumes (ms) */
#define RESTART_GRACE_PERIOD_MS 10000

/* ============================================================================
 * CLEANUP THREAD
 * ============================================================================
 * Fire-and-forget thread that attempts to clean up orphaned resources.
 */

typedef struct CleanupContext {
    HANDLE hungThread;          /* Handle to wait on */
    StallType stallType;        /* Which subsystem stalled */
    DWORD startTime;            /* When cleanup was scheduled */
} CleanupContext;

/* Forward declarations */
extern ReplayBufferState g_replayBuffer;  /* From main.c */

static DWORD WINAPI CleanupThread(LPVOID param) {
    CleanupContext* ctx = (CleanupContext*)param;
    if (!ctx) return 1;
    
    Logger_Log("[HealthMonitor] Cleanup thread started for stall type %d\n", ctx->stallType);
    
    /* Wait for the hung thread to potentially recover */
    if (ctx->hungThread) {
        DWORD waitResult = WaitForSingleObject(ctx->hungThread, CLEANUP_WAIT_TIMEOUT_MS);
        
        if (waitResult == WAIT_OBJECT_0) {
            /* Thread exited naturally - it cleaned up its own resources */
            Logger_Log("[HealthMonitor] Hung thread recovered/exited naturally\n");
        } else if (waitResult == WAIT_TIMEOUT) {
            /* Thread is truly stuck - attempt resource cleanup */
            Logger_Log("[HealthMonitor] Thread still hung after %d ms, attempting cleanup\n", 
                       CLEANUP_WAIT_TIMEOUT_MS);
            
            /* 
             * NOTE: At this point, the hung thread's resources (encoder, frame buffer, etc.)
             * are orphaned. We can't safely free them because:
             * 1. The thread may still be holding locks
             * 2. NVENC/D3D11 calls may be in progress
             * 3. We'd need to use TryEnterCriticalSection on each resource
             * 
             * For now, we just log the leak. A more sophisticated cleanup would:
             * - Try to acquire each lock with TryEnterCriticalSection
             * - Free resources only if lock acquired
             * - Track leaked resources for diagnostics
             */
            Logger_Log("[HealthMonitor] Resources from stalled thread are leaked (safe mode)\n");
            
            /* Reset leak tracker to account for the orphaned resources */
            /* This prevents the delta from looking like an active leak */
            Logger_Log("[HealthMonitor] Note: Leak tracker deltas may show orphaned buffer contents\n");
        } else if (waitResult == WAIT_FAILED) {
            Logger_Log("[HealthMonitor] WaitForSingleObject failed: %u\n", GetLastError());
        } else {
            Logger_Log("[HealthMonitor] Unexpected wait result: %u\n", waitResult);
        }
        
        /* Close our duplicated handle - we own it */
        CloseHandle(ctx->hungThread);
    }
    
    DWORD elapsed = GetTickCount() - ctx->startTime;
    Logger_Log("[HealthMonitor] Cleanup thread finished after %u ms\n", elapsed);
    
    free(ctx);
    return 0;
}

void HealthMonitor_ScheduleCleanup(HANDLE hungThread, StallType stallType) {
    /* If no handle provided, just log - nothing to wait on */
    if (!hungThread) {
        Logger_Log("[HealthMonitor] No thread handle for cleanup (already exited?)\n");
        return;
    }
    
    CleanupContext* ctx = (CleanupContext*)malloc(sizeof(CleanupContext));
    if (!ctx) {
        Logger_Log("[HealthMonitor] Failed to allocate cleanup context\n");
        CloseHandle(hungThread);  /* Don't leak the duplicated handle */
        return;
    }
    
    ctx->hungThread = hungThread;
    ctx->stallType = stallType;
    ctx->startTime = GetTickCount();
    
    /* Fire-and-forget thread - we don't track it */
    HANDLE hThread = CreateThread(NULL, 0, CleanupThread, ctx, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);  /* Let it run detached */
        Logger_Log("[HealthMonitor] Cleanup thread spawned\n");
    } else {
        Logger_Log("[HealthMonitor] Failed to create cleanup thread\n");
        CloseHandle(ctx->hungThread);  /* Don't leak the duplicated handle */
        free(ctx);
    }
}

/* ============================================================================
 * MONITOR THREAD
 * ============================================================================ */

static DWORD WINAPI MonitorThread(LPVOID param) {
    (void)param;
    
    Logger_Log("[HealthMonitor] Monitor thread started\n");
    Logger_Log("[HealthMonitor] Check interval: %d ms, soft threshold: %d ms, hard threshold: %d ms\n",
               HEALTH_CHECK_INTERVAL_MS, HEALTH_SOFT_THRESHOLD_MS, HEALTH_HARD_THRESHOLD_MS);
    
    /* Track if we've already warned for current stall (avoid log spam) */
    BOOL bufferWarned = FALSE;
    BOOL nvencWarned = FALSE;
    
    while (InterlockedCompareExchange(&g_monitorRunning, 0, 0)) {
        Sleep(HEALTH_CHECK_INTERVAL_MS);
        
        if (!InterlockedCompareExchange(&g_monitorRunning, 0, 0)) break;
        
        /* Send our own heartbeat */
        Logger_Heartbeat(THREAD_WATCHDOG);
        
        /* Skip checks if monitoring is disabled or restart in progress */
        if (!InterlockedCompareExchange(&g_monitorEnabled, 0, 0)) {
            continue;
        }
        
        /* Grace period after restart */
        if (InterlockedCompareExchange(&g_restartInProgress, 0, 0)) {
            DWORD elapsed = GetTickCount() - g_lastRestartTime;
            if (elapsed < RESTART_GRACE_PERIOD_MS) {
                continue;  /* Still in grace period */
            }
            InterlockedExchange(&g_restartInProgress, FALSE);
            Logger_Log("[HealthMonitor] Grace period ended, resuming monitoring\n");
        }
        
        /* Only monitor when replay buffer is actively capturing */
        if (g_replayBuffer.state != REPLAY_STATE_CAPTURING) {
            bufferWarned = FALSE;
            nvencWarned = FALSE;
            continue;
        }
        
        /* Check buffer thread heartbeat */
        DWORD bufferAge = Logger_GetHeartbeatAge(THREAD_BUFFER);
        DWORD nvencAge = Logger_GetHeartbeatAge(THREAD_NVENC_OUTPUT);
        
        /* Buffer thread stall detection */
        if (bufferAge != UINT_MAX && bufferAge > HEALTH_SOFT_THRESHOLD_MS) {
            if (!bufferWarned) {
                Logger_Log("[HealthMonitor] WARNING: Buffer thread slow (age=%u ms)\n", bufferAge);
                bufferWarned = TRUE;
            }
            
            if (bufferAge > HEALTH_HARD_THRESHOLD_MS) {
                Logger_Log("[HealthMonitor] CRITICAL: Buffer thread stalled (age=%u ms)\n", bufferAge);
            }
        } else {
            bufferWarned = FALSE;
        }
        
        /* NVENC thread stall detection */
        if (nvencAge != UINT_MAX && nvencAge > HEALTH_SOFT_THRESHOLD_MS) {
            if (!nvencWarned) {
                Logger_Log("[HealthMonitor] WARNING: NVENC thread slow (age=%u ms)\n", nvencAge);
                nvencWarned = TRUE;
            }
            
            if (nvencAge > HEALTH_HARD_THRESHOLD_MS) {
                Logger_Log("[HealthMonitor] CRITICAL: NVENC thread stalled (age=%u ms)\n", nvencAge);
            }
        } else {
            nvencWarned = FALSE;
        }
        
        /* Trigger recovery if stall persists */
        BOOL bufferStalled = (bufferAge != UINT_MAX && bufferAge > HEALTH_HARD_THRESHOLD_MS);
        BOOL nvencStalled = (nvencAge != UINT_MAX && nvencAge > HEALTH_HARD_THRESHOLD_MS);
        
        if (bufferStalled || nvencStalled) {
            StallType stallType = STALL_NONE;
            if (bufferStalled && nvencStalled) {
                stallType = STALL_MULTIPLE;
            } else if (bufferStalled) {
                stallType = STALL_BUFFER_THREAD;
            } else if (nvencStalled) {
                stallType = STALL_NVENC_THREAD;
            }
            
            Logger_Log("[HealthMonitor] Stall detected: type=%d (buffer=%s, nvenc=%s)\n",
                       stallType,
                       bufferStalled ? "STALLED" : "ok",
                       nvencStalled ? "STALLED" : "ok");
            
            /* Post message to UI for recovery */
            HWND hwnd = g_hwndNotify;  /* Local copy of volatile */
            if (hwnd) {
                if (PostMessage(hwnd, WM_WORKER_STALLED, (WPARAM)stallType, 0)) {
                    Logger_Log("[HealthMonitor] Posted WM_WORKER_STALLED to UI\n");
                } else {
                    Logger_Log("[HealthMonitor] PostMessage failed: %u\n", GetLastError());
                }
            }
            
            /* Disable monitoring until restart completes */
            InterlockedExchange(&g_monitorEnabled, FALSE);
            
            /* Reset warning flags */
            bufferWarned = FALSE;
            nvencWarned = FALSE;
        }
    }
    
    Logger_Log("[HealthMonitor] Monitor thread exiting\n");
    return 0;
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

BOOL HealthMonitor_Start(HWND hwndNotify) {
    /* Null guard at public API boundary */
    if (!hwndNotify) {
        Logger_Log("[HealthMonitor] Start failed: hwndNotify is NULL\n");
        return FALSE;
    }
    
    /* Atomic check-and-set to prevent race if called from multiple threads */
    if (InterlockedCompareExchange(&g_monitorRunning, TRUE, FALSE) != FALSE) {
        Logger_Log("[HealthMonitor] Already running\n");
        return TRUE;  /* Already running */
    }
    
    g_hwndNotify = hwndNotify;
    InterlockedExchange(&g_monitorEnabled, TRUE);
    InterlockedExchange(&g_restartInProgress, FALSE);
    
    g_monitorThread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    if (!g_monitorThread) {
        Logger_Log("[HealthMonitor] Failed to create monitor thread\n");
        InterlockedExchange(&g_monitorRunning, FALSE);
        return FALSE;
    }
    
    Logger_Log("[HealthMonitor] Started successfully\n");
    return TRUE;
}

void HealthMonitor_Stop(void) {
    if (!InterlockedCompareExchange(&g_monitorRunning, 0, 0)) {
        return;  /* Not running */
    }
    
    Logger_Log("[HealthMonitor] Stopping...\n");
    InterlockedExchange(&g_monitorRunning, FALSE);
    
    if (g_monitorThread) {
        DWORD waitResult = WaitForSingleObject(g_monitorThread, 5000);
        if (waitResult == WAIT_TIMEOUT) {
            Logger_Log("[HealthMonitor] Monitor thread did not exit in time\n");
        }
        CloseHandle(g_monitorThread);
        g_monitorThread = NULL;
    }
    
    g_hwndNotify = NULL;
    Logger_Log("[HealthMonitor] Stopped\n");
}

BOOL HealthMonitor_IsRunning(void) {
    return InterlockedCompareExchange(&g_monitorRunning, 0, 0) != 0;
}

void HealthMonitor_SetEnabled(BOOL enabled) {
    InterlockedExchange(&g_monitorEnabled, enabled ? TRUE : FALSE);
    Logger_Log("[HealthMonitor] Monitoring %s\n", enabled ? "enabled" : "disabled");
}

void HealthMonitor_NotifyRestart(void) {
    /* Order matters: set grace period BEFORE re-enabling to avoid race */
    g_lastRestartTime = GetTickCount();
    InterlockedExchange(&g_restartInProgress, TRUE);
    InterlockedExchange(&g_monitorEnabled, TRUE);  /* Re-enable after grace period */
    Logger_Log("[HealthMonitor] Restart notified, grace period started (%d ms)\n", 
               RESTART_GRACE_PERIOD_MS);
}

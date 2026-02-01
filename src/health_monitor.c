/*
 * Health Monitor Implementation
 * 
 * Architecture: See docs/HEALTHMONITOR_ARCHITECTURE_DESIGN.md
 * 
 * Key design decisions:
 * - HealthMonitor is SOLE authority for replay buffer recovery
 * - Executes recovery directly (no separate cleanup thread per Q12)
 * - Posts to UI only for COM-dependent operations (Start) and dialogs
 * - Tracks recovery count (3 in 5 min limit per Q4)
 * - Differentiates crashed vs hung threads (Q2)
 */

#include "health_monitor.h"
#include "logger.h"
#include "replay_buffer.h"
#include "nvenc_encoder.h"
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

/* Recovery tracking (Q4: 3 in 5 min limit) */
static DWORD g_recoveryTimestamps[MAX_RECOVERIES_IN_WINDOW] = {0};
static int g_recoveryIndex = 0;
static volatile LONG g_recoveryCount = 0;

/* External references */
extern ReplayBufferState g_replayBuffer;  /* From main.c */

/* ============================================================================
 * INTERNAL HELPERS
 * ============================================================================ */

/**
 * Check thread state via GetExitCodeThread (Q2, Q11)
 * Safe to call from any thread - kernel call only.
 */
static ThreadState GetThreadState(HANDLE hThread) {
    if (!hThread) return THREAD_STATE_UNKNOWN;
    
    DWORD exitCode = 0;
    if (!GetExitCodeThread(hThread, &exitCode)) {
        Logger_Log("[HealthMonitor] GetExitCodeThread failed: %u\n", GetLastError());
        return THREAD_STATE_UNKNOWN;
    }
    
    if (exitCode == STILL_ACTIVE) {
        return THREAD_STATE_RUNNING;
    } else if (exitCode == 0) {
        return THREAD_STATE_EXITED;
    } else {
        return THREAD_STATE_CRASHED;
    }
}

/**
 * Get string representation of thread state for logging.
 */
static const char* ThreadStateToString(ThreadState state) {
    switch (state) {
        case THREAD_STATE_RUNNING: return "RUNNING (hung)";
        case THREAD_STATE_CRASHED: return "CRASHED";
        case THREAD_STATE_EXITED:  return "EXITED";
        default:                   return "UNKNOWN";
    }
}

/**
 * Count recoveries within the tracking window (Q4).
 * Returns number of recoveries in last RECOVERY_WINDOW_MS.
 */
static int CountRecentRecoveries(void) {
    DWORD now = GetTickCount();
    int count = 0;
    
    for (int i = 0; i < MAX_RECOVERIES_IN_WINDOW; i++) {
        if (g_recoveryTimestamps[i] != 0) {
            DWORD age = now - g_recoveryTimestamps[i];
            if (age < RECOVERY_WINDOW_MS) {
                count++;
            }
        }
    }
    
    return count;
}

/**
 * Record a recovery attempt (Q4).
 */
static void RecordRecovery(void) {
    g_recoveryTimestamps[g_recoveryIndex] = GetTickCount();
    g_recoveryIndex = (g_recoveryIndex + 1) % MAX_RECOVERIES_IN_WINDOW;
    InterlockedIncrement(&g_recoveryCount);
}

/**
 * Execute recovery directly from monitor thread (Q11 - split execution).
 * This performs all safe kernel operations; UI thread only does ReplayBuffer_Start().
 * 
 * @param stallType  Which thread(s) stalled
 * @return TRUE if recovery should proceed (under limit), FALSE if permanent failure
 */
static BOOL ExecuteRecovery(StallType stallType) {
    Logger_Log("[HealthMonitor] === RECOVERY START ===\n");
    Logger_Log("[HealthMonitor] Stall type: %d\n", stallType);
    
    /* Step 1: Check thread states (Q2 - differentiate crashed vs hung) */
    ThreadState bufferState = GetThreadState(g_replayBuffer.bufferThread);
    Logger_Log("[HealthMonitor] Buffer thread state: %s\n", ThreadStateToString(bufferState));
    
    /* Step 2: Set state to RECOVERING (Q10) */
    InterlockedExchange(&g_replayBuffer.state, REPLAY_STATE_RECOVERING);
    
    /* Step 3: Signal stop event - safe kernel call */
    if (g_replayBuffer.hStopEvent) {
        SetEvent(g_replayBuffer.hStopEvent);
        Logger_Log("[HealthMonitor] Signaled stop event\n");
    }
    
    /* Step 4: Wait for thread to exit gracefully (Q11 - 5 second timeout) */
    if (g_replayBuffer.bufferThread) {
        Logger_Log("[HealthMonitor] Waiting for buffer thread (timeout=%d ms)...\n", 
                   RECOVERY_WAIT_TIMEOUT_MS);
        
        DWORD waitResult = WaitForSingleObject(g_replayBuffer.bufferThread, 
                                                RECOVERY_WAIT_TIMEOUT_MS);
        
        if (waitResult == WAIT_OBJECT_0) {
            Logger_Log("[HealthMonitor] Buffer thread exited gracefully\n");
            /* Thread exited - safe to close handle */
            CloseHandle(g_replayBuffer.bufferThread);
            g_replayBuffer.bufferThread = NULL;
        } else if (waitResult == WAIT_TIMEOUT) {
            Logger_Log("[HealthMonitor] Buffer thread did not exit - abandoning (hung)\n");
            /* Thread is truly hung - abandon it (will leak resources) */
            /* DO NOT close handle - thread is still running */
            g_replayBuffer.bufferThread = NULL;  /* Orphan it */
        } else {
            Logger_Log("[HealthMonitor] Wait failed: %u\n", GetLastError());
        }
    }
    
    /* Step 5: If thread CRASHED (not hung), we can safely cleanup (Q2) */
    if (bufferState == THREAD_STATE_CRASHED || bufferState == THREAD_STATE_EXITED) {
        Logger_Log("[HealthMonitor] Thread crashed/exited - safe to cleanup resources\n");
        /* Resources were cleaned up by the thread or will be recreated */
    } else if (bufferState == THREAD_STATE_RUNNING) {
        Logger_Log("[HealthMonitor] Thread was hung - resources orphaned (leak accepted)\n");
    }
    
    /* Step 6: Clean up NVENC sessions (Q8 - always call) */
    Logger_Log("[HealthMonitor] Cleaning up leaked NVENC sessions...\n");
    NVENCEncoder_ForceCleanupLeaked();
    
    /* Step 7: Reset heartbeat state */
    Logger_ResetHeartbeat(THREAD_BUFFER);
    Logger_ResetHeartbeat(THREAD_NVENC_OUTPUT);
    
    /* Step 8: Track recovery count (Q4) */
    RecordRecovery();
    int recentCount = CountRecentRecoveries();
    Logger_Log("[HealthMonitor] Recovery count: %d in last %d minutes\n", 
               recentCount, RECOVERY_WINDOW_MS / 60000);
    
    /* Step 9: Check if we've exceeded the limit (Q4 - 3 in 5 min) */
    if (recentCount >= MAX_RECOVERIES_IN_WINDOW) {
        Logger_Log("[HealthMonitor] PERMANENT FAILURE: Too many recoveries (%d in %d min)\n",
                   recentCount, RECOVERY_WINDOW_MS / 60000);
        InterlockedExchange(&g_replayBuffer.state, REPLAY_STATE_ERROR);
        return FALSE;  /* Don't restart - notify user of failure */
    }
    
    Logger_Log("[HealthMonitor] === RECOVERY COMPLETE - requesting restart ===\n");
    return TRUE;  /* OK to restart */
}

/* ============================================================================
 * MONITOR THREAD
 * ============================================================================ */

static DWORD WINAPI MonitorThread(LPVOID param) {
    (void)param;
    
    Logger_Log("[HealthMonitor] Monitor thread started\n");
    Logger_Log("[HealthMonitor] Config: check=%dms, soft=%dms, hard=%dms, grace=%dms\n",
               HEALTH_CHECK_INTERVAL_MS, HEALTH_SOFT_THRESHOLD_MS, 
               HEALTH_HARD_THRESHOLD_MS, RESTART_GRACE_PERIOD_MS);
    Logger_Log("[HealthMonitor] Failure limit: %d recoveries in %d minutes\n",
               MAX_RECOVERIES_IN_WINDOW, RECOVERY_WINDOW_MS / 60000);
    
    /* Track if we've already warned for current stall (avoid log spam) */
    BOOL bufferWarned = FALSE;
    BOOL nvencWarned = FALSE;
    
    while (InterlockedCompareExchange(&g_monitorRunning, 0, 0)) {
        Sleep(HEALTH_CHECK_INTERVAL_MS);
        
        if (!InterlockedCompareExchange(&g_monitorRunning, 0, 0)) break;
        
        /* Send our own heartbeat (Q7 - use THREAD_HEALTH_MONITOR) */
        Logger_Heartbeat(THREAD_HEALTH_MONITOR);
        
        /* Skip checks if monitoring is disabled */
        if (!InterlockedCompareExchange(&g_monitorEnabled, 0, 0)) {
            continue;
        }
        
        /* Grace period after restart (Q6 - after Start only) */
        if (InterlockedCompareExchange(&g_restartInProgress, 0, 0)) {
            DWORD elapsed = GetTickCount() - g_lastRestartTime;
            if (elapsed < RESTART_GRACE_PERIOD_MS) {
                continue;  /* Still in grace period */
            }
            InterlockedExchange(&g_restartInProgress, FALSE);
            Logger_Log("[HealthMonitor] Grace period ended, resuming monitoring\n");
        }
        
        /* Only monitor when replay buffer is actively capturing (Q1 - scope) */
        LONG currentState = InterlockedCompareExchange(&g_replayBuffer.state, 0, 0);
        if (currentState != REPLAY_STATE_CAPTURING) {
            bufferWarned = FALSE;
            nvencWarned = FALSE;
            continue;
        }
        
        /* Check heartbeat ages (Q3 - either stalled triggers recovery) */
        DWORD bufferAge = Logger_GetHeartbeatAge(THREAD_BUFFER);
        DWORD nvencAge = Logger_GetHeartbeatAge(THREAD_NVENC_OUTPUT);
        
        /* Soft warnings (Q5 - 2 second threshold) */
        if (bufferAge != UINT_MAX && bufferAge > HEALTH_SOFT_THRESHOLD_MS) {
            if (!bufferWarned) {
                Logger_Log("[HealthMonitor] WARNING: Buffer thread slow (age=%u ms)\n", bufferAge);
                bufferWarned = TRUE;
            }
        } else {
            bufferWarned = FALSE;
        }
        
        if (nvencAge != UINT_MAX && nvencAge > HEALTH_SOFT_THRESHOLD_MS) {
            if (!nvencWarned) {
                Logger_Log("[HealthMonitor] WARNING: NVENC thread slow (age=%u ms)\n", nvencAge);
                nvencWarned = TRUE;
            }
        } else {
            nvencWarned = FALSE;
        }
        
        /* Hard recovery (Q5 - 5 second threshold) */
        BOOL bufferStalled = (bufferAge != UINT_MAX && bufferAge > HEALTH_HARD_THRESHOLD_MS);
        BOOL nvencStalled = (nvencAge != UINT_MAX && nvencAge > HEALTH_HARD_THRESHOLD_MS);
        
        if (bufferStalled || nvencStalled) {
            /* Determine stall type (Q3) */
            StallType stallType = STALL_NONE;
            if (bufferStalled && nvencStalled) {
                stallType = STALL_MULTIPLE;
            } else if (bufferStalled) {
                stallType = STALL_BUFFER_THREAD;
            } else {
                stallType = STALL_NVENC_THREAD;
            }
            
            Logger_Log("[HealthMonitor] STALL DETECTED: buffer=%s (%ums), nvenc=%s (%ums)\n",
                       bufferStalled ? "STALLED" : "ok", bufferAge,
                       nvencStalled ? "STALLED" : "ok", nvencAge);
            
            /* Disable monitoring during recovery */
            InterlockedExchange(&g_monitorEnabled, FALSE);
            
            /* Execute recovery directly (Q11 - split execution) */
            BOOL recoveryOk = ExecuteRecovery(stallType);
            
            /* Post appropriate message to UI */
            HWND hwnd = g_hwndNotify;
            if (hwnd) {
                if (recoveryOk) {
                    /* Recovery succeeded - UI should restart */
                    if (PostMessage(hwnd, WM_WORKER_RESTART, (WPARAM)stallType, 0)) {
                        Logger_Log("[HealthMonitor] Posted WM_WORKER_RESTART to UI\n");
                    } else {
                        Logger_Log("[HealthMonitor] PostMessage failed: %u\n", GetLastError());
                    }
                } else {
                    /* Too many failures - UI should disable and notify user (Q9) */
                    if (PostMessage(hwnd, WM_WORKER_FAILED, (WPARAM)stallType, 0)) {
                        Logger_Log("[HealthMonitor] Posted WM_WORKER_FAILED to UI\n");
                    } else {
                        Logger_Log("[HealthMonitor] PostMessage failed: %u\n", GetLastError());
                    }
                }
            }
            
            /* Reset warning flags */
            bufferWarned = FALSE;
            nvencWarned = FALSE;
            
            /* Monitoring stays disabled until NotifyRestart is called */
        }
    }
    
    Logger_Log("[HealthMonitor] Monitor thread exiting\n");
    return 0;
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

BOOL HealthMonitor_Start(HWND hwndNotify) {
    if (!hwndNotify) {
        Logger_Log("[HealthMonitor] Start failed: hwndNotify is NULL\n");
        return FALSE;
    }
    
    /* Atomic check-and-set */
    if (InterlockedCompareExchange(&g_monitorRunning, TRUE, FALSE) != FALSE) {
        Logger_Log("[HealthMonitor] Already running\n");
        return TRUE;
    }
    
    g_hwndNotify = hwndNotify;
    InterlockedExchange(&g_monitorEnabled, TRUE);
    InterlockedExchange(&g_restartInProgress, FALSE);
    
    /* Reset recovery tracking */
    memset(g_recoveryTimestamps, 0, sizeof(g_recoveryTimestamps));
    g_recoveryIndex = 0;
    InterlockedExchange(&g_recoveryCount, 0);
    
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
        return;
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
    /* Order matters: set grace period BEFORE re-enabling */
    g_lastRestartTime = GetTickCount();
    InterlockedExchange(&g_restartInProgress, TRUE);
    InterlockedExchange(&g_monitorEnabled, TRUE);
    Logger_Log("[HealthMonitor] Restart notified, grace period started (%d ms)\n", 
               RESTART_GRACE_PERIOD_MS);
}

void HealthMonitor_ResetFailureCount(void) {
    memset(g_recoveryTimestamps, 0, sizeof(g_recoveryTimestamps));
    g_recoveryIndex = 0;
    InterlockedExchange(&g_recoveryCount, 0);
    Logger_Log("[HealthMonitor] Failure count reset\n");
}

int HealthMonitor_GetRecoveryCount(void) {
    return CountRecentRecoveries();
}

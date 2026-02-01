/*
 * Health Monitor - Dedicated Thread for Worker Health Monitoring
 * 
 * ARCHITECTURE (see docs/HEALTHMONITOR_ARCHITECTURE_DESIGN.md):
 * 
 * HealthMonitor is the SOLE authority for replay buffer recovery:
 * - Detects stalls via heartbeat monitoring
 * - Diagnoses CRASHED vs HUNG threads
 * - Executes recovery directly (signal, wait, cleanup)
 * - Posts to UI only for operations requiring COM (Start) or dialogs
 * 
 *   ┌─────────────────────────────────────────────────────────┐
 *   │               HEALTH MONITOR THREAD                      │
 *   │  • Checks heartbeats every 500ms                        │
 *   │  • Diagnoses crashed vs hung (GetExitCodeThread)        │
 *   │  • Executes cleanup directly (no separate thread)       │
 *   │  • Posts WM_WORKER_RESTART or WM_WORKER_FAILED to UI    │
 *   └─────────────────────────────────────────────────────────┘
 *            │                              
 *            ▼                              
 *   ┌─────────────────────────────────────────────────────────┐
 *   │      MONITORED THREADS (replay buffer only)             │
 *   │  • THREAD_BUFFER (capture + encode submit)              │
 *   │  • THREAD_NVENC_OUTPUT (bitstream retrieval)            │
 *   │  Audio and RecordingThread manage themselves (Q1)       │
 *   └─────────────────────────────────────────────────────────┘
 * 
 * THREAD SAFETY:
 * - Monitor thread runs independently
 * - Uses Logger heartbeat system (already thread-safe)
 * - Recovery uses only kernel calls (no locks needed)
 * - Posts messages to UI via PostMessage (thread-safe)
 */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <windows.h>

/* ============================================================================
 * CONFIGURATION
 * ============================================================================ */

/* How often the monitor thread checks heartbeats (ms) */
#define HEALTH_CHECK_INTERVAL_MS    500

/* Stall threshold for soft warning (ms) */
#define HEALTH_SOFT_THRESHOLD_MS    2000

/* Stall threshold for hard recovery (ms) */
#define HEALTH_HARD_THRESHOLD_MS    5000

/* Grace period after restart before monitoring resumes (ms) */
#define RESTART_GRACE_PERIOD_MS     10000

/* How long to wait for thread to exit gracefully during recovery (ms) */
#define RECOVERY_WAIT_TIMEOUT_MS    5000

/* Failure tracking: max recoveries within time window before permanent failure */
#define MAX_RECOVERIES_IN_WINDOW    3
#define RECOVERY_WINDOW_MS          (5 * 60 * 1000)  /* 5 minutes */

/* ============================================================================
 * WINDOW MESSAGES
 * ============================================================================ */

/* Posted when recovery completes, UI should call ReplayBuffer_Start() */
#define WM_WORKER_RESTART           (WM_USER + 301)

/* Posted when recovery limit exceeded (3 in 5 min), UI should disable + notify */
#define WM_WORKER_FAILED            (WM_USER + 302)

/* Legacy - kept for compatibility but WM_WORKER_RESTART/FAILED preferred */
#define WM_WORKER_STALLED           (WM_USER + 300)

/* ============================================================================
 * STALL INFO
 * ============================================================================ */

/* Which subsystem stalled - passed as WPARAM */
typedef enum {
    STALL_NONE = 0,
    STALL_BUFFER_THREAD,
    STALL_NVENC_THREAD,
    STALL_MULTIPLE
} StallType;

/* Thread state from GetExitCodeThread */
typedef enum {
    THREAD_STATE_UNKNOWN = 0,
    THREAD_STATE_RUNNING,       /* Still active (STILL_ACTIVE) */
    THREAD_STATE_CRASHED,       /* Exited with non-zero code */
    THREAD_STATE_EXITED         /* Exited normally (code 0) */
} ThreadState;

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/**
 * Initialize and start the health monitor thread.
 * 
 * @param hwndNotify  Window handle to receive WM_WORKER_* messages
 * @return TRUE if monitor started successfully
 */
BOOL HealthMonitor_Start(HWND hwndNotify);

/**
 * Stop the health monitor thread and clean up.
 * Blocks until monitor thread exits (with timeout).
 */
void HealthMonitor_Stop(void);

/**
 * Check if the health monitor is currently running.
 * 
 * @return TRUE if monitor thread is active
 */
BOOL HealthMonitor_IsRunning(void);

/**
 * Enable or disable monitoring (e.g., disable during intentional shutdown).
 * Monitor thread keeps running but won't trigger recovery.
 * 
 * @param enabled  TRUE to enable monitoring, FALSE to suspend
 */
void HealthMonitor_SetEnabled(BOOL enabled);

/**
 * Notify health monitor that replay buffer has restarted.
 * Starts grace period to prevent false positives during initialization.
 * Call this AFTER successful ReplayBuffer_Start().
 */
void HealthMonitor_NotifyRestart(void);

/**
 * Reset the failure counter (e.g., when user manually re-enables replay buffer).
 */
void HealthMonitor_ResetFailureCount(void);

/**
 * Get current recovery count within the tracking window.
 * 
 * @return Number of recoveries in the last RECOVERY_WINDOW_MS
 */
int HealthMonitor_GetRecoveryCount(void);

#endif /* HEALTH_MONITOR_H */

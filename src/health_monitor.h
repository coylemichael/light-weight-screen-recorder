/*
 * Health Monitor - Dedicated Thread for Worker Health Monitoring
 * 
 * Provides a dedicated monitoring thread that:
 * - Runs independently of the UI thread
 * - Monitors all worker thread heartbeats (buffer, NVENC, audio)
 * - Detects stalls and triggers recovery
 * - Spawns cleanup threads for orphaned resources
 * - Posts messages to UI for restart coordination
 * 
 * ARCHITECTURE:
 * 
 *   ┌─────────────────────────────────────────────────────────┐
 *   │               HEALTH MONITOR THREAD                      │
 *   │  • Checks heartbeats every 500ms                        │
 *   │  • Soft warning at 2s stall                             │
 *   │  • Hard recovery at 5s stall                            │
 *   │  • Spawns cleanup thread, posts WM_WORKER_STALLED       │
 *   └─────────────────────────────────────────────────────────┘
 *            │                              │
 *            ▼                              ▼
 *   ┌─────────────────────┐    ┌─────────────────────────────┐
 *   │   CLEANUP THREAD    │    │      WORKER THREADS         │
 *   │  (fire-and-forget)  │    │  • Buffer (capture+encode)  │
 *   │  • Waits for hung   │    │  • NVENC output             │
 *   │    thread 30-60s    │    │  • Audio capture/encode     │
 *   │  • Frees resources  │    │  • Each sends heartbeat     │
 *   └─────────────────────┘    └─────────────────────────────┘
 * 
 * THREAD SAFETY:
 * - Monitor thread runs independently
 * - Uses Logger heartbeat system (already thread-safe)
 * - Posts messages to UI via PostMessage (thread-safe)
 * - Cleanup thread uses TryEnterCriticalSection for safe access
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

/* How long cleanup thread waits for hung thread before giving up (ms) */
#define CLEANUP_WAIT_TIMEOUT_MS     60000

/* Custom window message for stall notification (avoid conflict with WM_TRAYICON at +100) */
#define WM_WORKER_STALLED           (WM_USER + 300)

/* ============================================================================
 * STALL INFO
 * ============================================================================ */

/* Which subsystem stalled - passed as WPARAM in WM_WORKER_STALLED */
typedef enum {
    STALL_NONE = 0,
    STALL_BUFFER_THREAD,
    STALL_NVENC_THREAD,
    STALL_AUDIO_THREAD,
    STALL_MULTIPLE
} StallType;

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/**
 * Initialize and start the health monitor thread.
 * 
 * @param hwndNotify  Window handle to receive WM_WORKER_STALLED messages
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
 * Notify health monitor that replay buffer is being restarted.
 * Resets stall counters to prevent false positives during restart.
 */
void HealthMonitor_NotifyRestart(void);

/**
 * Schedule cleanup of orphaned resources from a stalled thread.
 * Called internally when stall is detected. Spawns a fire-and-forget
 * cleanup thread that attempts to free resources safely.
 * 
 * @param hungThread      Handle to the hung thread (to wait on)
 * @param stallType       Which subsystem stalled
 */
void HealthMonitor_ScheduleCleanup(HANDLE hungThread, StallType stallType);

#endif /* HEALTH_MONITOR_H */

/*
 * logger.c - Async file logging + heartbeat tracking
 *
 * Architecture:
 * - Lock-free ring buffer (power-of-two size) for producers across N threads
 * - Dedicated consumer thread drains the queue to disk
 * - Per-thread heartbeat slots for stall detection
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation checks
 * - No HRESULT usage - pure Win32 APIs
 * - Silent failure on log write errors (don't crash due to logging)
 * - Thread-safe state checks using InterlockedCompareExchange
 *
 * NOTE: This module uses standard assert() instead of LWSR_ASSERT
 * because LWSR_ASSERT calls Logger_Log, which would cause infinite recursion.
 */

#include "logger.h"
#include "mem_utils.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Configuration
// ============================================================================

#define LOG_QUEUE_SIZE      4096    // Max pending log entries (must be power of two)
#define LOG_QUEUE_MASK      (LOG_QUEUE_SIZE - 1)
#define LOG_ENTRY_SIZE      512     // Max chars per log entry
#define HEARTBEAT_INTERVAL  5000    // Log heartbeat status every 5 seconds
#define STALL_THRESHOLD     10000   // Consider thread stalled after 10 seconds

// ============================================================================
// Log Entry Structure
// ============================================================================

typedef struct {
    char message[LOG_ENTRY_SIZE];
    ULONGLONG timestamp;          // GetTickCount64 value (64-bit to prevent overflow)
    volatile LONG ready;  // 0 = empty, 1 = filled
} LogEntry;

/* ============================================================================
 * THREAD HEARTBEAT TRACKING
 * ============================================================================
 * Used to detect stalled threads and log periodic status.
 */

/*
 * Thread name lookup table for logging.
 * Thread Access: [ReadOnly - initialized at compile time]
 */
static const char* const g_threadNames[THREAD_MAX] = {
    "MAIN",
    "BUFFER",
    "AUDIO_MIX",
    "AUDIO_SRC",
    "WATCHDOG"
};

typedef struct {
    // lastHeartbeat stores the low 32 bits of GetTickCount64() at the most
    // recent beat. Producer (any worker thread) writes via InterlockedExchange,
    // consumer (logger thread) reads via the volatile load. Age comparisons
    // use modular subtraction `(DWORD)now - lastBeat`, which is correct
    // across the 49.7-day DWORD wrap as long as a single age fits in DWORD
    // (always true since STALL_THRESHOLD is 10s).
    volatile LONG lastHeartbeat;
    volatile LONG beatCount;       // Total heartbeat count
    volatile LONG active;          // Is this thread running?
} ThreadHeartbeat;

/* ============================================================================
 * LOGGING SUBSYSTEM STATE
 * ============================================================================
 * Lock-free ring buffer for log messages with dedicated consumer thread.
 */

/*
 * LoggerState - Consolidated logger state
 * Houses all runtime state for the async logging subsystem.
 * Thread Access: [Mixed - see individual fields]
 */
typedef struct LoggerState {
    /* Ring buffer indices (atomic) */
    volatile LONG writeIndex;       /* Next slot for producers [Any thread] */
    volatile LONG readIndex;        /* Next slot for consumer [Logger thread] */
    
    /* Thread management [Main thread] */
    HANDLE thread;                  /* Logger thread handle */
    HANDLE event;                   /* Event to signal new entries */
    
    /* File output [Logger thread only] */
    FILE* file;
    
    /* Timing [ReadOnly after init] */
    ULONGLONG startTime;
    
    /* Initialization flags (atomic) */
    volatile LONG running;
    volatile LONG initialized;
} LoggerState;

static LoggerState g_log = {0};

/*
 * Ring buffer of pending log entries.
 * Thread Access: [Producers write to slots atomically, Logger thread reads]
 * Synchronization: Lock-free via g_log.writeIndex/g_log.readIndex atomics
 */
static LogEntry g_logQueue[LOG_QUEUE_SIZE];

/*
 * Per-thread heartbeat tracking.
 * Thread Access: [Any thread - uses atomic operations]
 * Each thread updates its own slot; logger thread reads all.
 */
static ThreadHeartbeat g_heartbeats[THREAD_MAX] = {0};

/* ============================================================================
 * LOGGER THREAD
 * ============================================================================ */

static DWORD WINAPI LoggerThreadProc(LPVOID param) {
    (void)param;
    
    ULONGLONG lastHeartbeatLog = GetTickCount64();
    
    // Thread-safe loop condition - use atomic reads for cross-thread safety
    LONG readIdx, writeIdx;
    while (InterlockedCompareExchange(&g_log.running, 0, 0) || 
           ((readIdx = InterlockedCompareExchange(&g_log.readIndex, 0, 0)) != 
            (writeIdx = InterlockedCompareExchange(&g_log.writeIndex, 0, 0)))) {
        // Wait for new entries or timeout for heartbeat check
        WaitForSingleObject(g_log.event, 1000);
        
        // Process all ready entries - use atomic reads
        while ((readIdx = InterlockedCompareExchange(&g_log.readIndex, 0, 0)) != 
               (writeIdx = InterlockedCompareExchange(&g_log.writeIndex, 0, 0))) {
            LONG idx = (LONG)((ULONG)readIdx & LOG_QUEUE_MASK);
            LogEntry* entry = &g_logQueue[idx];
            
            // Wait for entry to be ready (producer might still be writing)
            if (InterlockedCompareExchange(&entry->ready, 0, 0) == 0) {
                // Not ready yet, break and wait
                break;
            }
            
            // Write to file with timestamp
            if (g_log.file) {
                ULONGLONG relativeMs = entry->timestamp - g_log.startTime;
                fprintf(g_log.file, "[%02llu:%02llu:%02llu.%03llu] %s",
                        (relativeMs / 3600000) % 24,
                        (relativeMs / 60000) % 60,
                        (relativeMs / 1000) % 60,
                        relativeMs % 1000,
                        entry->message);
                fflush(g_log.file);
            }
            
            // Mark entry as consumed
            InterlockedExchange(&entry->ready, 0);
            InterlockedIncrement(&g_log.readIndex);
        }
        
        // Periodic heartbeat status (every HEARTBEAT_INTERVAL)
        ULONGLONG now = GetTickCount64();
        if (now - lastHeartbeatLog >= HEARTBEAT_INTERVAL) {
            lastHeartbeatLog = now;
            
            if (g_log.file) {
                ULONGLONG relativeMs = now - g_log.startTime;
                fprintf(g_log.file, "[%02llu:%02llu:%02llu.%03llu] === HEARTBEAT STATUS ===\n",
                        (relativeMs / 3600000) % 24,
                        (relativeMs / 60000) % 60,
                        (relativeMs / 1000) % 60,
                        relativeMs % 1000);
                
                for (int i = 0; i < THREAD_MAX; i++) {
                    if (g_heartbeats[i].active) {
                        DWORD lastBeat = (DWORD)g_heartbeats[i].lastHeartbeat;
                        DWORD age = (DWORD)now - lastBeat;  // Safe: relative time within session
                        LONG count = g_heartbeats[i].beatCount;
                        
                        const char* status = "OK";
                        if (age > STALL_THRESHOLD) {
                            status = "STALLED!";
                        } else if (age > STALL_THRESHOLD / 2) {
                            status = "SLOW";
                        }
                        
                        fprintf(g_log.file, "  %-12s: beats=%6ld, last=%5lums ago [%s]\n",
                                g_threadNames[i], (long)count, (unsigned long)age, status);
                    }
                }
                fprintf(g_log.file, "=========================\n");
                fflush(g_log.file);
            }
        }
    }
    
    // Final flush
    if (g_log.file) {
        ULONGLONG elapsed = GetTickCount64() - g_log.startTime;
        fprintf(g_log.file, "[%02llu:%02llu:%02llu.%03llu] Logger thread exiting normally\n",
                (elapsed / 3600000) % 24,
                (elapsed / 60000) % 60,
                (elapsed / 1000) % 60,
                elapsed % 1000);
        fflush(g_log.file);
    }
    
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

/*
 * MULTI-RESOURCE FUNCTION: Logger_Init
 * Resources: 3 - log file, signal event, logger thread
 * Pattern: goto-cleanup with SAFE_*
 */
BOOL Logger_Init(const char* filename, const char* mode) {
    // Preconditions
    assert(filename != NULL && "Logger_Init: filename cannot be NULL");
    assert(mode != NULL && "Logger_Init: mode cannot be NULL");
    
    if (!filename || !mode) return FALSE;
    
    // Thread-safe check
    if (InterlockedCompareExchange(&g_log.initialized, 0, 0)) return TRUE; // Already initialized
    
    BOOL success = FALSE;
    
    // Initialize state up-front so failure paths leave a clean slate
    g_log.startTime = GetTickCount64();
    InterlockedExchange(&g_log.writeIndex, 0);
    InterlockedExchange(&g_log.readIndex, 0);
    memset(g_logQueue, 0, sizeof(g_logQueue));
    memset(g_heartbeats, 0, sizeof(g_heartbeats));
    
    // Resource 1: log file
    g_log.file = fopen(filename, mode);
    if (!g_log.file) goto cleanup;
    
    // Resource 2: signal event
    g_log.event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_log.event) goto cleanup;
    
    // Resource 3: logger thread (set running BEFORE creating so the thread sees TRUE)
    InterlockedExchange(&g_log.running, TRUE);
    g_log.thread = CreateThread(NULL, 0, LoggerThreadProc, NULL, 0, NULL);
    if (!g_log.thread) goto cleanup;
    
    // Set high priority so logging doesn't get starved
    SetThreadPriority(g_log.thread, THREAD_PRIORITY_ABOVE_NORMAL);
    
    InterlockedExchange(&g_log.initialized, TRUE);
    success = TRUE;
    
    // Write header
    SYSTEMTIME st;
    GetLocalTime(&st);
    Logger_Log("=== LWSR Log Started %04d-%02d-%02d %02d:%02d:%02d ===\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Logger_Log("Logger thread started (async, queue=%d entries)\n", LOG_QUEUE_SIZE);
    
    return TRUE;
    
cleanup:
    // Roll back in reverse acquisition order. `success` is FALSE here.
    InterlockedExchange(&g_log.running, FALSE);
    SAFE_CLOSE_HANDLE(g_log.thread);
    SAFE_CLOSE_HANDLE(g_log.event);
    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }
    g_log.startTime = 0;
    return success;
}

void Logger_Shutdown(void) {
    // Thread-safe check
    if (!InterlockedCompareExchange(&g_log.initialized, 0, 0)) return;
    
    Logger_Log("Logger shutting down...\n");
    
    // Clear `initialized` FIRST so any new producer calls early-return before
    // they try to read g_log.event / g_logQueue. A brief sleep gives in-flight
    // producers that already passed the check time to finish their SetEvent
    // call before we close the handle.
    InterlockedExchange(&g_log.initialized, FALSE);
    Sleep(20);
    
    // Signal thread to stop - atomic write
    InterlockedExchange(&g_log.running, FALSE);
    if (g_log.event) SetEvent(g_log.event);
    
    // Wait for logger thread to finish (with timeout)
    if (g_log.thread) {
        WaitForSingleObject(g_log.thread, 5000);
        SAFE_CLOSE_HANDLE(g_log.thread);
    }
    
    // Cleanup
    SAFE_CLOSE_HANDLE(g_log.event);
    
    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }
}

void Logger_Log(const char* fmt, ...) {
    // Preconditions - fmt must be non-null
    assert(fmt != NULL && "Logger_Log: format string cannot be NULL");
    
    // Thread-safe check
    if (!InterlockedCompareExchange(&g_log.initialized, 0, 0)) return;
    
    // Check-then-claim: only advance writeIndex once we have confirmed there
    // is room. Using CAS keeps the producer lock-free while preserving the
    // ring-buffer invariant (write - read) <= LOG_QUEUE_SIZE. The previous
    // claim-then-check version left writeIndex desynced on a full queue and
    // caused the consumer to re-emit a stale slot as a duplicate.
    LONG curWrite, curRead;
    for (;;) {
        curWrite = InterlockedCompareExchange(&g_log.writeIndex, 0, 0);
        curRead  = InterlockedCompareExchange(&g_log.readIndex, 0, 0);
        // Modular distance handles signed wrap correctly.
        if ((LONG)((ULONG)curWrite - (ULONG)curRead) >= LOG_QUEUE_SIZE) {
            return;  // Queue full, drop message (better than blocking)
        }
        if (InterlockedCompareExchange(&g_log.writeIndex, curWrite + 1, curWrite) == curWrite) {
            break;  // We own slot at curWrite
        }
        // Lost the race to another producer; retry.
    }
    
    LONG idx = (LONG)((ULONG)curWrite & LOG_QUEUE_MASK);
    LogEntry* entry = &g_logQueue[idx];
    
    // Format message
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, LOG_ENTRY_SIZE - 1, fmt, args);
    va_end(args);
    entry->message[LOG_ENTRY_SIZE - 1] = '\0';
    
    // Set timestamp and mark ready
    entry->timestamp = GetTickCount64();
    InterlockedExchange(&entry->ready, 1);
    
    // Signal logger thread
    if (g_log.event) SetEvent(g_log.event);
}

void Logger_Heartbeat(ThreadId thread) {
    // Precondition: valid thread ID
    assert(thread >= 0 && thread < THREAD_MAX && "Logger_Heartbeat: invalid thread ID");
    
    if (thread < 0 || thread >= THREAD_MAX) return;
    
    // Truncate GetTickCount64() to 32 bits; modular subtraction at read time
    // keeps age comparisons correct across the DWORD wrap (see ThreadHeartbeat).
    DWORD now = (DWORD)GetTickCount64();
    InterlockedExchange(&g_heartbeats[thread].lastHeartbeat, (LONG)now);
    InterlockedIncrement(&g_heartbeats[thread].beatCount);
    InterlockedExchange(&g_heartbeats[thread].active, 1);
}

BOOL Logger_IsInitialized(void) {
    return InterlockedCompareExchange(&g_log.initialized, 0, 0);
}

void Logger_Flush(void) {
    if (!InterlockedCompareExchange(&g_log.initialized, 0, 0)) return;
    
    // Wait for queue to drain (with timeout)
    // Use atomic reads for cross-thread safety
    for (int i = 0; i < 100 && 
         InterlockedCompareExchange(&g_log.readIndex, 0, 0) != InterlockedCompareExchange(&g_log.writeIndex, 0, 0); 
         i++) {
        SetEvent(g_log.event);
        Sleep(10);
    }
}

void Logger_ResetHeartbeat(ThreadId thread) {
    if (thread < 0 || thread >= THREAD_MAX) return;
    
    // Mark thread as inactive - HealthMonitor will return UINT_MAX for age
    // This prevents stale heartbeat data from old thread instance triggering false stalls
    InterlockedExchange(&g_heartbeats[thread].active, 0);
    InterlockedExchange(&g_heartbeats[thread].lastHeartbeat, 0);
    InterlockedExchange(&g_heartbeats[thread].beatCount, 0);
}

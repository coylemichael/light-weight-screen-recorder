/*
 * Robust Async Logging Implementation
 * 
 * Architecture:
 * - Ring buffer queue for log messages (lock-free for producers)
 * - Dedicated logging thread writes to file
 * - Thread heartbeat tracking with stall detection
 * - Timestamp on every entry
 * - Designed to survive worker thread crashes
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Configuration
// ============================================================================

#define LOG_QUEUE_SIZE      4096    // Max pending log entries
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
    "NVENC",
    "AUDIO_MIX",
    "AUDIO_SRC1",
    "AUDIO_SRC2",
    "WATCHDOG"
};

typedef struct {
    volatile LONG lastHeartbeat;   // GetTickCount() of last heartbeat
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
            LONG idx = readIdx % LOG_QUEUE_SIZE;
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
                                g_threadNames[i], count, age, status);
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

void Logger_Init(const char* filename, const char* mode) {
    // Preconditions
    assert(filename != NULL && "Logger_Init: filename cannot be NULL");
    assert(mode != NULL && "Logger_Init: mode cannot be NULL");
    
    // Thread-safe check
    if (InterlockedCompareExchange(&g_log.initialized, 0, 0)) return;
    
    // Open log file
    g_log.file = fopen(filename, mode);
    if (!g_log.file) return;
    
    // Initialize state
    g_log.startTime = GetTickCount64();
    InterlockedExchange(&g_log.writeIndex, 0);
    InterlockedExchange(&g_log.readIndex, 0);
    memset(g_logQueue, 0, sizeof(g_logQueue));
    memset(g_heartbeats, 0, sizeof(g_heartbeats));
    
    // Create event for signaling new log entries
    g_log.event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_log.event) {
        fclose(g_log.file);
        g_log.file = NULL;
        return;
    }
    
    // Start logger thread - use atomic write
    InterlockedExchange(&g_log.running, TRUE);
    g_log.thread = CreateThread(NULL, 0, LoggerThreadProc, NULL, 0, NULL);
    if (!g_log.thread) {
        CloseHandle(g_log.event);
        g_log.event = NULL;
        fclose(g_log.file);
        g_log.file = NULL;
        InterlockedExchange(&g_log.running, FALSE);
        return;
    }
    
    // Set high priority so logging doesn't get starved
    SetThreadPriority(g_log.thread, THREAD_PRIORITY_ABOVE_NORMAL);
    
    InterlockedExchange(&g_log.initialized, TRUE);
    
    // Write header
    SYSTEMTIME st;
    GetLocalTime(&st);
    Logger_Log("=== LWSR Log Started %04d-%02d-%02d %02d:%02d:%02d ===\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Logger_Log("Logger thread started (async, queue=%d entries)\n", LOG_QUEUE_SIZE);
}

void Logger_Shutdown(void) {
    // Thread-safe check
    if (!InterlockedCompareExchange(&g_log.initialized, 0, 0)) return;
    
    Logger_Log("Logger shutting down...\n");
    
    // Signal thread to stop - atomic write
    InterlockedExchange(&g_log.running, FALSE);
    if (g_log.event) SetEvent(g_log.event);
    
    // Wait for logger thread to finish (with timeout)
    if (g_log.thread) {
        WaitForSingleObject(g_log.thread, 5000);
        CloseHandle(g_log.thread);
        g_log.thread = NULL;
    }
    
    // Cleanup
    if (g_log.event) {
        CloseHandle(g_log.event);
        g_log.event = NULL;
    }
    
    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }
    
    InterlockedExchange(&g_log.initialized, FALSE);
}

void Logger_Log(const char* fmt, ...) {
    // Preconditions - fmt must be non-null
    assert(fmt != NULL && "Logger_Log: format string cannot be NULL");
    
    // Thread-safe check
    if (!InterlockedCompareExchange(&g_log.initialized, 0, 0)) return;
    
    // Get next slot (atomic)
    LONG idx = InterlockedIncrement(&g_log.writeIndex) - 1;
    idx = idx % LOG_QUEUE_SIZE;
    
    // Check if slot is free (consumer hasn't caught up)
    LogEntry* entry = &g_logQueue[idx];
    if (InterlockedCompareExchange(&entry->ready, 0, 0) != 0) {
        // Queue full, drop message (better than blocking)
        return;
    }
    
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

void Logger_LogThread(ThreadId thread, const char* fmt, ...) {
    // Preconditions
    assert(fmt != NULL && "Logger_LogThread: format string cannot be NULL");
    assert(thread >= 0 && thread < THREAD_MAX && "Logger_LogThread: invalid thread ID");
    
    if (!g_log.initialized) return;
    if (thread < 0 || thread >= THREAD_MAX) return;
    
    // Get next slot
    LONG idx = InterlockedIncrement(&g_log.writeIndex) - 1;
    idx = idx % LOG_QUEUE_SIZE;
    
    LogEntry* entry = &g_logQueue[idx];
    if (InterlockedCompareExchange(&entry->ready, 0, 0) != 0) {
        return;  // Queue full
    }
    
    // Format with thread prefix
    char temp[LOG_ENTRY_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(temp, LOG_ENTRY_SIZE - 1, fmt, args);
    va_end(args);
    
    snprintf(entry->message, LOG_ENTRY_SIZE - 1, "[%s] %s", g_threadNames[thread], temp);
    entry->message[LOG_ENTRY_SIZE - 1] = '\0';
    
    entry->timestamp = GetTickCount64();
    InterlockedExchange(&entry->ready, 1);
    
    if (g_log.event) SetEvent(g_log.event);
}

void Logger_Heartbeat(ThreadId thread) {
    // Precondition: valid thread ID
    assert(thread >= 0 && thread < THREAD_MAX && "Logger_Heartbeat: invalid thread ID");
    
    if (thread < 0 || thread >= THREAD_MAX) return;
    
    // Note: Using DWORD cast since lastHeartbeat is LONG for Interlocked operations
    // This is fine for relative time comparisons within a session
    DWORD now = (DWORD)GetTickCount64();
    InterlockedExchange(&g_heartbeats[thread].lastHeartbeat, (LONG)now);
    InterlockedIncrement(&g_heartbeats[thread].beatCount);
    InterlockedExchange(&g_heartbeats[thread].active, 1);
}

BOOL Logger_IsThreadStalled(ThreadId thread) {
    if (thread < 0 || thread >= THREAD_MAX) return FALSE;
    if (!g_heartbeats[thread].active) return FALSE;
    
    DWORD now = (DWORD)GetTickCount64();
    DWORD lastBeat = (DWORD)g_heartbeats[thread].lastHeartbeat;
    DWORD age = now - lastBeat;
    
    return age > STALL_THRESHOLD;
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

const char* Logger_GetThreadName(ThreadId thread) {
    if (thread < 0 || thread >= THREAD_MAX) return "UNKNOWN";
    return g_threadNames[thread];
}

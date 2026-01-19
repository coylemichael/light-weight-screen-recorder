/*
 * Robust Async Logging Implementation
 * 
 * Architecture:
 * - Ring buffer queue for log messages (lock-free for producers)
 * - Dedicated logging thread writes to file
 * - Thread heartbeat tracking with stall detection
 * - Timestamp on every entry
 * - Designed to survive worker thread crashes
 */

#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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
    DWORD timestamp;
    volatile LONG ready;  // 0 = empty, 1 = filled
} LogEntry;

// ============================================================================
// Thread Heartbeat Tracking
// ============================================================================

static const char* g_threadNames[THREAD_MAX] = {
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

static ThreadHeartbeat g_heartbeats[THREAD_MAX] = {0};

// ============================================================================
// Global State
// ============================================================================

static LogEntry g_logQueue[LOG_QUEUE_SIZE];
static volatile LONG g_writeIndex = 0;  // Next slot for producers
static volatile LONG g_readIndex = 0;   // Next slot for consumer (logger thread)

static FILE* g_logFile = NULL;
static HANDLE g_logThread = NULL;
static volatile LONG g_logRunning = FALSE;  // Thread-safe: use InterlockedExchange
static HANDLE g_logEvent = NULL;        // Signals new log entries
static DWORD g_startTime = 0;           // For relative timestamps

static volatile LONG g_logInitialized = FALSE;  // Thread-safe: use InterlockedExchange

// ============================================================================
// Logger Thread
// ============================================================================

static DWORD WINAPI LoggerThreadProc(LPVOID param) {
    (void)param;
    
    DWORD lastHeartbeatLog = GetTickCount();
    
    // Thread-safe loop condition
    while (InterlockedCompareExchange(&g_logRunning, 0, 0) || g_readIndex != g_writeIndex) {
        // Wait for new entries or timeout for heartbeat check
        WaitForSingleObject(g_logEvent, 1000);
        
        // Process all ready entries
        while (g_readIndex != g_writeIndex) {
            LONG idx = g_readIndex % LOG_QUEUE_SIZE;
            LogEntry* entry = &g_logQueue[idx];
            
            // Wait for entry to be ready (producer might still be writing)
            if (InterlockedCompareExchange(&entry->ready, 0, 0) == 0) {
                // Not ready yet, break and wait
                break;
            }
            
            // Write to file with timestamp
            if (g_logFile) {
                DWORD relativeMs = entry->timestamp - g_startTime;
                fprintf(g_logFile, "[%02lu:%02lu:%02lu.%03lu] %s",
                        (relativeMs / 3600000) % 24,
                        (relativeMs / 60000) % 60,
                        (relativeMs / 1000) % 60,
                        relativeMs % 1000,
                        entry->message);
                fflush(g_logFile);
            }
            
            // Mark entry as consumed
            InterlockedExchange(&entry->ready, 0);
            InterlockedIncrement(&g_readIndex);
        }
        
        // Periodic heartbeat status (every HEARTBEAT_INTERVAL)
        DWORD now = GetTickCount();
        if (now - lastHeartbeatLog >= HEARTBEAT_INTERVAL) {
            lastHeartbeatLog = now;
            
            if (g_logFile) {
                DWORD relativeMs = now - g_startTime;
                fprintf(g_logFile, "[%02lu:%02lu:%02lu.%03lu] === HEARTBEAT STATUS ===\n",
                        (relativeMs / 3600000) % 24,
                        (relativeMs / 60000) % 60,
                        (relativeMs / 1000) % 60,
                        relativeMs % 1000);
                
                for (int i = 0; i < THREAD_MAX; i++) {
                    if (g_heartbeats[i].active) {
                        LONG lastBeat = g_heartbeats[i].lastHeartbeat;
                        LONG age = now - lastBeat;
                        LONG count = g_heartbeats[i].beatCount;
                        
                        const char* status = "OK";
                        if (age > STALL_THRESHOLD) {
                            status = "STALLED!";
                        } else if (age > STALL_THRESHOLD / 2) {
                            status = "SLOW";
                        }
                        
                        fprintf(g_logFile, "  %-12s: beats=%6ld, last=%5ldms ago [%s]\n",
                                g_threadNames[i], count, age, status);
                    }
                }
                fprintf(g_logFile, "=========================\n");
                fflush(g_logFile);
            }
        }
    }
    
    // Final flush
    if (g_logFile) {
        fprintf(g_logFile, "[LOGGER] Thread exiting normally\n");
        fflush(g_logFile);
    }
    
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

void Logger_Init(const char* filename, const char* mode) {
    // Thread-safe check
    if (InterlockedCompareExchange(&g_logInitialized, 0, 0)) return;
    
    // Open log file
    g_logFile = fopen(filename, mode);
    if (!g_logFile) return;
    
    // Initialize state
    g_startTime = GetTickCount();
    InterlockedExchange(&g_writeIndex, 0);
    InterlockedExchange(&g_readIndex, 0);
    memset(g_logQueue, 0, sizeof(g_logQueue));
    memset(g_heartbeats, 0, sizeof(g_heartbeats));
    
    // Create event for signaling new log entries
    g_logEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_logEvent) {
        fclose(g_logFile);
        g_logFile = NULL;
        return;
    }
    
    // Start logger thread - use atomic write
    InterlockedExchange(&g_logRunning, TRUE);
    g_logThread = CreateThread(NULL, 0, LoggerThreadProc, NULL, 0, NULL);
    if (!g_logThread) {
        CloseHandle(g_logEvent);
        g_logEvent = NULL;
        fclose(g_logFile);
        g_logFile = NULL;
        InterlockedExchange(&g_logRunning, FALSE);
        return;
    }
    
    // Set high priority so logging doesn't get starved
    SetThreadPriority(g_logThread, THREAD_PRIORITY_ABOVE_NORMAL);
    
    InterlockedExchange(&g_logInitialized, TRUE);
    
    // Write header
    SYSTEMTIME st;
    GetLocalTime(&st);
    Logger_Log("=== LWSR Log Started %04d-%02d-%02d %02d:%02d:%02d ===\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Logger_Log("Logger thread started (async, queue=%d entries)\n", LOG_QUEUE_SIZE);
}

void Logger_Shutdown(void) {
    // Thread-safe check
    if (!InterlockedCompareExchange(&g_logInitialized, 0, 0)) return;
    
    Logger_Log("Logger shutting down...\n");
    
    // Signal thread to stop - atomic write
    InterlockedExchange(&g_logRunning, FALSE);
    if (g_logEvent) SetEvent(g_logEvent);
    
    // Wait for logger thread to finish (with timeout)
    if (g_logThread) {
        WaitForSingleObject(g_logThread, 5000);
        CloseHandle(g_logThread);
        g_logThread = NULL;
    }
    
    // Cleanup
    if (g_logEvent) {
        CloseHandle(g_logEvent);
        g_logEvent = NULL;
    }
    
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
    
    InterlockedExchange(&g_logInitialized, FALSE);
}

void Logger_Log(const char* fmt, ...) {
    // Thread-safe check
    if (!InterlockedCompareExchange(&g_logInitialized, 0, 0)) return;
    
    // Get next slot (atomic)
    LONG idx = InterlockedIncrement(&g_writeIndex) - 1;
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
    entry->timestamp = GetTickCount();
    InterlockedExchange(&entry->ready, 1);
    
    // Signal logger thread
    if (g_logEvent) SetEvent(g_logEvent);
}

void Logger_LogThread(ThreadId thread, const char* fmt, ...) {
    if (!g_logInitialized) return;
    if (thread < 0 || thread >= THREAD_MAX) return;
    
    // Get next slot
    LONG idx = InterlockedIncrement(&g_writeIndex) - 1;
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
    
    entry->timestamp = GetTickCount();
    InterlockedExchange(&entry->ready, 1);
    
    if (g_logEvent) SetEvent(g_logEvent);
}

void Logger_Heartbeat(ThreadId thread) {
    if (thread < 0 || thread >= THREAD_MAX) return;
    
    DWORD now = GetTickCount();
    InterlockedExchange(&g_heartbeats[thread].lastHeartbeat, now);
    InterlockedIncrement(&g_heartbeats[thread].beatCount);
    InterlockedExchange(&g_heartbeats[thread].active, 1);
}

BOOL Logger_IsThreadStalled(ThreadId thread) {
    if (thread < 0 || thread >= THREAD_MAX) return FALSE;
    if (!g_heartbeats[thread].active) return FALSE;
    
    DWORD now = GetTickCount();
    DWORD lastBeat = g_heartbeats[thread].lastHeartbeat;
    DWORD age = now - lastBeat;
    
    return age > STALL_THRESHOLD;
}

BOOL Logger_IsInitialized(void) {
    return InterlockedCompareExchange(&g_logInitialized, 0, 0);
}

void Logger_Flush(void) {
    if (!InterlockedCompareExchange(&g_logInitialized, 0, 0)) return;
    
    // Wait for queue to drain (with timeout)
    // Use atomic reads for cross-thread safety
    for (int i = 0; i < 100 && 
         InterlockedCompareExchange(&g_readIndex, 0, 0) != InterlockedCompareExchange(&g_writeIndex, 0, 0); 
         i++) {
        SetEvent(g_logEvent);
        Sleep(10);
    }
}

const char* Logger_GetThreadName(ThreadId thread) {
    if (thread < 0 || thread >= THREAD_MAX) return "UNKNOWN";
    return g_threadNames[thread];
}

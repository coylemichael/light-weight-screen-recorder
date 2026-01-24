/*
 * Robust Async Logging System
 * 
 * Features:
 * - Dedicated logging thread (won't crash with worker threads)
 * - Lock-free queue for non-blocking log submission
 * - Automatic timestamps on all entries
 * - Thread heartbeat tracking with stall detection
 * - Survives worker thread crashes
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <windows.h>

// Thread IDs for heartbeat tracking
typedef enum {
    THREAD_MAIN = 0,
    THREAD_BUFFER,
    THREAD_NVENC_OUTPUT,
    THREAD_AUDIO_MIX,
    THREAD_AUDIO_SRC1,
    THREAD_AUDIO_SRC2,
    THREAD_WATCHDOG,
    THREAD_MAX
} ThreadId;

// Initialize the async logger (starts logging thread)
// Returns TRUE on success, FALSE on failure (file open, thread creation, etc.)
BOOL Logger_Init(const char* filename, const char* mode);

// Shutdown the logger (flushes queue, stops thread)
void Logger_Shutdown(void);

// Log a formatted message (printf-style) - NON-BLOCKING
// Adds timestamp automatically
void Logger_Log(const char* fmt, ...);

// Log with explicit thread ID prefix
void Logger_LogThread(ThreadId thread, const char* fmt, ...);

// Register a heartbeat from a thread
// Call this periodically from each worker thread
void Logger_Heartbeat(ThreadId thread);

// Check if logger is initialized
BOOL Logger_IsInitialized(void);

// Force flush all pending log entries (blocking)
void Logger_Flush(void);

// Get thread name for logging
const char* Logger_GetThreadName(ThreadId thread);

// Check if a thread is stalled (no heartbeat for >10 seconds)
BOOL Logger_IsThreadStalled(ThreadId thread);

// Get milliseconds since last heartbeat for a thread
// Returns UINT_MAX if thread has never heartbeat
DWORD Logger_GetHeartbeatAge(ThreadId thread);

#endif // LOGGER_H

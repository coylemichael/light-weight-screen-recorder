/*
 * Centralized Logging Implementation
 * Thread-safe debug logging for replay buffer and related modules
 */

#include "logger.h"
#include <stdio.h>
#include <stdarg.h>

// Global log file handle
static FILE* g_logFile = NULL;
static CRITICAL_SECTION g_logLock;
static BOOL g_logInitialized = FALSE;

void Logger_Init(const char* filename, const char* mode) {
    if (g_logInitialized) return;
    
    InitializeCriticalSection(&g_logLock);
    g_logFile = fopen(filename, mode);
    g_logInitialized = TRUE;
}

void Logger_Shutdown(void) {
    if (!g_logInitialized) return;
    
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
    LeaveCriticalSection(&g_logLock);
    
    DeleteCriticalSection(&g_logLock);
    g_logInitialized = FALSE;
}

void Logger_Log(const char* fmt, ...) {
    if (!g_logInitialized) return;
    
    EnterCriticalSection(&g_logLock);
    
    if (g_logFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fflush(g_logFile);
    }
    
    LeaveCriticalSection(&g_logLock);
}

BOOL Logger_IsInitialized(void) {
    return g_logInitialized && g_logFile != NULL;
}

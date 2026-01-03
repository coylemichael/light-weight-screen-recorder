/*
 * Centralized Logging
 * Thread-safe debug logging for replay buffer and related modules
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <windows.h>

// Initialize the logger (opens log file)
// Mode: "w" for overwrite, "a" for append
void Logger_Init(const char* filename, const char* mode);

// Shutdown the logger (closes log file)
void Logger_Shutdown(void);

// Log a formatted message (printf-style)
void Logger_Log(const char* fmt, ...);

// Check if logger is initialized
BOOL Logger_IsInitialized(void);

#endif // LOGGER_H

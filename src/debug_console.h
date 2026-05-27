/*
 * Debug Console - AllocConsole-based debug output for auto-clip pipeline
 *
 * Opens a console window showing live OCR/detection feed when enabled.
 * Tied to the "Show Regions" debug checkbox in settings.
 */

#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include <windows.h>

/* Open a console window and redirect stdout/stderr to it.
 * No-op if already open. */
void DebugConsole_Open(void);

/* Close the console window. No-op if not open. */
void DebugConsole_Close(void);

/* Print a timestamped, formatted message to the console.
 * No-op if console is not open. Thread-safe. */
void DebugConsole_Print(const char* fmt, ...);

/* Returns TRUE if the console is currently open. */
BOOL DebugConsole_IsOpen(void);

#endif /* DEBUG_CONSOLE_H */

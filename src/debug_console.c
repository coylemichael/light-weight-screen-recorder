/*
 * Debug Console - AllocConsole-based debug output for auto-clip pipeline
 *
 * Uses AllocConsole() to create a cmd-style window showing live detection
 * info. Since LWSR is /SUBSYSTEM:WINDOWS, there is no existing console to
 * conflict with. Also writes to bin\Debug\autoclip_debug_*.txt for review.
 */

#include "debug_console.h"
#include <stdio.h>
#include <stdarg.h>

static volatile BOOL s_consoleOpen = FALSE;
static CRITICAL_SECTION s_cs;
static volatile BOOL s_csInit = FALSE;
static FILE* s_logFile = NULL;

static void OpenLogFile(void)
{
    /* Write next to the exe in Debug\ folder (same as main logs) */
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (slash) *(slash + 1) = '\0';

    SYSTEMTIME st;
    GetLocalTime(&st);

    char logPath[MAX_PATH];
    _snprintf_s(logPath, sizeof(logPath), _TRUNCATE,
                "%sDebug\\autoclip_debug_%04d%02d%02d_%02d%02d%02d.txt",
                exePath, st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);

    /* Ensure Debug directory exists */
    char dirPath[MAX_PATH];
    _snprintf_s(dirPath, sizeof(dirPath), _TRUNCATE, "%sDebug", exePath);
    CreateDirectoryA(dirPath, NULL);

    fopen_s(&s_logFile, logPath, "w");
    if (s_logFile) {
        fprintf(s_logFile, "=== LWSR Auto-Clip Debug Log ===\n");
        fprintf(s_logFile, "Started: %04d-%02d-%02d %02d:%02d:%02d\n\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
        fflush(s_logFile);
    }
}

void DebugConsole_Open(void)
{
    if (s_consoleOpen) return;

    if (!s_csInit) {
        InitializeCriticalSection(&s_cs);
        s_csInit = TRUE;
    }

    if (!AllocConsole()) return;

    /* Redirect C stdio to the new console */
    FILE* fp = NULL;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    /* Set console title and size */
    SetConsoleTitleA("LWSR - Auto-Clip Debug");

    /* Make the buffer big enough to scroll back */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        COORD bufSize = { 120, 9999 };
        SetConsoleScreenBufferSize(hOut, bufSize);

        SMALL_RECT winRect = { 0, 0, 119, 34 };
        SetConsoleWindowInfo(hOut, TRUE, &winRect);

        /* Green-on-black for that debug feel */
        SetConsoleTextAttribute(hOut, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    }

    /* Open log file alongside console */
    OpenLogFile();

    s_consoleOpen = TRUE;

    printf("=== LWSR Auto-Clip Debug Console ===\n");
    printf("Showing live OCR / kill detection feed.\n");
    if (s_logFile)
        printf("Logging to: bin\\Debug\\autoclip_debug_*.txt\n");
    printf("Uncheck 'Debug console' to stop.\n\n");
}

void DebugConsole_Close(void)
{
    if (!s_consoleOpen) return;

    /* Set flag first to prevent new Print calls from entering */
    s_consoleOpen = FALSE;

    /* Wait for any in-flight Print call to finish */
    EnterCriticalSection(&s_cs);

    printf("\n=== Debug console closing ===\n");
    fflush(stdout);

    if (s_logFile) {
        fprintf(s_logFile, "\n=== Debug log closed ===\n");
        fclose(s_logFile);
        s_logFile = NULL;
    }

    /* Redirect stdout/stderr to NUL BEFORE FreeConsole.
     * This ensures no thread can write to the console handle after it's freed,
     * and FreeConsole won't conflict with in-flight CRT console writes. */
    FILE* nul = NULL;
    freopen_s(&nul, "NUL", "w", stdout);
    freopen_s(&nul, "NUL", "w", stderr);

    FreeConsole();

    LeaveCriticalSection(&s_cs);
}

void DebugConsole_Print(const char* fmt, ...)
{
    if (!s_consoleOpen) return;

    EnterCriticalSection(&s_cs);

    /* Re-check after acquiring lock: Close() may have freed the console
     * between our flag check and entering the CS */
    if (!s_consoleOpen) {
        LeaveCriticalSection(&s_cs);
        return;
    }

    /* Timestamp */
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d.%03d] ",
           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    fflush(stdout);

    /* Mirror to log file */
    if (s_logFile) {
        fprintf(s_logFile, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args2;
        va_start(args2, fmt);
        vfprintf(s_logFile, fmt, args2);
        va_end(args2);
        fflush(s_logFile);
    }

    LeaveCriticalSection(&s_cs);
}

BOOL DebugConsole_IsOpen(void)
{
    return s_consoleOpen;
}

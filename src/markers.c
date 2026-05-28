/*
 * markers.c - Recording markers - timestamped bookmarks + sidecar file writer
 *
 * InterlockedIncrement protects count read/write, but Markers_Add has a race
 * between slot claim and data write; safe only if readers occur after recording
 * stops (current usage: writers on F8 keypress, readers after stop).
 *
 * ERROR HANDLING PATTERN:
 * - Early return for NULL/invalid parameters
 * - File I/O errors return FALSE
 */

#include "markers.h"
#include "constants.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>

void Markers_Init(MarkerList* list) {
    LWSR_ASSERT(list != NULL);
    if (!list) return;
    
    memset(list->markers, 0, sizeof(list->markers));
    InterlockedExchange(&list->count, 0);
}

BOOL Markers_Add(MarkerList* list, ULONGLONG timestampMs) {
    LWSR_ASSERT(list != NULL);
    if (!list) return FALSE;
    
    /* Atomically claim a slot */
    LONG index = InterlockedIncrement(&list->count) - 1;
    if (index >= MAX_MARKERS) {
        /* Full - roll back the count */
        InterlockedDecrement(&list->count);
        Logger_Log("Markers_Add: list full (%d max)\n", MAX_MARKERS);
        return FALSE;
    }
    
    list->markers[index].timestampMs = timestampMs;
    Logger_Log("Marker added: index=%ld, timestampMs=%llu\n", index, timestampMs);
    return TRUE;
}

int Markers_GetCount(const MarkerList* list) {
    if (!list) return 0;
    return (int)InterlockedCompareExchange((volatile LONG*)&list->count, 0, 0);
}

void Markers_Clear(MarkerList* list) {
    if (!list) return;
    InterlockedExchange(&list->count, 0);
}

BOOL Markers_WriteSidecar(const MarkerList* list, const char* videoPath,
                          ULONGLONG startMs, ULONGLONG endMs) {
    LWSR_ASSERT(list != NULL);
    LWSR_ASSERT(videoPath != NULL);
    if (!list || !videoPath) return FALSE;
    
    int total = Markers_GetCount(list);
    if (total == 0) return FALSE;
    
    /* Build sidecar path: replace extension with .markers.txt */
    char sidecarPath[MAX_PATH];
    strncpy(sidecarPath, videoPath, MAX_PATH - 1);
    sidecarPath[MAX_PATH - 1] = '\0';

    /* Locate filename start (past last '\' or '/') so we only match a dot in
     * the filename, not one inside a directory name like "C:\foo.bar\clip". */
    char* filename = sidecarPath;
    for (char* p = sidecarPath; *p; p++) {
        if (*p == '\\' || *p == '/') filename = p + 1;
    }
    char* dot = strrchr(filename, '.');
    if (dot) {
        *dot = '\0';
    }

    /* Verify the suffix fits; strncat would otherwise silently drop chars and
     * leave sidecarPath equal to videoPath, causing fopen to overwrite it. */
    const char* suffix = ".markers.txt";
    size_t used = strlen(sidecarPath);
    size_t need = strlen(suffix);
    if (used + need + 1 > MAX_PATH) {
        Logger_Log("Markers_WriteSidecar: path too long (%zu + %zu >= %d)\n",
                   used, need, MAX_PATH);
        return FALSE;
    }
    strncat(sidecarPath, suffix, MAX_PATH - used - 1);
    
    /* Count markers in range */
    int inRange = 0;
    for (int i = 0; i < total; i++) {
        ULONGLONG ts = list->markers[i].timestampMs;
        if (ts >= startMs && ts <= endMs) {
            inRange++;
        }
    }
    
    if (inRange == 0) {
        Logger_Log("Markers_WriteSidecar: no markers in range [%llu, %llu]\n",
                   startMs, endMs);
        return FALSE;
    }
    
    FILE* f = fopen(sidecarPath, "w");
    if (!f) {
        Logger_Log("Markers_WriteSidecar: failed to create %s\n", sidecarPath);
        return FALSE;
    }
    
    for (int i = 0; i < total; i++) {
        ULONGLONG ts = list->markers[i].timestampMs;
        if (ts < startMs || ts > endMs) continue;
        
        /* Rebase to 0 and convert ms to HH:MM:SS.mmm */
        ULONGLONG relativeMs = ts - startMs;
        int ms  = (int)(relativeMs % 1000);
        int sec = (int)((relativeMs / 1000) % 60);
        int min = (int)((relativeMs / 60000) % 60);
        int hrs = (int)(relativeMs / 3600000);
        
        fprintf(f, "%02d:%02d:%02d.%03d\n", hrs, min, sec, ms);
    }
    
    fclose(f);
    Logger_Log("Markers_WriteSidecar: wrote %d markers to %s\n", inRange, sidecarPath);
    return TRUE;
}

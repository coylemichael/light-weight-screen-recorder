/*
 * markers.c - Recording Markers (Bookmarks)
 *
 * Stores timestamped markers and writes sidecar files.
 * Thread-safe add via InterlockedIncrement on count.
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
    
    char* dot = strrchr(sidecarPath, '.');
    if (dot) {
        *dot = '\0';
    }
    strncat(sidecarPath, ".markers.txt", MAX_PATH - strlen(sidecarPath) - 1);
    
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

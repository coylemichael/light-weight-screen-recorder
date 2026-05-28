/*
 * markers.h - Recording Markers (Bookmarks)
 *
 * Stores timestamped markers during recording or replay buffer capture.
 * On save, writes a .markers.txt sidecar file alongside the video.
 *
 * Thread safety: InterlockedIncrement protects count read/write, but
 * Markers_Add has a race between slot claim and data write; safe only if
 * readers occur after recording stops.
 */

#ifndef MARKERS_H
#define MARKERS_H

#include <windows.h>

/* Maximum markers per recording/replay session */
#define MAX_MARKERS 256

typedef struct {
    ULONGLONG timestampMs;  /* GetTickCount64() value when marker was placed */
} Marker;

typedef struct {
    Marker markers[MAX_MARKERS];
    volatile LONG count;    /* InterlockedIncrement-published; see header note on slot-write race */
} MarkerList;

/* Reset marker list (call at recording/replay start) */
void Markers_Init(MarkerList* list);

/* Add a marker at the given timestamp (GetTickCount64 ms). Returns FALSE if list is full. */
BOOL Markers_Add(MarkerList* list, ULONGLONG timestampMs);

/* Get current marker count (thread-safe read) */
int Markers_GetCount(const MarkerList* list);

/* Write .markers.txt sidecar file next to videoPath.
 * Only writes markers within [startMs, endMs] (GetTickCount64 values).
 * Timestamps in the file are rebased to 0 (relative to startMs).
 * Returns FALSE if no markers to write or file creation fails. */
BOOL Markers_WriteSidecar(const MarkerList* list, const char* videoPath,
                          ULONGLONG startMs, ULONGLONG endMs);

/* Clear all markers */
void Markers_Clear(MarkerList* list);

#endif /* MARKERS_H */

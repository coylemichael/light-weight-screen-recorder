/*
 * autoclip_overlay.h - Debug overlay for auto-clip region visualization
 *
 * When autoClipShowRegions is enabled, draws colored rectangles over the
 * monitor showing the configured kill feed and badge detection regions.
 * Shows detected player name near the badge box once OCR confirms it.
 *
 * Thread model: All functions must be called from the main (UI) thread.
 */

#ifndef AUTOCLIP_OVERLAY_H
#define AUTOCLIP_OVERLAY_H

#include <windows.h>
#include "config.h"
#include "capture.h"

/* Create and show the debug overlay window.
 * Draws badge region outline (cyan).
 * Returns TRUE on success. */
BOOL AutoClipOverlay_Show(const AppConfig* config, const CaptureState* capture);

/* Update with successfully parsed player name (green highlight). */
void AutoClipOverlay_SetName(const char* name);

/* Update with raw badge OCR text (yellow, shown even if parse failed).
 * Lets the user see what OCR is reading for debugging. */
void AutoClipOverlay_SetRawOCR(const char* rawText);

/* Hide and destroy the overlay window. */
void AutoClipOverlay_Hide(void);

#endif /* AUTOCLIP_OVERLAY_H */

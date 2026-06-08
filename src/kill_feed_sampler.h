#ifndef KILL_FEED_SAMPLER_H
#define KILL_FEED_SAMPLER_H

/*
 * Kill Feed Sampler - Template-only kill detection
 *
 * Scans a calibrated screen region for "RUNNER DOWN" / "RUNNER DOWN [ASSIST]"
 * banner templates using NCC (Normalized Cross-Correlation) matching.
 *
 * On match: triggers WM_AUTOCLIP_SAVE with "Marathon_Kill".
 * Foreground check: only triggers when Marathon is in front.
 *
 * - Scans every 2 seconds
 * - Replay trigger cooldown: 30 seconds
 */

#include <windows.h>
#include <d3d11.h>
#include "config.h"
#include "capture.h"

typedef struct KillFeedSampler KillFeedSampler;

/* Initialize. Returns NULL if detection region not calibrated.
 * overlayWnd receives WM_AUTOCLIP_SAVE when a kill is detected. */
KillFeedSampler* KillFeedSampler_Init(const AppConfig* config, const CaptureState* capture,
                                      HWND overlayWnd);

/* Call every frame from capture thread. Handles internal timing. */
void KillFeedSampler_FeedFrame(KillFeedSampler* sampler, CaptureState* capture,
                               ID3D11Texture2D* bgraTexture);

/* Write companion .txt (always) and .bmp (when debugMode=TRUE) next to the clip.
 * Call after the auto-clip save completes. Consumes stored trigger context. */
void KillFeedSampler_WriteTriggerContext(KillFeedSampler* sampler, const char* clipPath, BOOL debugMode);

/* Last best NCC match from the most recent scan, in monitor-overlay coordinates
 * (compatible with the settings_dialog region-overlay window at (0,0,SM_CXSCREEN,SM_CYSCREEN)).
 * Returns TRUE only if a value was published within the last ~3s and score >= 0.50.
 * Safe to call from any thread; no sampler pointer needed (single-instance assumption). */
BOOL KillFeedSampler_GetLastMatch(int* outX, int* outY, int* outW, int* outH, float* outScore);

/* Shutdown and free resources. */
void KillFeedSampler_Shutdown(KillFeedSampler* sampler);

#endif /* KILL_FEED_SAMPLER_H */

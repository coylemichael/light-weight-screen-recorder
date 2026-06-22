#ifndef KILL_FEED_SAMPLER_H
#define KILL_FEED_SAMPLER_H

/*
 * Kill Feed Sampler - Template-only kill detection
 *
 * Scans a calibrated screen region for game-specific banner templates using
 * multi-scale NCC (Normalized Cross-Correlation). The active game and its
 * templates/region/threshold/cooldown come from a GameProfile selected by
 * the buffer thread based on the current foreground exe.
 *
 * On match: posts WM_AUTOCLIP_SAVE with the profile's SaveLabel.
 *
 * Foreground gating happens upstream in replay_buffer.c — when a sampler
 * exists, it is for the foreground game. This module no longer makes any
 * "is game X in front" decisions.
 */

#include <windows.h>
#include <d3d11.h>
#include "capture.h"
#include "game_profile.h"

typedef struct KillFeedSampler KillFeedSampler;

/* Initialize a sampler bound to a GameProfile. Returns NULL if the profile
 * has no valid region, no templates load, or any resource fails. The sampler
 * holds the profile pointer (for cooldown bookkeeping); the profile must
 * outlive the sampler (catalog lives for the process). */
KillFeedSampler* KillFeedSampler_Init(GameProfile* profile, const CaptureState* capture,
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

/* Return the GameProfile this sampler is bound to. NULL if sampler is NULL. */
const GameProfile* KillFeedSampler_GetProfile(const KillFeedSampler* sampler);

/* Shutdown and free resources. */
void KillFeedSampler_Shutdown(KillFeedSampler* sampler);

#endif /* KILL_FEED_SAMPLER_H */

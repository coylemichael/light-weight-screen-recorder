/*
 * game_profile.h - Per-game auto-clip catalog
 *
 * Loads game profiles from <exeDir>\games\*.ini at startup. Each profile
 * declares the executable basenames that identify the game, the detection
 * region, the template list, the threshold, the cooldown, and the save
 * label. User overrides (enable + calibrated region + custom cooldown)
 * live in lwsr_config.ini under [AutoClip.<ProfileId>].
 *
 * The catalog is shared (one instance per process). Lookups by foreground
 * exe basename are O(N * E) where N=profiles and E=exes-per-profile; both
 * small (~10 each), so a linear scan is fine.
 *
 * Threading: catalog is built once on the UI thread at startup, then
 * read-only. The per-profile cooldown timestamp is the only mutable field
 * and is only touched by the buffer thread (single-writer).
 */

#ifndef GAME_PROFILE_H
#define GAME_PROFILE_H

#include <windows.h>

#define GAME_PROFILE_MAX_EXES         8
#define GAME_PROFILE_MAX_TEMPLATES    8
#define GAME_PROFILE_ID_LEN           64    /* INI filename without extension; also section suffix */
#define GAME_PROFILE_NAME_LEN         64    /* Display name */
#define GAME_PROFILE_EXE_LEN          64    /* Single exe basename */
#define GAME_PROFILE_TEMPLATE_LEN     64    /* Single template basename (no .png) */
#define GAME_PROFILE_LABEL_LEN        64    /* SaveLabel: clip filename prefix + save subfolder */
#define GAME_PROFILE_DIR_LEN          64    /* TemplatesDir: subfolder under static\ */
#define GAME_PROFILE_MAX_PROFILES     16    /* Hard cap on catalog size */

typedef struct {
    /* Identity */
    char id[GAME_PROFILE_ID_LEN];           /* e.g. "marathon" (from filename, lowercase) */
    char displayName[GAME_PROFILE_NAME_LEN];/* e.g. "Marathon" */

    /* Foreground matching */
    char exes[GAME_PROFILE_MAX_EXES][GAME_PROFILE_EXE_LEN]; /* basenames, case-insensitive */
    int  exeCount;

    /* Detection */
    char templatesDir[GAME_PROFILE_DIR_LEN];                /* subfolder under static\ */
    char templates[GAME_PROFILE_MAX_TEMPLATES][GAME_PROFILE_TEMPLATE_LEN];
    int  templateCount;
    float templateThreshold;                /* NCC threshold (0..1) */
    int  defaultCooldownSec;                /* fallback if no user override */
    char saveLabel[GAME_PROFILE_LABEL_LEN]; /* clip filename prefix + save subfolder */

    /* Catalog default region (monitor-relative percentages). User calibration
     * in lwsr_config.ini overrides these when present. */
    float defaultRegionXPct;
    float defaultRegionYPct;
    float defaultRegionWPct;
    float defaultRegionHPct;

    /* ── User-override state (loaded from lwsr_config.ini) ── */
    BOOL  userEnabled;                       /* defaults to TRUE */
    BOOL  userHasRegion;                     /* TRUE if region keys exist in user config */
    float userRegionXPct;
    float userRegionYPct;
    float userRegionWPct;
    float userRegionHPct;
    BOOL  userHasCooldown;                   /* TRUE if user CooldownSec key exists */
    int   userCooldownSec;

    /* ── Runtime state (buffer thread only) ── */
    ULONGLONG lastTriggerMs;                 /* cooldown timestamp, persists across sampler restarts */
} GameProfile;

/* Resolve the active region (user override if present, else catalog default).
 * Returns FALSE if neither produces a usable rect (W or H <= 0). */
BOOL GameProfile_GetActiveRegion(const GameProfile* p, float* outX, float* outY,
                                 float* outW, float* outH);

/* Resolve the active cooldown in seconds (user override if present, else default). */
int GameProfile_GetActiveCooldownSec(const GameProfile* p);

/* ─── Catalog ─── */

/* Load all profiles from <exeDir>\games\*.ini. Idempotent: safe to call once.
 * Returns the number of profiles loaded (0 on failure or empty catalog). */
int GameProfile_LoadCatalog(void);

/* Number of loaded profiles. Safe to call after LoadCatalog. */
int GameProfile_GetCount(void);

/* Get profile by index [0..count). NULL if out of range. */
GameProfile* GameProfile_GetByIndex(int idx);

/* Look up profile by id (case-insensitive). NULL if not found. */
GameProfile* GameProfile_FindById(const char* id);

/* Look up profile by foreground exe basename (case-insensitive, with or
 * without .exe suffix). NULL if no profile claims that exe.
 * Skips profiles where userEnabled is FALSE. */
GameProfile* GameProfile_FindByExe(const char* exeBasename);

/* Resolve the foreground process to a profile (or NULL).
 * Calls GetForegroundWindow + QueryFullProcessImageName internally.
 * Cheap enough to call per-frame on the buffer thread. */
GameProfile* GameProfile_GetForegroundMatch(void);

/* Returns TRUE if hwnd has changed since the last call (per-thread state
 * intended for use by the buffer thread's foreground-change polling).
 * On first call returns TRUE so callers re-resolve. */
BOOL GameProfile_ForegroundChanged(HWND* lastSeen);

/* Persist user override fields (Enabled, Region*, CooldownSec) to
 * lwsr_config.ini under [AutoClip.<id>]. Called when calibration finishes
 * or when the per-game enable checkbox toggles. */
void GameProfile_SaveUserOverrides(const GameProfile* p);

/* Free everything. Call at app shutdown. */
void GameProfile_Shutdown(void);

#endif /* GAME_PROFILE_H */

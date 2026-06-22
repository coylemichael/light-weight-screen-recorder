/*
 * game_profile.c - Catalog loader for per-game auto-clip profiles
 *
 * Profiles live in <exeDir>\games\*.ini. The filename (without extension,
 * lowercased) is the profile id and the section suffix in lwsr_config.ini.
 *
 * Foreground-match lookup is O(profiles * exes) — linear is fine at this
 * scale and we only hit it on HWND changes from the buffer thread.
 */

#include "game_profile.h"
#include "logger.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

/* Catalog (single instance per process; built once at startup). */
static GameProfile g_profiles[GAME_PROFILE_MAX_PROFILES];
static int g_profileCount = 0;
static BOOL g_loaded = FALSE;

/* ─── Helpers ─── */

/* Resolve the directory containing the running exe (with trailing backslash).
 * Returns FALSE on failure. */
static BOOL GetExeDir(char* out, size_t outSize)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)outSize);
    if (n == 0 || n >= outSize) return FALSE;
    char* slash = strrchr(out, '\\');
    if (!slash) return FALSE;
    *(slash + 1) = '\0';
    return TRUE;
}

/* Build the path to lwsr_config.ini (where user overrides live). */
static void GetUserConfigPath(char* out, size_t outSize)
{
    if (!GetExeDir(out, outSize)) {
        strncpy(out, "lwsr_config.ini", outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }
    strncat(out, "lwsr_config.ini", outSize - strlen(out) - 1);
}

/* lowercase in place (ASCII only — INI ids are ASCII). */
static void StrLowerAscii(char* s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

/* Trim leading/trailing whitespace in place. */
static void StrTrim(char* s)
{
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

/* Strip a trailing ".exe" (case-insensitive). Returns input length after strip. */
static void StripExeSuffix(char* s)
{
    size_t len = strlen(s);
    if (len >= 4 && _stricmp(s + len - 4, ".exe") == 0) {
        s[len - 4] = '\0';
    }
}

/* Build a profile id from an INI filename (lowercase, strip .ini). */
static void IdFromFilename(const char* filename, char* out, size_t outSize)
{
    strncpy(out, filename, outSize - 1);
    out[outSize - 1] = '\0';
    size_t len = strlen(out);
    if (len >= 4 && _stricmp(out + len - 4, ".ini") == 0) {
        out[len - 4] = '\0';
    }
    StrLowerAscii(out);
}

/* Parse a comma-separated list. Writes up to maxCount entries into out[][],
 * each truncated to entryLen-1 chars. Returns the number written. */
static int ParseCsvList(const char* csv, char (*out)[GAME_PROFILE_TEMPLATE_LEN],
                        int maxCount)
{
    int n = 0;
    const char* p = csv;
    while (*p && n < maxCount) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t')) len--;
        if (len == 0) continue;
        if (len >= GAME_PROFILE_TEMPLATE_LEN) len = GAME_PROFILE_TEMPLATE_LEN - 1;
        memcpy(out[n], start, len);
        out[n][len] = '\0';
        n++;
    }
    return n;
}

/* GetPrivateProfileString wrapper that returns a float (atof). */
static float ReadFloat(const char* section, const char* key, float fallback,
                       const char* path)
{
    char buf[32];
    char defBuf[32];
    snprintf(defBuf, sizeof(defBuf), "%.6f", fallback);
    GetPrivateProfileStringA(section, key, defBuf, buf, sizeof(buf), path);
    return (float)atof(buf);
}

/* GetPrivateProfileString that distinguishes "missing" from "present with default". */
static BOOL ReadKeyExists(const char* section, const char* key, const char* path)
{
    char buf[8];
    /* Use a sentinel that no legit value will match. */
    DWORD n = GetPrivateProfileStringA(section, key, "\x01__missing__", buf, sizeof(buf), path);
    if (n == 0) return FALSE;
    return strcmp(buf, "\x01__missing__") != 0;
}

/* ─── User overrides ─── */

static void LoadUserOverrides(GameProfile* p, const char* userConfigPath)
{
    char section[GAME_PROFILE_ID_LEN + 16];
    snprintf(section, sizeof(section), "AutoClip.%s", p->id);

    p->userEnabled = GetPrivateProfileIntA(section, "Enabled", 1, userConfigPath) ? TRUE : FALSE;

    if (ReadKeyExists(section, "RegionWPct", userConfigPath)) {
        p->userRegionXPct = ReadFloat(section, "RegionXPct", 0.0f, userConfigPath);
        p->userRegionYPct = ReadFloat(section, "RegionYPct", 0.0f, userConfigPath);
        p->userRegionWPct = ReadFloat(section, "RegionWPct", 0.0f, userConfigPath);
        p->userRegionHPct = ReadFloat(section, "RegionHPct", 0.0f, userConfigPath);
        if (p->userRegionWPct > 0.0f && p->userRegionHPct > 0.0f) {
            p->userHasRegion = TRUE;
        }
    }

    if (ReadKeyExists(section, "CooldownSec", userConfigPath)) {
        p->userCooldownSec = GetPrivateProfileIntA(section, "CooldownSec",
                                                   p->defaultCooldownSec, userConfigPath);
        p->userHasCooldown = TRUE;
    }
}

/* ─── Legacy migration ─── */

/* If the legacy single-region keys exist under [AutoClip] and the user has
 * no [AutoClip.marathon] overrides yet, migrate them. Then strip the legacy
 * keys so we never read them again. */
static void MigrateLegacyMarathonRegion(const char* userConfigPath)
{
    GameProfile* marathon = GameProfile_FindById("marathon");
    if (!marathon || marathon->userHasRegion) return;

    if (!ReadKeyExists("AutoClip", "killfeedWPct", userConfigPath)) return;

    float x = ReadFloat("AutoClip", "killfeedXPct", 0.0f, userConfigPath);
    float y = ReadFloat("AutoClip", "killfeedYPct", 0.0f, userConfigPath);
    float w = ReadFloat("AutoClip", "killfeedWPct", 0.0f, userConfigPath);
    float h = ReadFloat("AutoClip", "killfeedHPct", 0.0f, userConfigPath);

    if (w <= 0.0f || h <= 0.0f) return;

    marathon->userHasRegion = TRUE;
    marathon->userRegionXPct = x;
    marathon->userRegionYPct = y;
    marathon->userRegionWPct = w;
    marathon->userRegionHPct = h;

    GameProfile_SaveUserOverrides(marathon);

    /* Strip the legacy keys (write NULL → delete) */
    WritePrivateProfileStringA("AutoClip", "killfeedXPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "killfeedYPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "killfeedWPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "killfeedHPct", NULL, userConfigPath);
    /* Also clean up other dead keys observed in lwsr_config.ini */
    WritePrivateProfileStringA("AutoClip", "badgeXPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "badgeYPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "badgeWPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "badgeHPct", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "PlayerName", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "ShowName", NULL, userConfigPath);
    WritePrivateProfileStringA("AutoClip", "ShowProcessing", NULL, userConfigPath);

    Logger_Log("GameProfile: migrated legacy killfeed region to [AutoClip.marathon]\n");
}

/* ─── Profile parsing ─── */

static BOOL ParseProfileFile(const char* iniPath, const char* filename, GameProfile* p)
{
    memset(p, 0, sizeof(*p));
    p->userEnabled = TRUE;

    IdFromFilename(filename, p->id, sizeof(p->id));
    if (!p->id[0]) return FALSE;

    /* [Profile] */
    GetPrivateProfileStringA("Profile", "Name", p->id,
                             p->displayName, sizeof(p->displayName), iniPath);

    char exesBuf[GAME_PROFILE_MAX_EXES * GAME_PROFILE_EXE_LEN];
    GetPrivateProfileStringA("Profile", "Exes", "", exesBuf, sizeof(exesBuf), iniPath);
    if (!exesBuf[0]) {
        Logger_Log("GameProfile: '%s' has no Exes= entry, skipping\n", filename);
        return FALSE;
    }
    p->exeCount = ParseCsvList(exesBuf, (char (*)[GAME_PROFILE_TEMPLATE_LEN])p->exes,
                               GAME_PROFILE_MAX_EXES);
    /* Strip .exe suffix and lowercase each exe for fast compare */
    for (int i = 0; i < p->exeCount; i++) {
        StrTrim(p->exes[i]);
        StripExeSuffix(p->exes[i]);
        StrLowerAscii(p->exes[i]);
    }

    /* [Detection] */
    char methodBuf[32];
    GetPrivateProfileStringA("Detection", "Method", "Template",
                             methodBuf, sizeof(methodBuf), iniPath);
    if (_stricmp(methodBuf, "Template") != 0) {
        Logger_Log("GameProfile: '%s' Method=%s not supported (only Template), skipping\n",
                   filename, methodBuf);
        return FALSE;
    }

    GetPrivateProfileStringA("Detection", "TemplatesDir", p->id,
                             p->templatesDir, sizeof(p->templatesDir), iniPath);

    char tmplsBuf[GAME_PROFILE_MAX_TEMPLATES * GAME_PROFILE_TEMPLATE_LEN];
    GetPrivateProfileStringA("Detection", "Templates", "", tmplsBuf, sizeof(tmplsBuf), iniPath);
    p->templateCount = ParseCsvList(tmplsBuf, p->templates, GAME_PROFILE_MAX_TEMPLATES);
    for (int i = 0; i < p->templateCount; i++) StrTrim(p->templates[i]);

    if (p->templateCount == 0) {
        Logger_Log("GameProfile: '%s' has no Templates= entry, skipping\n", filename);
        return FALSE;
    }

    p->templateThreshold = ReadFloat("Detection", "TemplateThreshold", 0.80f, iniPath);
    if (p->templateThreshold < 0.1f) p->templateThreshold = 0.1f;
    if (p->templateThreshold > 1.0f) p->templateThreshold = 1.0f;

    p->defaultCooldownSec = GetPrivateProfileIntA("Detection", "CooldownSec", 10, iniPath);
    if (p->defaultCooldownSec < AUTOCLIP_COOLDOWN_MIN_SEC) p->defaultCooldownSec = AUTOCLIP_COOLDOWN_MIN_SEC;
    if (p->defaultCooldownSec > AUTOCLIP_COOLDOWN_MAX_SEC) p->defaultCooldownSec = AUTOCLIP_COOLDOWN_MAX_SEC;

    /* SaveLabel defaults to "<DisplayName>_Kill" for backward compatibility */
    {
        char defaultLabel[GAME_PROFILE_LABEL_LEN];
        snprintf(defaultLabel, sizeof(defaultLabel), "%s_Kill", p->displayName);
        GetPrivateProfileStringA("Detection", "SaveLabel", defaultLabel,
                                 p->saveLabel, sizeof(p->saveLabel), iniPath);
    }

    /* [DefaultRegion] */
    p->defaultRegionXPct = ReadFloat("DefaultRegion", "XPct", 0.0f, iniPath);
    p->defaultRegionYPct = ReadFloat("DefaultRegion", "YPct", 0.0f, iniPath);
    p->defaultRegionWPct = ReadFloat("DefaultRegion", "WPct", 0.0f, iniPath);
    p->defaultRegionHPct = ReadFloat("DefaultRegion", "HPct", 0.0f, iniPath);

    return TRUE;
}

/* ─── Public API ─── */

int GameProfile_LoadCatalog(void)
{
    if (g_loaded) return g_profileCount;
    g_loaded = TRUE;
    g_profileCount = 0;

    char exeDir[MAX_PATH];
    if (!GetExeDir(exeDir, sizeof(exeDir))) {
        Logger_Log("GameProfile: cannot resolve exe dir, catalog empty\n");
        return 0;
    }

    char glob[MAX_PATH];
    snprintf(glob, sizeof(glob), "%sgames\\*.ini", exeDir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(glob, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        Logger_Log("GameProfile: no games\\*.ini found at %s\n", glob);
        return 0;
    }

    char userConfigPath[MAX_PATH];
    GetUserConfigPath(userConfigPath, sizeof(userConfigPath));

    do {
        if (g_profileCount >= GAME_PROFILE_MAX_PROFILES) {
            Logger_Log("GameProfile: catalog cap (%d) reached, ignoring '%s'\n",
                       GAME_PROFILE_MAX_PROFILES, fd.cFileName);
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char iniPath[MAX_PATH];
        snprintf(iniPath, sizeof(iniPath), "%sgames\\%s", exeDir, fd.cFileName);

        GameProfile* p = &g_profiles[g_profileCount];
        if (!ParseProfileFile(iniPath, fd.cFileName, p)) continue;

        LoadUserOverrides(p, userConfigPath);
        g_profileCount++;

        Logger_Log("GameProfile: loaded '%s' (id=%s, exes=%d, templates=%d, enabled=%d)\n",
                   p->displayName, p->id, p->exeCount, p->templateCount, p->userEnabled);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    MigrateLegacyMarathonRegion(userConfigPath);

    Logger_Log("GameProfile: catalog loaded with %d profile(s)\n", g_profileCount);
    return g_profileCount;
}

int GameProfile_GetCount(void) { return g_profileCount; }

GameProfile* GameProfile_GetByIndex(int idx)
{
    if (idx < 0 || idx >= g_profileCount) return NULL;
    return &g_profiles[idx];
}

GameProfile* GameProfile_FindById(const char* id)
{
    if (!id) return NULL;
    for (int i = 0; i < g_profileCount; i++) {
        if (_stricmp(g_profiles[i].id, id) == 0) return &g_profiles[i];
    }
    return NULL;
}

GameProfile* GameProfile_FindByExe(const char* exeBasename)
{
    if (!exeBasename || !exeBasename[0]) return NULL;

    char needle[GAME_PROFILE_EXE_LEN];
    strncpy(needle, exeBasename, sizeof(needle) - 1);
    needle[sizeof(needle) - 1] = '\0';
    StripExeSuffix(needle);
    StrLowerAscii(needle);

    for (int i = 0; i < g_profileCount; i++) {
        GameProfile* p = &g_profiles[i];
        if (!p->userEnabled) continue;
        for (int e = 0; e < p->exeCount; e++) {
            if (strcmp(needle, p->exes[e]) == 0) return p;
        }
    }
    return NULL;
}

GameProfile* GameProfile_GetForegroundMatch(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return NULL;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return NULL;

    char exePath[MAX_PATH];
    DWORD pathSize = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameA(hProc, 0, exePath, &pathSize);
    CloseHandle(hProc);
    if (!ok) return NULL;

    const char* slash = strrchr(exePath, '\\');
    const char* base = slash ? slash + 1 : exePath;
    return GameProfile_FindByExe(base);
}

BOOL GameProfile_ForegroundChanged(HWND* lastSeen)
{
    if (!lastSeen) return FALSE;
    HWND cur = GetForegroundWindow();
    if (cur == *lastSeen) return FALSE;
    *lastSeen = cur;
    return TRUE;
}

void GameProfile_SaveUserOverrides(const GameProfile* p)
{
    if (!p) return;

    char userConfigPath[MAX_PATH];
    GetUserConfigPath(userConfigPath, sizeof(userConfigPath));

    char section[GAME_PROFILE_ID_LEN + 16];
    snprintf(section, sizeof(section), "AutoClip.%s", p->id);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", p->userEnabled ? 1 : 0);
    WritePrivateProfileStringA(section, "Enabled", buf, userConfigPath);

    if (p->userHasRegion) {
        snprintf(buf, sizeof(buf), "%.4f", p->userRegionXPct);
        WritePrivateProfileStringA(section, "RegionXPct", buf, userConfigPath);
        snprintf(buf, sizeof(buf), "%.4f", p->userRegionYPct);
        WritePrivateProfileStringA(section, "RegionYPct", buf, userConfigPath);
        snprintf(buf, sizeof(buf), "%.4f", p->userRegionWPct);
        WritePrivateProfileStringA(section, "RegionWPct", buf, userConfigPath);
        snprintf(buf, sizeof(buf), "%.4f", p->userRegionHPct);
        WritePrivateProfileStringA(section, "RegionHPct", buf, userConfigPath);
    }

    if (p->userHasCooldown) {
        snprintf(buf, sizeof(buf), "%d", p->userCooldownSec);
        WritePrivateProfileStringA(section, "CooldownSec", buf, userConfigPath);
    }
}

BOOL GameProfile_GetActiveRegion(const GameProfile* p, float* outX, float* outY,
                                 float* outW, float* outH)
{
    if (!p) return FALSE;
    float x, y, w, h;
    if (p->userHasRegion) {
        x = p->userRegionXPct; y = p->userRegionYPct;
        w = p->userRegionWPct; h = p->userRegionHPct;
    } else {
        x = p->defaultRegionXPct; y = p->defaultRegionYPct;
        w = p->defaultRegionWPct; h = p->defaultRegionHPct;
    }
    if (w <= 0.0f || h <= 0.0f) return FALSE;
    if (outX) *outX = x;
    if (outY) *outY = y;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return TRUE;
}

int GameProfile_GetActiveCooldownSec(const GameProfile* p)
{
    if (!p) return AUTOCLIP_COOLDOWN_MIN_SEC;
    return p->userHasCooldown ? p->userCooldownSec : p->defaultCooldownSec;
}

void GameProfile_Shutdown(void)
{
    g_profileCount = 0;
    g_loaded = FALSE;
    memset(g_profiles, 0, sizeof(g_profiles));
}

---
name: lwsr-codebase
description: Architecture patterns and conventions for LWSR screen recorder
---

# LWSR Codebase Guide

## What's In This File

| Section | Contents |
|---------|----------|
| [File Ownership](#file-ownership) | Which file to modify for which feature |
| [Naming Conventions](#naming-conventions) | Function, control ID, and variable naming |
| [Common Patterns](#common-patterns) | UI creation, thread-safety, cleanup |
| [Threading Model](#threading-model) | Thread purposes and heartbeats |
| [Known Gotchas](#known-gotchas) | Pitfalls and their solutions |
| [Adding a New Setting](#adding-a-new-setting) | Step-by-step checklist |
| [Adding a New Tab Button Action](#adding-a-new-tab-button-action) | Step-by-step checklist |

---

## Quick Reference

**Language:** C (C11), Windows-only, MSVC
**UI Framework:** Raw Win32 (no frameworks)
**Video:** DXGI Desktop Duplication → NVENC HEVC → Custom MP4 muxer
**Audio:** WASAPI capture → Media Foundation AAC

## File Ownership

When modifying features, know which file owns what:

| Feature | Primary File | May Touch |
|---------|-------------|-----------|
| Mode buttons (Area/Window/Monitor) | `overlay.c` | - |
| Settings tabs/controls | `settings_dialog.c` | `overlay.c` for button labels |
| Recording start/stop | `overlay.c` → `recording.c` | - |
| Replay buffer | `replay_buffer.c` | `frame_buffer.c`, `nvenc_encoder.c` |
| Audio sources | `audio_capture.c` | `audio_device.c` for enumeration |
| Config persistence | `config.c` | - |

## Naming Conventions

### Functions
- Public: `Module_Action()` — e.g., `SettingsDialog_IsVisible()`, `ReplayBuffer_Stop()`
- Static/internal: `Action()` or `ModuleInternal_Action()`
- Callbacks: `Module_OnEvent()` — e.g., `ActionToolbar_OnRecord()`

### Control IDs (overlay.c)
```c
#define ID_MODE_AREA       1001   // Mode buttons: 1001-1003
#define ID_MODE_WINDOW     1002
#define ID_MODE_MONITOR    1003
#define ID_BTN_RECORD      1015   // Action buttons: 1015+
#define ID_CMB_FORMAT      1009   // Settings controls: various
```

### Globals
- `g_` prefix: `g_config`, `g_replayBuffer`, `g_isRecording`
- `s_` prefix for file-static: `s_settingsWnd`, `s_currentTab`

## Common Patterns

### UI Control Creation
```c
HWND ctl = CreateWindowW(L"COMBOBOX", L"",
    WS_CHILD | CBS_DROPDOWNLIST,
    x, y, width, height, hwnd, (HMENU)ID_xxx, hInstance, NULL);
SendMessage(ctl, WM_SETFONT, (WPARAM)font, TRUE);
AddToSection(ctl, s_xxxControls, &s_xxxControlCount);
```

### Thread-Safe State Checks
```c
// Reading a flag
if (InterlockedCompareExchange(&g_isRecording, 0, 0)) { ... }

// Setting a flag
InterlockedExchange(&g_isRecording, TRUE);
```

### Cleanup Pattern (goto cleanup)
```c
BOOL success = FALSE;
RESOURCE* res = NULL;

res = AllocateResource();
if (!res) goto cleanup;

// ... use resource ...
success = TRUE;

cleanup:
    SAFE_RELEASE(res);
    return success;
```

## Threading Model

| Thread | Purpose | Heartbeat ID |
|--------|---------|--------------|
| Main | Win32 message pump, UI | THREAD_MAIN |
| Buffer | Replay capture loop | THREAD_BUFFER |
| Recording | Traditional recording | THREAD_RECORDING |
| Watchdog | Deadlock detection | THREAD_WATCHDOG |
| Logger | Async log writes | (internal) |

**Critical:** Replay buffer and recording threads share GPU resources. Don't run both simultaneously.

## Known Gotchas

1. **Replay + Recording conflict**: Starting recording while replay buffer is active causes deadlock. The record button is disabled when replay buffer runs.

2. **Settings visibility check**: When settings dialog is open, mode buttons switch tabs instead of capture modes. Check `SettingsDialog_IsVisible()`.

3. **WM_SETREDRAW for anti-flicker**: Use `SendMessage(hwnd, WM_SETREDRAW, FALSE, 0)` before bulk UI changes, then `TRUE` + `RedrawWindow()`.

4. **NVENC cleanup is slow**: `ExitProcess(0)` used on close button to avoid 2-3 second NVIDIA driver cleanup.

5. **Window height changes**: When adding controls to a tab, update `SETTINGS_HEIGHT` in `settings_dialog.h`.

## Adding a New Setting

1. Add config field to `Config` struct in `config.h`
2. Add load/save in `config.c` (`Config_Load`, `Config_Save`)
3. Add control ID in `settings_dialog.c`
4. Create control in `CreateXxxSection()`
5. Add to appropriate `s_xxxControls` array
6. Handle `WM_COMMAND` for the control
7. Call `Config_MarkModified()` on change

## Adding a New Tab Button Action

1. Find `WM_COMMAND` handler in `overlay.c`
2. Check `SettingsDialog_IsVisible()` first
3. If settings open: switch tab with `SettingsDialog_SwitchTab()`
4. If settings closed: do normal action

## Debug Logging

```c
Logger_Log("Format string %d %s\n", intVal, strVal);
```

Logs go to `bin/Debug/lwsr_log_*.txt`. Check heartbeat status for thread health.

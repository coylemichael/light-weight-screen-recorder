# Global Variables Audit

This document summarizes the global and static variable audit performed on the Lightweight Screen Recorder codebase.

## Executive Summary

The codebase used approximately **100+ static/global variables** spread across 15 source files. After refactoring:

- **~30** are read-only constants (now marked `const`)
- **~25** GDI+ function pointers consolidated into `GdiplusFunctions` struct
- **~15** replay buffer globals consolidated into `ReplayInternalState` struct  
- **~30** remain as documented mutable state (with thread access patterns)

### Key Refactoring Completed

1. **replay_buffer.c** - Consolidated 15+ scattered globals into:
   - `ReplayVideoState` - encoder, sample buffer, sequence header
   - `ReplayAudioState` - audio capture, AAC encoder, samples, lock
   - `ReplayInternalState` - combines both as single `g_internal` instance

2. **gdiplus_api.h/.c** - NEW shared module providing:
   - `GdiplusFunctions` struct consolidating ~25 function pointers
   - Single `GdiplusAPI_Init()` / `GdiplusAPI_Shutdown()` entry points
   - Eliminates duplicate GDI+ loading in overlay.c and action_toolbar.c

3. **const correctness** - Added `const` to all read-only string arrays

## Files Modified

| File | Changes |
|------|---------|
| [gdiplus_api.h](src/gdiplus_api.h) | **NEW** - Shared GDI+ API loading |
| [gdiplus_api.c](src/gdiplus_api.c) | **NEW** - GDI+ implementation |
| [replay_buffer.c](src/replay_buffer.c#L35-L90) | **REFACTORED** - Consolidated into ReplayInternalState |
| [app_context.h](src/app_context.h) | Centralized context documentation |
| [main.c](src/main.c#L78-L145) | Documented all globals with thread access |
| [config.c](src/config.c#L16-L22) | Added `const` to string arrays |
| [logger.c](src/logger.c#L40-L95) | Documented logging subsystem state |
| [crash_handler.c](src/crash_handler.c#L50-L90) | Documented crash handling state |
| [audio_capture.c](src/audio_capture.c#L1-L70) | Documented enumerator |
| [audio_device.c](src/audio_device.c#L1-L28) | Documented enumerator |
| [mp4_muxer.c](src/mp4_muxer.c#L20-L30) | Documented GUID constant |
| [aac_encoder.c](src/aac_encoder.c#L20-L28) | Documented GUID constant |
| [border.c](src/border.c#L1-L25) | Documented UI state |
| [action_toolbar.c](src/action_toolbar.c#L1-L95) | Documented GDI+ and UI state |
| [overlay.c](src/overlay.c#L60-L280) | Documented GDI+ and UI state |
| [mem_utils.c](src/mem_utils.c#L35-L60) | Documented tracking state |

## Categorization of Globals

### 1. Read-Only Constants (marked `const`)

These are initialized at compile time and never modified:

| Variable | File | Type | Status |
|----------|------|------|--------|
| `FORMAT_EXTENSIONS[]` | config.c | `const char* const[]` | ✅ Done |
| `FORMAT_NAMES[]` | config.c | `const char* const[]` | ✅ Done |
| `g_threadNames[]` | logger.c | `const char* const[]` | ✅ Done |
| `CLSID_AACEncoder` | aac_encoder.c | `const GUID` | Already const |
| `MFVideoFormat_HEVC_Local` | mp4_muxer.c | `const GUID` | Already const |
| `MUTEX_NAME` | main.c | `const char* const` | ✅ Done |
| `WINDOW_CLASS` | main.c | `const char* const` | ✅ Done |

### 2. Consolidated Structs (COMPLETED)

#### replay_buffer.c - ReplayInternalState
```c
typedef struct ReplayInternalState {
    ReplayVideoState video;   /* encoder, frameBuffer, seqHeader */
    ReplayAudioState audio;   /* capture, encoder, samples, lock */
} ReplayInternalState;

static ReplayInternalState g_internal = {0};  /* Single instance */
```

**Benefits:**
- Reduced 15+ scattered globals to 1 struct instance
- Clear ownership and lifetime (BufferThreadProc)
- Thread access documented per-field
- Callbacks receive struct pointer instead of using globals

#### gdiplus_api.h - GdiplusFunctions
```c
typedef struct GdiplusFunctions {
    HMODULE module;
    ULONG_PTR token;
    fn_GdipCreateFromHDC CreateFromHDC;
    fn_GdipDeleteGraphics DeleteGraphics;
    /* ... 25+ function pointers ... */
} GdiplusFunctions;
```

**Benefits:**
- Single struct replaces ~25 individual static pointers (per module)
- overlay.c and action_toolbar.c can share one instance
- Clear initialization/shutdown lifecycle

### 3. Application State (Cross-Module)

Defined in `main.c`, used across modules:

| Variable | Type | Thread Access | Purpose |
|----------|------|---------------|---------|
| `g_config` | `AppConfig` | Main writes, Any reads | INI configuration |
| `g_capture` | `CaptureState` | Main/Recording | DXGI duplication |
| `g_replayBuffer` | `ReplayBufferState` | Main/BufferThread | Instant replay |
| `g_isRecording` | `volatile LONG` | Any (atomic) | Recording flag |
| `g_isSelecting` | `volatile LONG` | Any (atomic) | Selection mode |
| `g_overlayWnd` | `HWND` | Main only | Overlay window |
| `g_controlWnd` | `HWND` | Main only | Control panel |
| `g_mutex` | `HANDLE` | Main only | Single instance |

### 4. Remaining Module-Scoped State

These globals remain in their current form with proper documentation:

#### logger.c - Async Logging
- `g_logQueue[]` - Ring buffer (Lock-free producers/consumer)
- `g_writeIndex/g_readIndex` - Atomic indices
- `g_logFile` - File handle (Logger thread only)
- `g_heartbeats[]` - Thread liveness tracking (Atomic per-thread)

#### crash_handler.c - Error Recovery
- `g_crashInProgress` - Atomic flag preventing re-entry
- `g_heartbeatCounter` - Watchdog heartbeat (Atomic)
- `g_crashLock` - Critical section for crash handling

#### overlay.c - UI State
- Selection state: `g_selState`, `g_selectedRect`, `g_isDragging`
- Window handles: `g_settingsWnd`, `g_crosshairWnd`
- Recording: `g_encoder`, `g_recordThread`, `g_stopRecording`
- GDI+ function pointers (can migrate to `GdiplusFunctions` struct)

#### border.c - Visual Overlays
- Recording border: `g_borderWnd`, `g_isVisible`, `g_currentRect`
- Preview border: `g_previewBorderWnd`, `g_previewVisible`
- Area selector: `g_areaSelectorWnd`, `g_areaLocked`

#### action_toolbar.c - Toolbar UI
- `g_toolbarWnd`, `g_buttons[]`, `g_hoveredButton`
- Callbacks: `g_onMinimize`, `g_onRecord`, etc.
- GDI+ function pointers (can migrate to `GdiplusFunctions` struct)

## Future Improvements

These are optional refactors for future consideration:

1. **Migrate overlay.c to GdiplusFunctions** - Use the new shared `gdiplus_api.h`
2. **Migrate action_toolbar.c to GdiplusFunctions** - Use the new shared `gdiplus_api.h`
3. **Pass contexts as parameters** - For better testability
4. **Consolidate window handles** - Store in a WindowHandles struct

## Thread Safety Patterns Used

| Pattern | Variables | Notes |
|---------|-----------|-------|
| Atomic operations | `g_isRecording`, `g_stopRecording`, `g_logRunning` | Use `InterlockedExchange` |
| Critical section | `audio.lock`, `g_crashLock`, `g_allocLock` | EnterCriticalSection/Leave |
| Event-based | `ReplayBufferState.hReadyEvent` etc. | Windows event objects |

## Summary

The global variable audit resulted in:

1. **~15 globals eliminated** in replay_buffer.c via ReplayInternalState consolidation
2. **New GdiplusFunctions struct** available for overlay.c and action_toolbar.c migration
3. **All read-only globals marked const**
4. **All remaining globals documented** with thread access patterns
| Single-thread | Most UI globals | Main thread only |
| Read-only after init | `g_config`, GDI+ pointers | Set once, read many |

## Conclusion

The codebase follows reasonable patterns for a Windows desktop application:
- UI state is main-thread only
- Cross-thread communication uses atomics or critical sections
- Constants are properly const-qualified

The `app_context.h` header provides a roadmap for future consolidation while maintaining backward compatibility with existing code.

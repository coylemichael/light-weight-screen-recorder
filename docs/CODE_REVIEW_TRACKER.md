# Code Review Tracker

Track review status of each source file against our coding standards.

## Review Criteria

Each file is checked against:
- [CODING_BEST_PRACTICE.md](CODING_BEST_PRACTICE.md) - Patterns, YAGNI, resource management
- [FILE_MANIFEST.md](FILE_MANIFEST.md) - File header matches purpose
- [debugging.md](skills/debugging.md) - Error handling, tracing

## Review Checklist Per File

1. **Header** - Has docstring matching FILE_MANIFEST.md?
2. **Includes** - Every `#include` is used?
3. **Functions** - Every function has a caller?
4. **Resources** - Uses goto-cleanup pattern for 3+ resources?
5. **SAFE_* macros** - No raw free/Release in cleanup?
6. **Thread flags** - Uses `volatile LONG` + Interlocked, not BOOL?
7. **HRESULT** - Uses `FAILED()`, not `!= S_OK`?
8. **Allocations** - All checked for NULL?
9. **Integer overflow** - Cast to `size_t` before large multiplications?
10. **COM** - Proper CoInitializeEx/CoUninitialize?

---

## Core Pipeline

| File | Reviewed | R2 | Issues | Notes |
|------|:--------:|:--:|--------|-------|
| `main.c` | ✅ | ✅ | R1: 1, R2: 8 | R1: Docstring updated to match manifest. R2: Removed unused `<windowsx.h>`, `<d3d11.h>`, `<dxgi1_2.h>`, `<mfidl.h>`, `<mfreadwrite.h>`, `<mferror.h>`, `<stdlib.h>`, `comdlg32.lib` |
| `capture.c` | ✅ | ✅ | R1: 4, R2: 0 | R1: Docstring updated, removed unused `stdio.h`, `Capture_EnumMonitors`, `Capture_ReleaseFrame`. R2: Clean |
| `capture.h` | ✅ | ✅ | R1: 3, R2: 1 | R1: Docstring updated, removed unused `MonitorEnumProc`, `Capture_EnumMonitors`, `Capture_ReleaseFrame`. R2: Removed unused `capturing` field from CaptureState |
| `gpu_converter.c` | ✅ | ✅ | R1: 2, R2: 0 | R1: Docstring updated to match manifest; manifest updated (zero-copy, no CPU readback). R2: Clean - all includes used, goto-cleanup pattern, SAFE_RELEASE, FAILED() checks |
| `gpu_converter.h` | ✅ | ✅ | R1: 1, R2: 1 | R1: Docstring updated. R2: Removed unused `<d3d11_1.h>` |
| `encoder.c` | ✅ | ✅ | R1: 2, R2: 1 | R1: Docstring updated; removed unused `Encoder_GetOutputPath`; added to FILE_MANIFEST.md. R2: Removed unused `<mferror.h>` |
| `encoder.h` | ✅ | ✅ | R1: 2, R2: 0 | R1: Docstring updated; removed unused `Encoder_GetOutputPath` declaration. R2: Clean - all MF headers required by mfreadwrite.h dependencies, all functions called |
| `nvenc_encoder.c` | ✅ | ✅ | R1: 2, R2: 0 | R1: Removed unused `<stdio.h>`; removed 3 dead code stubs. R2: Clean - all includes used (leak_tracker.h, constants.h), all functions called from replay_buffer.c, proper goto-cleanup via NVENCEncoder_Destroy |
| `nvenc_encoder.h` | ✅ | ✅ | R1: 0, R2: 2 | R1: Docstring aligns with manifest (detailed OK). R2: Removed unused `NVENCEncoder_IsAvailable`, `NVENCEncoder_ForceCleanupLeaked` |
| `frame_buffer.c` | ✅ | ✅ | R1: 5, R2: 0 | R1: Removed unused `util.h`, `<stdio.h>`, `FrameBuffer_WriteToFile`, `FrameBuffer_Clear`; use `SAFE_FREE`. R2: Clean - all includes used, all functions called from replay_buffer.c, proper goto-cleanup pattern, SAFE_FREE in cleanup, size_t overflow protection |
| `frame_buffer.h` | ✅ | ✅ | R1: 2, R2: 0 | R1: Removed unused `FrameBuffer_WriteToFile`, `FrameBuffer_Clear` declarations. R2: Clean - all includes used (windows.h, nvenc_encoder.h for EncodedFrame, config.h for QualityPreset, mp4_muxer.h for MuxerSample, constants.h for MAX_SEQ_HEADER_SIZE), all functions called from replay_buffer.c |
| `mp4_muxer.c` | ✅ | ✅ | R1: 1, R2: 1 | R1: Removed unused `<stdio.h>`. R2: Removed unused `<mferror.h>` |
| `mp4_muxer.h` | ✅ | ✅ | R1: 0, R2: 0 | R1: Clean - docstring matches manifest, all types/functions used. R2: Clean - windows.h for Win types, config.h for QualityPreset, all structs used by replay_buffer.c/frame_buffer.c, both functions called |
| `replay_buffer.c` | ✅ | ✅ | R1: 5, R2: 1 | R1: Removed unused `<stdio.h>`, `<objbase.h>`, `ReplayBuffer_Save`, `ReplayBuffer_IsSaving`, `ReplayBuffer_GetStatus`. R2: Removed dead `ALLOW_TERMINATE_THREAD` code (~50 lines) - TerminateThread approach doesn't work for NVENC |
| `replay_buffer.h` | ✅ | ✅ | R1: 3, R2: 2 | R1: Removed unused `ReplayBuffer_Save`, `ReplayBuffer_IsSaving`, `ReplayBuffer_GetStatus` declarations. R2: Removed unused enum values `REPLAY_STATE_SAVING`, `REPLAY_STATE_RECOVERING` |

## Audio

| File | Reviewed | R2 | Issues | Notes |
|------|:--------:|:--:|--------|-------|
| `audio_device.c` | ✅ | ✅ | R1: 3, R2: 3 | R1: Removed unused `<stdio.h>`, `AudioDevice_GetDefaultOutput`, `AudioDevice_GetDefaultInput`. R2: Added missing `logger.h` include, updated docstring to match manifest, wired `AudioDevice_Shutdown` into main.c cleanup |
| `audio_device.h` | ✅ | ✅ | R1: 2, R2: 0 | R1: Removed unused `AudioDevice_GetDefaultOutput`, `AudioDevice_GetDefaultInput` declarations. R2: Clean - `<windows.h>` required for BOOL, all types/enums/functions used |
| `audio_capture.c` | ✅ | ✅ | R1: 4, R2: 2 | R1: Docstring updated; removed unused `<stdio.h>`, `<math.h>`, `<limits.h>`; made `AudioCapture_GetTimestamp` static; removed unused `AudioCapture_HasData`. R2: Removed unused `<functiondiscoverykeys_devpkey.h>`; wired `AudioCapture_Shutdown` into main.c cleanup (was leaking g_audioEnumerator) |
| `audio_capture.h` | ✅ | ✅ | R1: 3, R2: 1 | R1: Docstring updated; removed unused `AudioCapture_GetTimestamp`, `AudioCapture_HasData` declarations. R2: Removed unused `"audio_device.h"` include (moved to .c file where needed) |
| `aac_encoder.c` | ✅ | ✅ | R1: 3, R2: 1 | R1: Removed unused `<stdio.h>`, `AACEncoder_Create`, `AACEncoder_Flush`. R2: Removed unused `<wmcodecdsp.h>` (CLSID defined locally) |
| `aac_encoder.h` | ✅ | ✅ | R1: 4, R2: 1 | R1: Docstring updated; removed unused `audio_capture.h`, `AACEncoder_Create`, `AACEncoder_Flush` declarations. R2: Removed unused `"constants.h"` |

## UI

| File | Reviewed | R2 | Issues | Notes |
|------|:--------:|:--:|--------|-------|
| `overlay.c` | ✅ | ✅ | R1: 8, R2: 2 | R1: Removed unused `<commdlg.h>`, `comdlg32.lib`; removed 7 dead functions: `UpdateCrosshair`, `CaptureToClipboard`, `CaptureToFile`, `LoadIconFromPNG`, `LoadPNGImage`, `DrawPNGImage`, `FreePNGImage`, `LoadSettingsIcon`, `UpdateActionToolbar`; removed unused `g_settingsImage`. R2: Wired `Overlay_Destroy` into main.c cleanup (was never called - leaked tray icon, windows, ActionToolbar, Border); removed unused `Overlay_GetWindow` |
| `overlay.h` | ✅ | ✅ | R1: 3, R2: 5 | R1: Docstring updated; removed unused `Overlay_ShowSelection`, `Overlay_ShowControls` declarations. R2: Removed unused `"config.h"` include; removed 4 internal-only declarations (made static in .c): `Overlay_SetMode`, `Overlay_GetSelectedRegion` (dead code - never called), `Overlay_SetRecordingState`, `Recording_Start`, `Recording_Stop` |
| `action_toolbar.c` | ✅ | ✅ | R1: 2, R2: 0 | R1: Removed unused `<dwmapi.h>`; removed unused `ActionToolbar_GetWindow`. R2: Clean - all 3 includes used, all 5 public functions called from overlay.c, proper goto-cleanup for GDI resources |
| `action_toolbar.h` | ✅ | ✅ | R1: 2, R2: 0 | R1: Docstring updated; removed unused `ActionToolbar_GetWindow` declaration. R2: Clean - `<windows.h>` needed for BOOL/HINSTANCE, all 5 functions called from overlay.c |
| `border.c` | ✅ | ✅ | R1: 4, R2: 1 | R1: Removed unused `<stdlib.h>`, `UpdatePreviewBorderBitmap`, `PreviewBorder_Show`, `Border_IsVisible`. R2: Updated docstring to match FILE_MANIFEST |
| `border.h` | ✅ | ✅ | R1: 3, R2: 1 | R1: Removed unused `PREVIEW_BORDER_THICKNESS`, `PreviewBorder_Show`, `Border_IsVisible` declarations. R2: Updated docstring to match FILE_MANIFEST |

## Infrastructure

| File | Reviewed | R2 | Issues | Notes |
|------|:--------:|:--:|--------|-------|
| `config.c` | ✅ | ✅ | R1: 2, R2: 1 | R1: Removed unused `Config_GetFormatName` function and `FORMAT_NAMES` array. R2: Made `Config_GetPath` static (only used internally) |
| `config.h` | ✅ | ✅ | R1: 1, R2: 1 | R1: Removed unused `Config_GetFormatName` declaration. R2: Removed unused `Config_GetPath` declaration (made static in .c) |
| `logger.c` | ✅ | ✅ | R1: 4, R2: 1 | R1: Removed unused `Logger_LogThread`, `Logger_IsThreadStalled`, `Logger_GetThreadName`, `Logger_GetHeartbeatAge`. R2: Removed unused `AUDIO_SRC2` from g_threadNames (all 3 source threads share AUDIO_SRC) |
| `logger.h` | ✅ | ✅ | R1: 4, R2: 1 | R1: Removed unused declarations for above 4 functions. R2: Removed unused `THREAD_AUDIO_SRC2` (all 3 audio source threads share `THREAD_AUDIO_SRC1`) |
| `util.c` | ✅ | ✅ | R1: 2, R2: 1 | R1: Removed unused `Util_CalculateTimestamp`, `Util_CalculateFrameDuration`. R2: Removed unused `"logger.h"` include (LWSR_ASSERT is in constants.h via util.h) |
| `util.h` | ✅ | ✅ | R1: 2, R2: 1 | R1: Removed unused declarations for above 2 functions. R2: Removed unused `"constants.h"` include |
| `constants.h` | ✅ | ✅ | R1: 2, R2: 13 | R1: Removed unused `BYTES_PER_PIXEL_RGB24`, `CONFIG_BUFFER_SIZE`. R2: Removed 13 unused constants: `MF_UNITS_PER_MILLISECOND`, `QP_INTRA_OFFSET`, `NVENC_NUM_BUFFERS`, `AUDIO_DEVICE_ID_MAX_LEN`, `AUDIO_VOLUME_MIN`, `AUDIO_VOLUME_MAX`, `MUTEX_ACQUIRE_TIMEOUT_MS`, `EVENT_WAIT_TIMEOUT_MS`, `THREAD_JOIN_TIMEOUT_MS`, `LOG_RATE_LIMIT`, `CONTROL_WINDOW_TOP_OFFSET`, `GDIP_STROKE_OFFSET`, `OVERLAY_HIDE_SETTLE_MS`; removed duplicate `AAC_LC_PROFILE_LEVEL`/`AAC_OUTPUT_BUFFER_SIZE` definitions; added missing `constants.h` include to util.c |
| `main.h` | ✅ | ✅ | R1: 0, R2: 4 | R1: (was app_context.h) Clean. R2: Renamed from app_context.h; removed 4 unused structs (AppContext, RecordingContext, ReplayAudioContext, ReplayVideoContext) - YAGNI scaffolding for never-implemented architecture; slimmed from 175 to 22 lines |

## Safety

| File | Reviewed | R2 | Issues | Notes |
|------|:--------:|:--:|--------|-------|
| `mem_utils.c` | ✅ | ⬜ | 0 fixed | Clean - optional debug code under LWSR_DEBUG_MEMORY |
| `mem_utils.h` | ✅ | ⬜ | 0 fixed | Clean - unused macros (CHECK_ALLOC, MF_LOCK_BUFFER etc) are documented utility patterns |
| `leak_tracker.c` | ✅ | ⬜ | 2 fixed | Removed unused `LeakTracker_Reset`, `LeakTracker_HasPotentialLeak` |
| `leak_tracker.h` | ✅ | ⬜ | 2 fixed | Removed unused declarations for above functions |
| `crash_handler.c` | ✅ | ⬜ | 0 fixed | Clean - unused `CrashHandler_ForceCrash` is debug/test utility |
| `crash_handler.h` | ✅ | ⬜ | 0 fixed | Clean |

## External / Vendor

| File | Reviewed | R2 | Issues | Notes |
|------|:--------:|:--:|--------|-------|
| `gdiplus_api.c` | ✅ | ⬜ | 0 fixed | Clean - runtime DLL loading wrapper |
| `gdiplus_api.h` | ✅ | ⬜ | 0 fixed | Clean - GDI+ type definitions and function table |
| `audio_guids.h` | ✅ | ✅ | R1: 0, R2: 1 | R1: Clean. R2: Removed dead code - identical #ifdef/#else branches and unused DEFINE_AUDIO_GUIDS pattern; slimmed from 52 to 25 lines |
| `nvEncodeAPI.h` | ✅ | ⬜ | 0 fixed | Clean - NVIDIA SDK vendor header (do not modify) |

## Resources

| File | Reviewed | Issues | Notes |
|------|:--------:|--------|-------|
| `lwsr.rc` | ⬜ | - | Windows resource file (icon, manifest, version) |

---

## Legend

- ⬜ = Not reviewed
- 🔄 = In progress
- ✅ = Reviewed, compliant
- ⚠️ = Reviewed, has issues (see Issues column)

---

## Progress Summary

- **Total files:** 46
- **Reviewed:** 45
- **Compliant:** 45
- **Has issues:** 0
- **Remaining:** 1 (`lwsr.rc`)

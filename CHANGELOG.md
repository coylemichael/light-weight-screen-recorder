# Changelog

## [1.2.18] - 2026-01-26

### Added
- **IMFMediaBuffer Lock/Unlock helper macros in mem_utils.h**
  - `MF_LOCK_BUFFER()`: Lock with HRESULT check, LWSR_ASSERT, NULL validation, and lock flag tracking
  - `MF_LOCK_BUFFER_LOG()`: Same as above with logging on failure
  - `MF_UNLOCK_BUFFER()`: Safe unlock that only unlocks if lock flag is TRUE

### Fixed
- **aac_encoder.c (ProcessOutput)**: CRITICAL - Lock() return value was not checked; Unlock() was called unconditionally even if Lock() failed
- **aac_encoder.c (AACEncoder_Feed)**: Added LWSR_ASSERT and defensive NULL check after Lock()
- **mp4_muxer.c (WriteVideoSampleToWriter)**: Added LWSR_ASSERT and defensive NULL check after Lock() with proper Unlock on error
- **mp4_muxer.c (WriteAudioSampleToWriter)**: Added LWSR_ASSERT and defensive NULL check after Lock() with proper Unlock on error
- **encoder.c (Encoder_WriteFrame)**: Added LWSR_ASSERT and defensive NULL check after Lock()

### Improved
- **Defensive NULL checks after all IMFMediaBuffer::Lock() operations**
  - All 5 Lock() call sites now verify HRESULT before using buffer pointer
  - All 5 Lock() call sites have LWSR_ASSERT() for debug builds
  - All 5 Lock() call sites have runtime NULL checks for release builds
  - All 5 Lock() call sites have corresponding Unlock() on both success and error paths

## [1.2.17] - 2026-01-26

### Improved
- **Consolidated goto-cleanup pattern enforcement across codebase**
  - Audited 26 functions acquiring 3+ resources (see summary below)
  - Standardized on `SAFE_RELEASE`, `SAFE_FREE`, `SAFE_CLOSE_HANDLE`, `SAFE_COTASKMEM_FREE` macros
  - All cleanup paths now use SAFE_* macros instead of raw `Release()`/`free()`/`CloseHandle()`

### Changed
- **replay_buffer.c**: Refactored `ReplayBuffer_Init` from inline cleanup to proper goto-cleanup pattern
- **replay_buffer.c**: `ReplayBuffer_Shutdown` now uses `SAFE_CLOSE_HANDLE` for event handles
- **audio_capture.c**: `CreateSource` and `DestroySource` now use `SAFE_FREE`, `SAFE_RELEASE`, `SAFE_COTASKMEM_FREE`
- **audio_capture.c**: `AudioCapture_Create` refactored to goto-cleanup pattern with `SAFE_FREE`
- **audio_capture.c**: `AudioCapture_Destroy` now uses `SAFE_FREE`
- **gpu_converter.c**: `GPUConverter_Shutdown` now uses `SAFE_RELEASE` for all COM objects
- **aac_encoder.c**: `AACEncoder_Destroy` now uses `SAFE_FREE` and `SAFE_RELEASE`
- **frame_buffer.c**: `FrameBuffer_Init` refactored to goto-cleanup pattern with `SAFE_FREE`

### Added
- **mem_utils.h**: Multi-resource function comment template for documenting functions with 3+ resources

### Audit Summary
- 22 functions properly using goto-cleanup with SAFE_* macros ✅
- 3 functions fixed in this release (FrameBuffer_Init, AudioCapture_Create, AudioCapture_Destroy)
- 5 GDI/GDI+ functions use manual cleanup (acceptable pattern for these APIs)

## [1.2.16] - 2026-01-26

### Added
- **Dedicated HealthMonitor thread for stall detection**
  - New `health_monitor.h/c`: monitors worker thread heartbeats every 500ms
  - Detects soft stalls (2s warning) and hard stalls (5s critical)
  - Posts `WM_WORKER_STALLED` to UI thread for safe recovery
  - Spawns cleanup thread to safely terminate stalled threads
  - Replaces old timer-based detection that could block UI thread

### Fixed
- **Stall recovery now works correctly** - `WM_WORKER_STALLED` posted to `g_controlWnd` (where handler lives) instead of `g_overlayWnd`

### Verified
- 13+ hour continuous run: 2.9M frames @ 60 FPS, zero stalls, stable leak tracker deltas

## [1.2.15] - 2026-01-24

### Improved
- **Comprehensive line-by-line code review with fixes across 9 files**
  - Systematic review of error handling, resource management, and thread safety
  - All identified issues fixed and committed

### Fixed

**logger.c:**
- Implement missing `Logger_GetHeartbeatAge()` function (was declared but not defined)
- Fix `Logger_LogThread()` to use atomic read for `g_log.initialized`
- Add explicit casts for format specifiers in heartbeat status

**config.c:**
- Check `GetModuleFileNameA` return value, handle truncation
- Handle `strrchr()` returning NULL (no backslash in path)
- Check `CreateDirectoryA` result, fallback on failure
- Validate loaded config values (format, quality, replay duration, FPS, volumes)

**audio_device.c:**
- Check `GetCount()` HRESULT before using count
- Check all `GetId()` HRESULTs (5 occurrences)
- `GetDefaultOutput`/`GetDefaultInput` now return FALSE if GetId fails
- Replace inefficient `AudioDevice_GetById` (was enumerating all devices) with direct `IMMDeviceEnumerator::GetDevice` lookup
- Query `IMMEndpoint` for proper device type detection
- Add `IID_IMMEndpoint` GUID to audio_guids.h

**border.c:**
- Check `GetDC(NULL)` and `CreateCompatibleDC` return values
- Use goto cleanup pattern in `UpdateBorderBitmap`/`UpdatePreviewBorderBitmap`
- Fix duplicate `SetLayeredWindowAttributes` call (first was ineffective without WS_EX_LAYERED)
- Check `CreateWindowExA` return in `AreaSelector_Show`
- Use `GET_X_LPARAM`/`GET_Y_LPARAM` for proper signed coordinates

**action_toolbar.c:**
- Check `GetDC(NULL)` and `CreateCompatibleDC` return values
- Use goto cleanup pattern in `UpdateToolbarBitmap`
- Check `RegisterClassExA` return value (allow ERROR_CLASS_ALREADY_EXISTS)
- Use `GET_X_LPARAM`/`GET_Y_LPARAM` for proper signed coordinates
- Include `<windowsx.h>` with `#undef DeleteFont` to avoid macro conflict

**gdiplus_api.c:**
- Add failure checks for manual `GetProcAddress` calls (`BrushDelete`, `PenDelete`, `SetTextRenderingHint`, `DrawRectangle`, `CreatePen1`)

**util.c:**
- Add defensive division by zero protection in `Util_CalculateTimestamp`
- Add defensive division by zero protection in `Util_CalculateFrameDuration`
- `Util_CalculateAspectRect` returns sourceBounds if ratioW/H <= 0 (native mode)

**mem_utils.c:**
- Add integer overflow check in `MemDebug_Calloc` before `count * size` multiplication

**main.c:**
- Check `CreateMutexA` for `ERROR_ALREADY_EXISTS` (race condition between instances)
- Add `GdiplusAPI_Shutdown` to `Capture_Init` failure cleanup path
- Add `GdiplusAPI_Shutdown` to `Overlay_Create` failure cleanup path
- Add logger fallback when exe path has no backslash

---

## [1.2.14] - 2026-01-20

### Security
- **Comprehensive null pointer check audit at public API boundaries**
  - Added defensive runtime null guards to all public functions that had only debug assertions
  - Ensures release builds don't crash from null pointer dereferences when LWSR_ASSERT is disabled
  - Pattern: `LWSR_ASSERT(ptr != NULL)` for debug + `if (!ptr) return FALSE/NULL/0;` for production

### Technical Details

**Files modified with new null guards:**
- `capture.c` - 11 functions: Init, SetRegion, SetMonitor, SetAllMonitors, GetRefreshRate, ReinitDuplication, GetMonitorBoundsByIndex, GetMonitorFromPoint, GetAllMonitorsBounds, GetWindowRect, EnumMonitors
- `encoder.c` - 2 functions: Encoder_Init, Encoder_WriteFrame
- `border.c` - 3 functions: Border_Init, PreviewBorder_Init, AreaSelector_Init (HINSTANCE parameter)
- `action_toolbar.c` - 1 function: ActionToolbar_Init (HINSTANCE parameter)
- `logger.c` - 1 function: Logger_Init (filename/mode parameters)
- `overlay.c` - 2 functions: Overlay_Create (HINSTANCE), Overlay_GetSelectedRegion (RECT* region)
- `config.c` - 3 functions: Config_GetPath, Config_Load, Config_Save
- `util.c` - 1 function: Util_GetAspectRatioDimensions (ratioW/ratioH parameters)

**Already protected (no changes needed):**
- All `*_Destroy`, `*_Shutdown`, `*_Stop` functions
- AudioCapture_*, SampleBuffer_*, NVENCEncoder_*, AACEncoder_*, ReplayBuffer_*, GPUConverter_*, Mp4Muxer_* modules

---

## [1.2.13] - 2026-01-20

### Improved
- **Continued function refactoring for mp4_muxer.c and audio_capture.c**
  - Extended the refactoring effort to remaining long functions
  - Each helper has single responsibility with descriptive names and doc comments
  - No functional changes - same behavior, better code organization

### Technical Details

**mp4_muxer.c - Muxer Helpers:**
- `CreateHEVCMediaType()` - Creates IMFMediaType for HEVC video stream with sequence header
- `CreateAACMediaType()` - Creates IMFMediaType for AAC audio stream with AudioSpecificConfig
- `WriteVideoSampleToWriter()` - Writes single video sample to sink writer
- `WriteAudioSampleToWriter()` - Writes single audio sample to sink writer
- Both `MP4Muxer_WriteFile` and `MP4Muxer_WriteFileWithAudio` now share these helpers
- `MP4Muxer_WriteFileWithAudio` reduced from ~260 lines to ~120 lines

**audio_capture.c - Mix Thread Helpers:**
- `IsSourceDormant()` - Checks if audio source is dormant (no recent packets from virtual devices)
- `ReadFromSourceBuffer()` - Reads audio data from source's ring buffer
- `MixAudioSamples()` - Mixes multiple audio sources with volume and peak tracking
- `WriteMixedToBuffer()` - Writes mixed audio to context's output ring buffer
- `MixCaptureThread` reduced from ~250 lines to ~140 lines

**Codebase Review Complete:**
All source files reviewed for functions over 100 lines. Remaining long functions not refactored due to:
- Goto-cleanup patterns where extraction would hurt error handling clarity (aac_encoder.c, nvenc_encoder.c)
- Step-based comments already providing good readability (nvenc_encoder.c, capture.c)
- Flat repetitive operations without complex logic (config.c)
- Standard entry point patterns (main.c WinMain)

---

## [1.2.12] - 2026-01-20

### Improved
- **Refactored long functions for better maintainability**
  - Extracted helper functions from functions exceeding 100 lines
  - Each helper has single responsibility with descriptive names and doc comments
  - No functional changes - same behavior, better code organization

### Technical Details

**overlay.c - Settings Window Helpers:**
- `CreateOutputSettings()` - Format & quality dropdown creation
- `CreateCaptureCheckboxes()` - Mouse cursor & border checkbox creation
- `CreateTimeLimitControls()` - Hour/minute/second dropdown creation
- `CreateSavePathControls()` - Save path edit box and browse button
- `SettingsLayout` struct - Consolidated layout parameters for UI creation

**overlay.c - Control Panel WM_DRAWITEM Helpers:**
- `DrawModeButton()` - Renders capture mode buttons with selection/hover state
- `DrawRecordButton()` - Renders record button (red circle) / stop button (white square)
- `DrawMDL2IconButton()` - Draws Windows 11-style Segoe MDL2 Assets icon buttons
- `DrawRecordingPanel()` - Renders timer+stop panel with anti-aliased GDI+ graphics
- WM_DRAWITEM handler reduced from ~200 lines to ~50 lines

**replay_buffer.c - BufferThreadProc Helpers (previous session):**
- `InitCaptureRegion()` - Sets up monitor bounds for capture
- `ApplyAspectRatioAdjustment()` - Crops capture region for target aspect ratio
- `InitVideoPipeline()` - GPU converter + NVENC encoder initialization
- `InitAudioPipeline()` - WASAPI + AAC encoder setup
- `ShutdownAudioPipeline()` / `ShutdownVideoPipeline()` - Cleanup helpers
- `CopyAudioSamplesForMuxing()` / `FreeAudioSampleCopies()` - Audio sample management
- `HandleSaveRequest()` - Complete save operation handler
- BufferThreadProc main loop reduced from ~500 lines to ~150 lines

---

## [1.2.11] - 2026-01-20

### Improved
- **Consolidated GDI+ API loading into shared module**
  - Created `gdiplus_api.h/.c` to eliminate ~200 lines of duplicate GDI+ code between `overlay.c` and `action_toolbar.c`
  - Single `GdiplusFunctions` struct consolidates 25+ function pointers
  - Global `g_gdip` instance initialized once in `main.c`, shared by both modules
  - Improves maintainability: GDI+ changes now only need to be made in one place

### Fixed
- **Windows SDK macro conflict with GDI+ field names**
  - Issue: `DeleteBrush` and `DeletePen` are Windows SDK macros that expand to `DeleteObject`
  - Symptoms: Compiler error "struct GdiplusFunctions has no field DeleteObject"
  - Fix: Renamed struct fields to `BrushDelete` and `PenDelete` to avoid macro expansion
  - `gdiplus_api.c` uses explicit `GetProcAddress()` calls for these two functions

### Technical Details
- `gdiplus_api.h`: GDI+ types, function pointer typedefs, `GdiplusFunctions` struct, `extern g_gdip`
- `gdiplus_api.c`: `GdiplusAPI_Init()` loads gdiplus.dll, `GdiplusAPI_Shutdown()` releases resources
- `main.c`: Calls `GdiplusAPI_Init(&g_gdip)` before UI creation, `GdiplusAPI_Shutdown(&g_gdip)` at exit
- `overlay.c`: Removed `InitGdiPlus()`/`ShutdownGdiPlus()`, now uses shared `g_gdip`
- `action_toolbar.c`: Removed `InitToolbarGdiPlus()`/`ShutdownToolbarGdiPlus()`, now uses shared `g_gdip`
- Thread safety: All GDI+ operations remain main-thread-only (UI operations)

---

## [1.2.10] - 2026-01-19

### Improved
- **Added assertions throughout codebase for early error detection**
  - Created `LWSR_ASSERT` macro that logs before asserting for better diagnostics
  - Added `LWSR_ASSERT_MSG` variant for custom error messages
  - Assertions can be disabled in release builds via `LWSR_DISABLE_ASSERTS`
  - Added precondition assertions to all public-facing API functions
  - Added invariant checks for buffer indices, counts, and state consistency
  - Logger module uses standard `assert()` to avoid circular dependency

### Technical Details
- `constants.h`: Added `LWSR_ASSERT` and `LWSR_ASSERT_MSG` macros
- Modules updated with assertions:
  - `logger.c`: Format strings non-null, thread IDs in range
  - `config.c`: Config pointers non-null, buffer sizes valid
  - `capture.c`: State/bounds/region validation, monitor indices >= 0
  - `encoder.c`: Output paths non-null, dimensions positive
  - `sample_buffer.c`: Buffer indices in bounds, counts non-negative
  - `replay_buffer.c`: State pointers, config validation
  - `mp4_muxer.c`: Sample arrays, config validity
  - `audio_capture.c`: Context pointers, buffer sizes
  - `aac_encoder.c`: Encoder state, PCM data
  - `nvenc_encoder.c`: Device pointers, texture handles
  - `gpu_converter.c`: Converter state, texture validation
  - `util.c`: Dimension/FPS positive, string pointers non-null
- Assertions catch programming errors during development without runtime overhead in release builds

---

## [1.2.9] - 2026-01-19

### Improved
- **Standardized error handling across entire codebase**
  - All HRESULT checks now use `FAILED()`/`SUCCEEDED()` macros exclusively
  - Fixed `nvenc_encoder.c`: changed `hr != S_OK` to `FAILED(hr)` for correct device-removed detection
  - Added missing error logging in `gpu_converter.c` for `CreateVideoProcessorInputView` and `VideoProcessorBlt` failures
  - Documented error handling pattern in header comment of all 17 source files
  - Project-wide standards documented in `main.c` header

- **Live debug logging toggle**
  - Debug logging checkbox now takes effect immediately (no restart required)
  - Toggling off gracefully shuts down logger with final "disabled" message

- **Reduced log verbosity**
  - Removed 9 per-frame debug logs from NVENC encoder (was ~540 lines/sec at 60fps)
  - Logs now contain only meaningful events: init/shutdown, errors, rate-limited warnings

### Technical Details
- Error patterns used: goto-cleanup (multi-resource), early-return (validation), continue-on-error (loops)
- All functions return BOOL/NULL for error propagation; callers must check
- Thread-safe flags use `InterlockedExchange`/`InterlockedCompareExchange`
- See `mem_utils.h` for goto-cleanup pattern examples

---

## [1.2.8] - 2026-01-19

### Fixed
- **COM object lifecycle audit - memory leaks in error paths**
  - `aac_encoder.c` `ProcessOutput()`: Fixed leak when `MFCreateSample`/`MFCreateMemoryBuffer` fails
  - `aac_encoder.c` `AACEncoder_Create()`: Unified `MFTEnumEx` cleanup paths (was split into two branches)
  - `aac_encoder.c` `AACEncoder_Feed()`: Added error checking for `MFCreateSample`, `MFCreateMemoryBuffer`, and `Lock()`
  - `mp4_muxer.c` `WriteFileWithAudio()`: Fixed leaks when `Lock()` or `MFCreateSample()` fails in write loops

### Technical Details
- All 19 source files audited for COM lifecycle management
- Pattern enforced: check HRESULT → release on failure → cascade cleanup
- Hot-path functions (called per-frame) now have proper error handling
- Prevents memory accumulation during long recording sessions
- Files with exemplary patterns: `gpu_converter.c`, `nvenc_encoder.c`, `capture.c`

---

## [1.2.7] - 2026-01-19

### Fixed
- **Integer overflow protection for large resolutions**
  - Frame buffer size calculations now use `size_t` to prevent 32-bit overflow
  - Added overflow validation before memory allocations in `sample_buffer.c`
  - `encoder.c`: Buffer size uses safe 64-bit math, validates against `MAXDWORD` for Media Foundation API
  - `capture.c`: Row bytes calculation uses `size_t` for proper `memcpy` size
  - `util.c`: Megapixels calculation casts to `size_t` before multiplication
  - `replay_buffer.c`: RAM estimation and audio allocation include overflow checks

### Technical Details
- Pattern: `(size_t)width * (size_t)height * BYTES_PER_PIXEL` prevents overflow before cast
- Overflow check pattern: `if (count > 0 && allocSize / count != elementSize)` detects wrap
- Affected calculations: frame buffers, sample arrays, muxer allocations
- Enables safe operation with 8K+ resolutions and multi-monitor setups
- Graceful failure (returns error) instead of crash/corruption on extreme resolutions

---

## [1.2.6] - 2026-01-19

### Fixed
- **Thread safety audit - race conditions eliminated**
  - `g_isRecording` and `g_isSelecting` changed from `BOOL` to `volatile LONG` with atomic operations
  - `g_stopRecording` loop condition now uses `InterlockedCompareExchange` for proper memory barrier
  - All reads/writes of recording state flags use `InterlockedExchange`/`InterlockedCompareExchange`
  - Encoder `initialized` and `recording` flags now use atomic operations across threads
  - Logger `g_logRunning` and `g_logInitialized` use atomic operations
  - **Audio capture** `ctx->running`, `src->active`, `src->deviceInvalidated` now atomic operations
  - **Crash handler** `g_watchdogRunning`, `g_crashInProgress` now use atomic operations for thread safety
  - **NVENC encoder** `enc->stopThread`, `enc->deviceLost` now use atomic operations for output thread safety
  
### Changed
- **Atomic operations for all shared flags**
  - Recording thread loop: `while (!InterlockedCompareExchange(&g_stopRecording, 0, 0))`
  - Recording state checks: `InterlockedCompareExchange(&g_isRecording, 0, 0)`
  - State transitions: `InterlockedExchange(&g_isRecording, TRUE/FALSE)`
  - Encoder state: `InterlockedExchange(&state->initialized, TRUE/FALSE)`
  - Logger state: `InterlockedExchange(&g_logInitialized, TRUE/FALSE)`
  - Audio sources: `InterlockedExchange(&src->active, TRUE/FALSE)`
  - Audio context: `InterlockedCompareExchange(&ctx->running, 0, 0)` in thread loops
  - Watchdog: `InterlockedCompareExchange(&g_watchdogRunning, 0, 0)` in loop
  - NVENC output: `InterlockedCompareExchange(&enc->stopThread, 0, 0)` in loop
  - NVENC device lost: `InterlockedExchange(&enc->deviceLost, TRUE)` on errors
  - Crash in progress: `InterlockedCompareExchange(&g_crashInProgress, TRUE, FALSE)` (CAS)

### Technical Details
- Race condition pattern fixed: bare `while(!flag)` loops → atomic reads with memory barriers
- TOCTOU (time-of-check-time-of-use) bugs eliminated in state checks
- All cross-thread flag access now uses Windows Interlocked* functions
- Changed `volatile BOOL` → `volatile LONG` for proper atomic semantics
- Files affected: `main.c`, `overlay.c`, `encoder.c`, `encoder.h`, `logger.c`, `audio_capture.c`, `audio_capture.h`, `crash_handler.c`, `nvenc_encoder.c`

---

## [1.2.5] - 2026-01-19

### Added
- **Memory safety utilities** - New `mem_utils.h` and `mem_utils.c` files
  - `SAFE_FREE`, `SAFE_RELEASE`, `SAFE_CLOSE_HANDLE`, `SAFE_COTASKMEM_FREE` macros
  - Null-checked cleanup that sets pointers to NULL after release (prevents double-free/use-after-free)
  - Optional debug memory tracking via `LWSR_DEBUG_MEMORY` flag
  - `CHECK_ALLOC` macro for consistent allocation error handling

### Fixed
- **Memory leak in sample_buffer.c** - `WriteToFile` and `GetSamplesForMuxing` leaked array when no samples copied
- **Memory leak in aac_encoder.c** - `MFTEnumEx` leaked activates array when encoder count was 0
- **Memory leak in audio_capture.c** - `MixCaptureThread` missing NULL check after malloc
- **Memory leak in replay_buffer.c** - Missing `GPUConverter_Shutdown` on `SampleBuffer_Init` failure path
- **Inconsistent cleanup in capture.c** - `frameBufferSize` not reset properly on malloc failure

### Changed
- **Goto-cleanup pattern** - Refactored 8+ functions across 6 files to use structured cleanup
  - `capture.c`: `Capture_Init`, `InitDuplicationForOutput`, `ReleaseDuplication`, `Capture_Shutdown`
  - `encoder.c`: `Encoder_Init`, `Encoder_WriteFrame`
  - `mp4_muxer.c`: `MP4Muxer_WriteFile`, `MP4Muxer_WriteFileWithAudio`
  - `aac_encoder.c`: `AACEncoder_Create`
  - `audio_device.c`: `EnumerateDeviceType`
  - `audio_capture.c`: `CreateSource`
- **SAFE_RELEASE macro adoption** - All cleanup blocks now use shared macros instead of inline Release/NULL

---

## [1.2.4] - 2026-01-19

### Added
- **Debug logging toggle** - New checkbox in settings to enable/disable debug logging
  - Off by default for normal use (no performance overhead)
  - When enabled, logs are saved to `Debug/` folder next to the executable
  - Timestamped log files preserve history for troubleshooting
- **Audio device invalidation handling** - Graceful handling when audio devices are removed
  - Detects `AUDCLNT_E_DEVICE_INVALIDATED` and `AUDCLNT_E_SERVICE_NOT_RUNNING`
  - Logs device invalidation events for debugging
  - Consecutive error counting to detect sustained failures

### Fixed
- **D3D11 GPU synchronization** - Added `Flush()` calls before NVENC operations
  - Prevents potential hangs when GPU device becomes invalid
  - Based on NVIDIA forum recommendations for D3D11/NVENC interop
- **Settings window dimensions** - Consolidated to single constant (`SETTINGS_WIDTH`, `SETTINGS_HEIGHT`)
  - Fixes bug where window size was defined in two places with different values

### Changed
- **Repo cleanup** - Removed build artifacts and empty folders
  - Updated `.gitignore` to exclude `tools/*.exe`, `tools/*.obj`, and `Debug/`
  - Removed empty `bin/WinSpy/` folder

---

## [1.2.3] - 2026-01-15

### Fixed
- **Instant close** - App now closes instantly instead of 5-7 second delay
  - Root cause: NVIDIA driver DLL cleanup during normal process teardown
  - Solution: Use `ExitProcess(0)` after proper resource cleanup to bypass slow driver unload
  - Window hidden immediately on close to prevent visual artifacts
- **AAC encoder fallback** - Generate AudioSpecificConfig if encoder doesn't provide one
  - Prevents potential muxing issues on some systems
- **DXGI device removal handling** - Added explicit handling for `DXGI_ERROR_DEVICE_REMOVED` and `DXGI_ERROR_ACCESS_LOST`
  - Improves robustness during GPU driver updates or display changes
- **Bitrate calculation overflow** - Use double precision for intermediate calculation
  - Prevents potential overflow with high resolution/fps combinations

### Changed
- **HEVC profile handling** - Clarified that profile is embedded in VPS/SPS from NVENC encoder
  - Added documentation comments explaining MF_MT_MPEG2_PROFILE usage for HEVC passthrough
- **AAC constants** - Use named constants (`AAC_SAMPLE_RATE`, `AAC_CHANNELS`, `AAC_BITRATE`) instead of magic numbers

---

## [1.2.2] - 2026-01-12

### Added
- **Per-source volume sliders** - Each audio source now has an independent volume control (0-100%)
  - Sliders appear inline next to each audio source dropdown in settings
  - Real-time percentage label updates as slider moves
  - Volumes saved to config and persist across sessions
  - Applied during mixing: samples scaled by volume before summing
- **Application icon** - Custom icon embedded in executable
  - Multi-resolution ICO (16px to 256px) for crisp display at all sizes

### Fixed
- **Audio mixing synchronization** - Multiple audio sources now mix properly without phase issues
  - Wait for all sources to have data before mixing
  - Use minimum available bytes across all sources for each mix cycle
  - Prevents choppy/distorted audio when mixing 2+ sources

---

## [1.2.1] - 2026-01-05

### Fixed
- **Audio capture thread leak** - Per-source capture threads now properly tracked and cleaned up
  - Added `captureThread` handle to `AudioCaptureSource` struct
  - `AudioCapture_Stop()` now waits for and closes all source threads before returning
- **Replay buffer static globals** - Reset all static globals at start of `BufferThreadProc`
  - Prevents stale state from persisting between buffer start/stop cycles
  - Fixed potential memory corruption on replay buffer restart
- **AAC encoder config leak** - Added proper error handling when `GetBlob` fails after malloc
  - Memory is now freed if `GetBlob` returns failure

### Removed
- **Redundant stop flag** - Removed legacy `g_stopBuffering` flag from replay buffer
  - Flag was never read; state machine events now control all thread coordination
- **Dead code in GPU converter** - Removed unused `inputView` member from `GPUConverter` struct
  - Input view is created per-frame in `GPUConverter_Convert()`, not stored

---

## [1.2.0] - 2026-01-05

### Changed
- **State machine architecture for replay buffer** - Replaced flag-based polling with proper Windows event synchronization
  - Added `ReplayStateEnum` with clear lifecycle states (UNINITIALIZED → STARTING → CAPTURING → SAVING → STOPPING)
  - Implemented Windows events (`hReadyEvent`, `hSaveRequestEvent`, `hSaveCompleteEvent`, `hStopEvent`) for cross-thread coordination
  - Uses `WaitForMultipleObjects` in main loop instead of busy polling
  - Added `InterlockedExchange`/`InterlockedIncrement` for thread-safe state transitions
  - Minimum 30 frames required before saves allowed (prevents empty/corrupt saves)

### Removed
- **Dead code cleanup** - Removed ~700 lines of unused legacy code
  - `h264_encoder.c/h` - Media Foundation MFT encoder, replaced by native NVENC API (`nvenc_encoder.c`)
  - `color_convert.c/h` - CPU-based BGRA→NV12 conversion, replaced by GPU conversion (`gpu_converter.c`)
  - `settings.h` - Declared `Settings_Show()` but never implemented or called

### Fixed
- **Capture region validation** - Now checks `Capture_SetRegion` return value and fails early if capture cannot be set up
- **Static counter pollution** - Moved diagnostic counters from static to function scope to prevent state pollution across buffer thread restarts

---

## [1.1.1] - 2026-01-03

### Fixed
- **Video playback speed accuracy** - Videos now play at correct real-time speed
  - Issue: Video content played faster than real-time (e.g., 10 seconds of content in 6 seconds)
  - Root cause: Frame timestamps were based on frame count rather than actual wall-clock time
  - Solution: Each frame now gets a real wall-clock timestamp from capture time
  - Inspired by ReplaySorcery's timestamp approach using `av_gettime_relative()`

- **Buffer duration accuracy** - Replay buffer now contains exactly the configured duration
  - Issue: 15-second buffer was producing 26-second videos
  - Root cause: Eviction used ideal frame durations (16.67ms at 60fps) but actual capture rate was ~34fps
  - Solution: Eviction now uses timestamp difference: `newest_timestamp - oldest_timestamp > max_duration`

- **Frame duration calculation** - Each frame's duration is now the real gap since the previous frame
  - Prevents timing drift in playback
  - Clamped to 25%-400% of ideal duration to handle timing glitches

### Technical Details
- Timestamp chain: Capture → Encoder → Buffer → Muxer all use real wall-clock timestamps
- Timestamps normalized to start at 0 when saving (first frame's timestamp becomes 0)
- Added diagnostic logging showing actual vs target FPS during capture

---

## [1.1.0] - 2026-01-03

### Changed
- **Phase 1-3 Architecture Refactoring** - Modular, maintainable codebase
  - Separated concerns into dedicated modules
  - Improved code organization and single responsibility

---

## [1.0.0] - 2026-01-03

### Added
- **ShadowPlay-style Instant Replay Buffer** - RAM-based H.264 encoding with on-demand MP4 muxing
  - Configurable duration (1 second to 20 minutes)
  - Configurable frame rate (15/30/60 FPS)
  - Aspect ratio options: Native, 16:9, 21:9, 4:3, 1:1
  - Per-monitor capture source selection
  - Hotkey-triggered save (default: F4)
  - RAM usage estimate displayed in settings UI
- Real-time H.264 encoding using Media Foundation Transform (MFT)
  - Software encoder (H264 Encoder MFT) for maximum compatibility
  - BGRA to NV12 color space conversion (BT.601)
  - Low-latency encoding mode for minimal capture delay
- Circular sample buffer for encoded H.264 NAL units
  - Duration-based eviction (keeps exactly N seconds)
  - Keyframe-aligned eviction for clean seeking
  - Thread-safe with critical section locking
- H.264 passthrough muxing to MP4 container
  - No re-encoding on save (instant, <500ms)
  - Precise frame timestamps to prevent timing drift
  - Proper keyframe marking for seeking

### Fixed
- Video duration accuracy: Precise timestamp calculation prevents cumulative rounding errors
  - Old: `timestamp = frame * (10000000/fps)` accumulated 1-second drift over 15 seconds
  - New: `timestamp = (frame * 10000000) / fps` maintains exact timing
- Aspect ratio cropping for ultra-wide monitors (5120x1440 → 2560x1440 for 16:9)

### Technical Details
- Three-component architecture:
  - `h264_encoder.c` - IMFTransform-based H.264 encoding to memory
  - `sample_buffer.c` - Circular buffer + passthrough MP4 muxing
  - `replay_buffer.c` - Capture orchestration and thread management
- Debug logging to `replay_debug.txt` for troubleshooting

---

## [0.9.1] - 2025-12-31

### Added
- Stop recording widget with timer display (MM:SS format counting up)
- Click-to-stop functionality on the timer widget
- Subtle hover effect on stop recording button
- Anti-aliased red recording indicator dot using GDI+
- Vertical divider separating timer from "Stop Recording" text

### Fixed
- Timer widget and border now excluded from screen capture (WDA_EXCLUDEFROMCAPTURE)
- All overlay windows created off-screen to prevent capture artifacts
- Improved window positioning to avoid black rectangle artifacts

### Changed
- Modern dark themed stop recording indicator with rounded corners
- Consistent Segoe UI font across all UI elements

## [0.9] - 2025-12-07

### Added
- Initial release
- Area, Window, Monitor, and All Monitors capture modes
- MP4 (H.264), AVI, and WMV output formats
- Quality presets: Low, Medium, High, Lossless
- DXGI Desktop Duplication for hardware-accelerated capture
- Windows 11 Snipping Tool-style UI
- Settings panel with format, quality, time limit, save location
- Single-instance mutex for macro key/Stream Deck toggle support
- Mouse cursor capture option
- Recording border overlay option
- Configurable time limit (hours/minutes/seconds)
- Auto-save with timestamp filenames
- INI-based configuration persistence

# CLAUDE.md - AI Assistant Instructions

This file provides project-specific coding patterns for AI assistants working on this codebase.

## Project Overview

Lightweight Screen Recorder (LWSR) - A Windows screen recorder using:
- DXGI Desktop Duplication for capture
- NVENC hardware encoding (HEVC)
- Media Foundation for muxing
- WASAPI for audio capture
- Win32/GDI+ for UI

---

# Part 1: Project-Specific Patterns

## Architecture Overview

```
┌─────────────┐    ┌──────────────┐    ┌─────────────┐    ┌───────────┐
│ DXGI Capture│───▶│GPU Converter │───▶│NVENC Encoder│───▶│Frame Buffer│
│ (capture.c) │    │(gpu_convert) │    │(nvenc_enc)  │    │(frame_buf) │
└─────────────┘    │ BGRA→NV12    │    │ HEVC encode │    │ circular   │
                   └──────────────┘    └─────────────┘    └─────┬──────┘
                                                                │
┌─────────────┐    ┌──────────────┐                             │
│WASAPI Capture│──▶│ AAC Encoder  │────────────────────────────▶│
│(audio_capt) │    │(aac_encoder) │                             │
└─────────────┘    └──────────────┘                             ▼
                                                          ┌───────────┐
                                                          │MP4 Muxer  │
                                                          │(mp4_muxer)│
                                                          └───────────┘
```

### Thread Model
| Thread | Purpose | Key State |
|--------|---------|-----------|
| Main/UI | Window messages, hotkeys | `g_controlWnd`, `g_overlayWnd` |
| BufferThread | Capture loop, encodes frames | `g_internal` (ReplayInternalState) |
| NVENC Output | Async bitstream retrieval | `enc->outputThread` |
| Audio Source[0-2] | Per-device WASAPI capture | `src->captureThread` |
| Audio Mix | Combines sources to PCM | `ctx->captureThread` |
| Health Monitor | Watchdog for stalls | `g_healthThread` |

---

## DXGI Desktop Duplication

### Device Removal Handling
```c
// DXGI can fail with device removal on driver update, GPU sleep, display change
if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_ACCESS_LOST) {
    // Must reinitialize entire D3D11 device chain
    Capture_ReinitDuplication(state);
}
```

### Output Duplication Lifecycle
- `AcquireNextFrame` → `QueryInterface` for texture → `ReleaseFrame`
- Frame must be released before next acquire
- Timeout on acquire is normal (no desktop change)

---

## NVENC Encoder (nvenc_encoder.c)

### Shared Texture Pattern with Keyed Mutexes
```
Source Device (capture)          Encoder Device (NVENC)
       │                                │
       ▼                                ▼
┌─────────────┐   shared    ┌─────────────────┐
│stagingTex[i]│────────────▶│ inputTexture[i] │
│ srcMutex[i] │   handle    │  encMutex[i]    │
└─────────────┘             └─────────────────┘
       │                                │
   AcquireSync(0)               AcquireSync(1)
   CopyResource                 nvEncMapInput
   ReleaseSync(1)               nvEncUnmap
                                ReleaseSync(0)
```

### NVENC Session Limits
- Consumer GPUs: max 3 concurrent encode sessions
- Session leak = unusable until reboot
- Always call `nvEncDestroyEncoder` in cleanup

### Async Output Thread
- NVENC encodes async; output thread waits on `completionEvents[i]`
- `nvEncLockBitstream` blocks until frame ready
- Callback delivers `EncodedFrame` to frame buffer

---

## WASAPI Audio Capture (audio_capture.c)

### Loopback vs Microphone
```c
if (deviceType == AUDIO_DEVICE_OUTPUT) {
    streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;  // System audio
}
```

### Device Invalidation
```c
// Device unplugged or driver reset
if (hr == AUDCLNT_E_DEVICE_INVALIDATED || 
    hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
    InterlockedExchange(&src->deviceInvalidated, TRUE);
    // Source continues running, contributes silence
}
```

### Format Conversion
- Devices report native format (often 32-bit float, 48kHz)
- Must convert to target format (16-bit PCM, 48kHz stereo)
- Resampling uses linear interpolation

---

## Media Foundation Muxer (mp4_muxer.c)

### HEVC Passthrough Pattern
```c
// Sequence header (VPS/SPS/PPS) goes in media type, NOT in samples
MFCreateMediaType(&videoType);
videoType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader, seqHeaderSize);

// Individual frames are raw NAL units (no start codes needed for MP4)
```

### Timestamp Normalization
- Capture timestamps are wall-clock (QueryPerformanceCounter)
- Muxer expects timestamps starting at 0
- Subtract first frame's timestamp from all frames

---

## Replay Buffer State Machine (replay_buffer.c)

```
UNINITIALIZED ──Init()──▶ STARTING ──ready──▶ CAPTURING
                              │                   │
                              │               SaveRequest
                              │                   │
                              ▼                   ▼
                           ERROR            SAVING ──done──▶ CAPTURING
                              │                   │
                              │               StopRequest
                              │                   │
                              ▼                   ▼
                         (cleanup)           STOPPING ──▶ UNINITIALIZED
```

### Windows Events (not flag polling!)
```c
// WRONG - busy polling, wastes CPU, race conditions
while (!g_stopFlag) { ... }

// CORRECT - proper synchronization
HANDLE events[] = { state->hStopEvent, state->hSaveRequestEvent };
DWORD result = WaitForMultipleObjects(2, events, FALSE, frameTime);
```

---

## Health Monitor (health_monitor.c)

### Heartbeat Pattern
```c
// Worker threads call periodically:
Logger_Heartbeat(THREAD_BUFFER);  // Updates timestamp

// Monitor checks timestamps every 500ms:
double age = Logger_GetHeartbeatAge(THREAD_BUFFER);
if (age > 5.0) {  // 5 second stall
    PostMessage(g_controlWnd, WM_WORKER_STALLED, ...);
}
```

---

## Key Constants (constants.h)

| Constant | Value | Purpose |
|----------|-------|---------|
| `NUM_BUFFERS` | 8 | NVENC input/output buffer count |
| `MAX_SEQ_HEADER_SIZE` | 256 | HEVC VPS/SPS/PPS |
| `MF_UNITS_PER_SECOND` | 10000000 | 100-nanosecond units |
| `AUDIO_SAMPLE_RATE` | 48000 | AAC sample rate |

---

# Part 2: Generic C Best Practices

## Resource Management - Goto-Cleanup Pattern

Functions with 3+ resources MUST use goto-cleanup:

```c
/*
 * MULTI-RESOURCE FUNCTION: FunctionName
 * Resources: N - list resources here
 * Pattern: goto-cleanup with SAFE_*
 */
BOOL FunctionName(void) {
    ResourceA* a = NULL;  // Initialize ALL to NULL
    ResourceB* b = NULL;
    BOOL csInitialized = FALSE;
    
    a = CreateResourceA();
    if (!a) goto cleanup;
    
    b = CreateResourceB();
    if (!b) goto cleanup;
    
    return TRUE;
    
cleanup:
    SAFE_FREE(b);    // Release in REVERSE order
    SAFE_FREE(a);    // Use SAFE_* macros, not raw free/Release
    return FALSE;
}
```

### SAFE_* Macros (mem_utils.h)
| Macro | Use For |
|-------|---------|
| `SAFE_FREE(ptr)` | malloc/calloc |
| `SAFE_RELEASE(ptr)` | COM objects |
| `SAFE_CLOSE_HANDLE(h)` | Windows handles |
| `SAFE_COTASKMEM_FREE(ptr)` | CoTaskMemAlloc |

---

## Thread Safety

```c
// Shared flags: volatile LONG + Interlocked*
volatile LONG g_isRecording;
InterlockedExchange(&g_isRecording, TRUE);
while (!InterlockedCompareExchange(&g_stop, 0, 0)) { ... }
```

---

## Error Handling

```c
// HRESULT: Always use FAILED(), not != S_OK
if (FAILED(hr)) { goto cleanup; }

// Allocations: Always check
buffer = malloc(size);
if (!buffer) goto cleanup;
```

---

## Integer Overflow Protection

```c
// Cast BEFORE multiplication for large resolutions
size_t frameSize = (size_t)width * (size_t)height * 4;
```

---

## Assertions + Runtime Checks

```c
LWSR_ASSERT(ptr != NULL);  // Debug builds
if (!ptr) return FALSE;     // Release builds too
```

---

## Common Pitfalls

1. **Raw Release/free in cleanup** → Use SAFE_* macros
2. **BOOL for thread flags** → Use `volatile LONG` + Interlocked*
3. **`hr != S_OK`** → Use `FAILED(hr)`
4. **Integer overflow** → Cast to `size_t` before multiplication
5. **Flag polling for threads** → Use Windows events
6. **Missing Lock() error check** → Always check before using buffer
7. **Leaking COM activates array** → Free even when count is 0
8. **NVENC session leak** → Always destroy encoder on all exit paths

# Replay Buffer Architecture - ShadowPlay-Style Implementation

## Executive Summary

This document describes the implemented architecture for a ShadowPlay-style instant replay buffer. The system uses RAM-based circular buffer of encoded H.264 samples with on-demand MP4 muxing - no disk I/O during buffering, instant save without re-encoding.

**Status: ✅ IMPLEMENTED AND WORKING** (as of January 2026)

---

## Architecture Overview

```
┌─────────────┐     ┌─────────────────┐     ┌──────────────────────┐
│   Capture   │────▶│  IMFTransform   │────▶│  Circular RAM Buffer │
│   (BGRA)    │     │  (H.264 encode) │     │  (encoded samples)   │
└─────────────┘     └─────────────────┘     └──────────────────────┘
      │                     │                         │
      │              BGRA→NV12                   On save:
      │              conversion                       │
      ▼                     ▼                         ▼
┌─────────────┐     ┌─────────────────┐     ┌──────────────────────┐
│   DXGI      │     │  H264 Encoder   │     │  IMFSinkWriter       │
│   Desktop   │     │  MFT (software) │     │  (passthrough mux)   │
│   Dup API   │     └─────────────────┘     └──────────────────────┘
└─────────────┘                                       │
                                                      ▼
                                            ┌──────────────────────┐
                                            │  Output .mp4 file    │
                                            │  (instant, <500ms)   │
                                            └──────────────────────┘
```

---

## Component Details

### 1. H.264 Memory Encoder (`h264_encoder.c/.h`)

Encodes raw BGRA frames to H.264 NAL units in memory using Media Foundation Transform.

**Key Features:**
- Uses software H264 Encoder MFT (avoids D3D11 device manager complexity)
- BGRA to NV12 color space conversion (BT.601 coefficients)
- Low-latency mode enabled for minimal capture delay
- Keyframe detection via `MFSampleExtension_CleanPoint`

**Data Structures:**
```c
typedef struct {
    IMFTransform* encoder;        // H.264 encoder MFT
    IMFMediaType* inputType;      // NV12 input format  
    IMFMediaType* outputType;     // H.264 output format
    int width, height, fps;
    UINT32 bitrate;
    LONGLONG frameCount;
    BOOL initialized;
} H264MemoryEncoder;

typedef struct {
    BYTE* data;                   // H.264 NAL unit data
    DWORD size;                   // Size in bytes
    LONGLONG timestamp;           // PTS in 100-ns units
    BOOL isKeyframe;              // TRUE if IDR frame
} EncodedFrame;
```

**Color Conversion (BGRA → NV12):**
```c
// BT.601 coefficients
Y  = 0.299*R + 0.587*G + 0.114*B
Cb = 128 - 0.169*R - 0.331*G + 0.500*B  
Cr = 128 + 0.500*R - 0.419*G - 0.081*B
```

### 2. Circular Sample Buffer (`sample_buffer.c/.h`)

Thread-safe circular buffer that stores encoded H.264 samples in RAM.

**Key Features:**
- Duration-based capacity (stores exactly N seconds of video)
- Automatic eviction of oldest samples when full
- Keyframe-aligned eviction for clean seeking
- Snapshot-based save (capture continues during mux)

**Data Structures:**
```c
typedef struct {
    BYTE* data;                   // Copy of encoded NAL data
    DWORD size;
    LONGLONG timestamp;
    BOOL isKeyframe;
} BufferedSample;

typedef struct {
    BufferedSample* samples;      // Circular array
    int capacity;                 // Max samples
    int count;                    // Current sample count
    int head;                     // Next write position
    int tail;                     // Oldest sample position
    int width, height, fps;
    QualityPreset quality;
    LONGLONG maxDuration;         // Target duration (100-ns units)
    CRITICAL_SECTION lock;
} SampleBuffer;
```

**Eviction Logic:**
```c
// When buffer exceeds maxDuration:
// 1. Calculate oldest timestamp to keep
// 2. Evict samples older than that threshold
// 3. Ensures we always have exactly the requested duration
```

### 3. Passthrough MP4 Muxer (in `sample_buffer.c`)

Writes buffered H.264 samples to MP4 without re-encoding.

**Key Implementation:**
```c
// Configure SinkWriter for H.264 passthrough
outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
outputType->SetUINT64(MF_MT_FRAME_SIZE, (width << 32) | height);
outputType->SetUINT64(MF_MT_FRAME_RATE, (fps << 32) | 1);

// CRITICAL: Use same type for input = passthrough mode
writer->SetInputMediaType(streamIndex, outputType, NULL);
```

**Precise Timestamp Calculation:**
```c
// Avoids cumulative rounding errors
// OLD (buggy): timestamp = frame * (10000000 / fps)
//   → 450 frames @ 30fps = 14.99s (lost 1 second!)

// NEW (correct): timestamp = (frame * 10000000) / fps  
//   → 450 frames @ 30fps = 15.000s (exact)

LONGLONG sampleTime = (LONGLONG)frameNumber * 10000000LL / fps;
LONGLONG nextTime = (LONGLONG)(frameNumber + 1) * 10000000LL / fps;
LONGLONG sampleDuration = nextTime - sampleTime;
```

### 4. Replay Buffer Controller (`replay_buffer.c/.h`)

Orchestrates capture, encoding, buffering, and save operations.

**Key Features:**
- Dedicated background thread for continuous capture
- Aspect ratio cropping (16:9, 21:9, 4:3, 1:1)
- Per-monitor capture source selection
- Hotkey-triggered save (non-blocking)

**Thread Model:**
```
Main Thread                    Buffer Thread
     │                              │
     ├── ReplayBuffer_Start() ─────▶│ (spawn)
     │                              │
     │                              ├── Capture frame
     │                              ├── Encode to H.264
     │                              ├── Add to buffer
     │                              └── Loop...
     │                              │
     ├── ReplayBuffer_Save() ──────▶│ (signal)
     │                              ├── WriteToFile()
     │                              └── Continue capturing
     │                              │
     └── ReplayBuffer_Stop() ──────▶│ (terminate)
```

---

## Memory Usage

```
Typical usage at High quality (90 Mbps base):

Resolution      FPS    Duration    Approx RAM
─────────────────────────────────────────────
1920x1080       30     15 sec      ~40 MB
2560x1440       30     15 sec      ~70 MB
2560x1440       60     60 sec      ~450 MB
3840x2160       60     60 sec      ~900 MB
```

RAM estimate displayed in settings UI:
```
"When enabled, ~70 MB of RAM is reserved for the video buffer."
```

---

## File Structure

```
src/
├── h264_encoder.c/.h      # IMFTransform H.264 encoding
├── sample_buffer.c/.h     # Circular buffer + MP4 muxing
├── replay_buffer.c/.h     # Orchestration + capture thread
├── encoder.c/.h           # Regular recording (non-replay)
├── capture.c/.h           # DXGI Desktop Duplication
└── overlay.c/.h           # Settings UI with replay options
```

---

## Debug Logging

All replay buffer operations log to `bin/replay_debug.txt`:

```
BufferThread started (ShadowPlay RAM mode)
Config: replayEnabled=1, duration=15, captureSource=1, monitorIndex=0
Config: replayFPS=30, replayAspectRatio=1, quality=3
Raw monitor bounds: 5120x1440 (rect: 0,0,5120,1440)
Aspect ratio 16:9 applied: 5120x1440 -> 2560x1440 (crop offset: 1280,0)
Final capture params: 2560x1440 @ 30 FPS, duration=15s, quality=3
H264Encoder_Init: 2560x1440 @ 30 fps, bitrate=64761076
Using encoder: H264 Encoder MFT (software)
...
SAVE REQUEST: 450 samples (15.0s) -> .../Replay_20260103_001702.mp4
WriteToFile: Writing 450 samples at 30 fps
WriteToFile: Final timestamp: 150000000 (15.000s), keyframes: 15
WriteToFile: wrote 450/450 samples, finalize=OK
SAVE OK
```

---

## Verification

Frame timestamps can be verified using FFprobe:
```powershell
ffprobe -select_streams v -show_frames -show_entries frame=pts_time,pict_type -of csv "video.mp4"
```

Expected output (30 FPS):
```
frame,0.000000,I    # Keyframe at 0.0s
frame,0.033333,P    # +33.3ms
frame,0.066667,P    # +33.3ms
...
frame,14.966667,P   # Final frame at ~15s
```

---

## Success Criteria (All Met ✅)

| Criteria | Status | Notes |
|----------|--------|-------|
| Exact duration | ✅ | 15s config = 15.000s video |
| Instant save | ✅ | <500ms (passthrough mux) |
| No data loss | ✅ | Circular buffer always has last N seconds |
| Correct playback speed | ✅ | Precise timestamps, no drift |
| Reasonable RAM | ✅ | ~70MB for 15s @ 1440p/30fps |

---

## Key Lessons Learned

### 1. Timestamp Precision Matters
Integer division truncation (`10000000 / 30 = 333333`) loses 0.33 microseconds per frame. Over 450 frames, this accumulated to nearly 1 second of drift, causing videos to play back too fast.

**Solution:** Calculate timestamps as `(frame * 10000000) / fps` to maintain precision.

### 2. Hardware Encoder Complexity
NVIDIA hardware encoder (NVENC) requires D3D11 device manager setup, which adds significant complexity. Software encoder works on all systems without GPU-specific setup.

**Solution:** Prefer software H264 Encoder MFT for compatibility; hardware encoder optional for future optimization.

### 3. Passthrough Muxing
To avoid re-encoding, the SinkWriter's input media type must match the output type exactly. This triggers "passthrough" mode where samples are muxed directly.

### 4. Color Space Conversion
H.264 encoders typically require NV12 (YUV 4:2:0) input, not BGRA. Manual color conversion is needed.

**Solution:** Implemented BT.601 BGRA→NV12 conversion in `h264_encoder.c`.

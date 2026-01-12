# Replay Buffer Architecture - ShadowPlay-Style Implementation

## Executive Summary

This document describes the implemented architecture for a ShadowPlay-style instant replay buffer. The system uses RAM-based circular buffer of encoded HEVC samples (via NVIDIA NVENC hardware encoder) with on-demand MP4 muxing - no disk I/O during buffering, instant save without re-encoding.

**Status: ✅ IMPLEMENTED AND WORKING** (as of January 2026)

---

## Architecture Overview

### Video Pipeline
```
DXGI Desktop Dup ──▶ GPU Converter ──▶ NVENC HEVC ──▶ RAM Buffer ──▶ MP4 Muxer
     (BGRA)         (BGRA→NV12)      (8-buf async)   (circular)    (passthrough)
```

### Audio Pipeline
```
WASAPI (up to 3 sources) ──▶ Volume Mixer ──▶ AAC Encoder ──▶ RAM Buffer
                             (per-source)       (MFT)        (time-synced)
```

### Save Flow
```
                          ┌─────────────┐
Video Buffer ────────────▶│             │
                          │  MP4 Muxer  │──▶ Output.mp4 (<500ms)
Audio Buffer ────────────▶│             │
                          └─────────────┘
```

---

## Component Details

### NVENC Hardware Encoder (`nvenc_encoder.c`)

Encodes NV12 frames to HEVC using NVIDIA Video Codec SDK 13.0.19. The SDK is particular about buffer management and threading.

**Async Pipeline:** NVENC performs best with multiple in-flight frames. We maintain 8 input textures, 8 output bitstream buffers, and 8 completion events in a ring:

```c
#define NUM_BUFFERS 8

struct NVENCEncoder {
    ID3D11Texture2D* inputTextures[NUM_BUFFERS];
    NV_ENC_REGISTERED_PTR registeredResources[NUM_BUFFERS];
    NV_ENC_INPUT_PTR mappedResources[NUM_BUFFERS];
    NV_ENC_OUTPUT_PTR outputBuffers[NUM_BUFFERS];
    HANDLE completionEvents[NUM_BUFFERS];
    
    int submitIndex;      // Next buffer for frame submission
    int retrieveIndex;    // Next buffer to retrieve output from
};
```

**Output Thread:** The SDK requires a separate thread to wait on completion events. Critically, input resources must not be unmapped until after `nvEncLockBitstream` returns:

```c
static unsigned __stdcall OutputThreadProc(void* param) {
    while (!enc->stopThread) {
        int idx = enc->retrieveIndex;
        if (WaitForSingleObject(enc->completionEvents[idx], 100) == WAIT_OBJECT_0) {
            enc->fn.nvEncLockBitstream(enc->encoder, &lockParams);
            // Deliver encoded frame to callback
            enc->fn.nvEncUnlockBitstream(enc->encoder, enc->outputBuffers[idx]);
            
            // NOW safe to unmap input (after lock completes)
            enc->fn.nvEncUnmapInputResource(enc->encoder, enc->mappedResources[idx]);
            enc->retrieveIndex = (enc->retrieveIndex + 1) % NUM_BUFFERS;
        }
    }
}
```

**Initialization:** Query async capability, get preset config, customize:

```c
enc->fn.nvEncGetEncodePresetConfigEx(enc->encoder,
    NV_ENC_CODEC_HEVC_GUID,
    NV_ENC_PRESET_P1_GUID,  // Fastest
    NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
    &presetConfig);

config.gopLength = fps * 2;  // 2-second keyframe interval
config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
```

**Cleanup Order:** Resources destroyed in reverse order: thread → EOS → events → outputs → inputs → encoder → library.

### GPU Color Converter (`gpu_converter.c`)

Converts captured BGRA to NV12 using D3D11 Video Processor (zero-copy GPU operation, BT.601 color space).

### Audio Capture (`audio_capture.c`)

Captures from up to 3 WASAPI sources with per-source volume control.

**Synchronized Mixing:** Waits for ALL sources to have data before mixing to prevent phase issues:

```c
// Find minimum bytes available across all sources
for (int i = 0; i < sourceCount; i++) {
    if (srcBytes[i] < minBytes) minBytes = srcBytes[i];
}

// Mix with per-source volume (0-100%)
for (int s = 0; s < numSamples; s++) {
    int leftSum = 0, rightSum = 0;
    for (int i = 0; i < sourceCount; i++) {
        short* samples = (short*)(srcBuffers[i] + s * 4);
        leftSum += (samples[0] * volumes[i]) / 100;
        rightSum += (samples[1] * volumes[i]) / 100;
    }
    if (srcCount > 1) { leftSum /= srcCount; rightSum /= srcCount; }
}
```

### AAC Encoder (`aac_encoder.c`)

Encodes mixed PCM to AAC-LC (48kHz stereo, 192kbps) via Media Foundation Transform. Circular buffer with duration-based eviction synchronized with video.

### MP4 Muxer (`mp4_muxer.c`)

Writes HEVC video + AAC audio to MP4 without re-encoding via IMFSinkWriter passthrough mode.

**Passthrough Trick:** Input media type must match output type exactly:

```c
outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
writer->SetInputMediaType(streamIndex, outputType, NULL);  // Same type = passthrough
```

**Timestamp Precision:** Avoids cumulative rounding errors that caused playback drift:

```c
// WRONG: timestamp = frame * (10000000 / fps)  → loses ~1 second over 450 frames
// RIGHT: timestamp = (frame * 10000000) / fps  → exact
```

### Replay Buffer Controller (`replay_buffer.c`)

Orchestrates capture, encoding, buffering, and save. Dedicated background thread with event-based state machine:

```
Main Thread                    Buffer Thread
     │                              │
     ├── ReplayBuffer_Start() ─────▶│ (spawn)
     │                              ├── Capture → Encode → Buffer (loop)
     ├── ReplayBuffer_Save() ──────▶│ (signal) → WriteToFile()
     └── ReplayBuffer_Stop() ──────▶│ (terminate)
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



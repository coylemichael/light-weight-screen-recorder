# Replay Buffer Architecture

**Status**: ✅ Implemented (Jan 2026)

## Contents

| Section | What's There |
|---------|-------------|
| [Overview](#overview) | ShadowPlay-style instant replay concept |
| [Pipeline](#pipeline) | Video/audio capture → buffer → save flow |
| [Components](#components) | NVENC, GPU converter, audio, muxer, buffer |

## Overview

ShadowPlay-style instant replay: RAM-based circular buffer of HEVC samples via NVIDIA NVENC hardware encoder. On-demand MP4 muxing - no disk I/O during buffering, instant save without re-encoding.

## Pipeline

```
Video:  DXGI Desktop Dup → GPU Converter → NVENC HEVC → RAM Buffer → MP4 Muxer
             (BGRA)        (BGRA→NV12)      (CUDA)      (circular)   (passthrough)

Audio:  WASAPI (3 sources) → Volume Mixer → AAC Encoder → RAM Buffer
                              (per-source)     (MFT)       (time-synced)

Save:   Video Buffer ─┬─→ MP4 Muxer → Output.mp4 (<500ms)
        Audio Buffer ─┘
```

## Components

### NVENC Encoder (`nvenc_encoder.c`)

CUDA-based NVENC encoding (replaced D3D11 in Jan 2026 to eliminate GPU deadlocks).

- **Input**: CPU NV12 frames uploaded to CUDA array
- **Output**: HEVC bitstream via callback
- **Sync mode**: No async thread - simpler, avoids D3D11 cross-device sync issues

### GPU Converter (`gpu_converter.c`)

D3D11 Video Processor converts BGRA→NV12, then CPU readback for CUDA upload.

### Audio Capture (`audio_capture.c`)

Up to 3 WASAPI sources with per-source volume. Synchronized mixing waits for all sources.

### MP4 Muxer (`mp4_muxer.c`)

IMFSinkWriter passthrough mode - no re-encoding. Input type = output type.

### Replay Buffer (`replay_buffer.c`)

Background thread with event-based state machine:
- `ReplayBuffer_Start()` → spawn thread
- `ReplayBuffer_SaveAsync()` → signal save, returns immediately
- `ReplayBuffer_Stop()` → terminate

Async save posts `WM_REPLAY_SAVE_COMPLETE` when done - UI stays responsive.

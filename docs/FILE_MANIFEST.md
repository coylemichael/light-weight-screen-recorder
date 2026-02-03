# File Manifest

One-liner purpose for each source file. If a file is doing more than this, question it.

## Core Pipeline

| File | Purpose |
|------|---------|
| `main.c` | Entry point, instance mutex, window creation, message loop |
| `capture.c` | DXGI Desktop Duplication - acquire frames from desktop |
| `encoder.c` | Media Foundation H.264/HEVC/WMV sink writer for traditional recording |
| `gpu_converter.c` | D3D11 Video Processor BGRA→NV12 conversion (zero-copy GPU path) |
| `nvenc_encoder.c` | CUDA-based NVENC HEVC encoding |
| `frame_buffer.c` | Circular buffer of encoded video samples |
| `mp4_muxer.c` | Media Foundation IMFSinkWriter - passthrough mux to MP4 |
| `replay_buffer.c` | Orchestrates capture→encode→buffer→save pipeline |

## Audio

| File | Purpose |
|------|---------|
| `audio_device.c` | Enumerate/select WASAPI audio devices |
| `audio_capture.c` | WASAPI capture + mixing from up to 3 sources |
| `aac_encoder.c` | Media Foundation AAC encoding |

## UI

| File | Purpose |
|------|---------|
| `overlay.c` | Recording indicator overlay window + hotkey handling |
| `action_toolbar.c` | Custom floating toolbar with smooth rounded corners for selection UI |
| `border.c` | Screen region selection border |

## Infrastructure

| File | Purpose |
|------|---------|
| `config.c` | INI file read/write for settings |
| `logger.c` | Async file logging + heartbeat tracking |
| `util.c` | String helpers, path utilities |
| `constants.h` | Shared constants (buffer counts, sample rates) |
| `app_context.h` | Shared global state declarations |

## Safety

| File | Purpose |
|------|---------|
| `mem_utils.c/h` | SAFE_* macros for cleanup |
| `leak_tracker.c` | Debug allocation tracking |
| `crash_handler.c` | Minidump on crash |

## External

| File | Purpose |
|------|---------|
| `gdiplus_api.c` | GDI+ initialization wrapper |
| `nvEncodeAPI.h` | NVIDIA Video Codec SDK header |
| `audio_guids.h` | WASAPI GUIDs |

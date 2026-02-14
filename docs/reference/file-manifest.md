# File Manifest

One-liner purpose for each source file. If a file is doing more than this, question it.

## Contents

| Section | What's There |
|---------|-------------|
| [Core Pipeline](#core-pipeline) | Main, capture, encoding, recording, replay |
| [Audio](#audio) | WASAPI capture, AAC encoding |
| [UI](#ui) | Overlay, settings, toolbar, border |
| [Infrastructure](#infrastructure) | Config, logging, utilities |
| [Safety](#safety) | Memory tracking, crash handling |
| [External](#external) | GDI+, NVENC SDK headers |

## Core Pipeline

| File | Purpose |
|------|---------|
| `main.c` | Entry point, instance mutex, window creation, message loop |
| `capture.c` | DXGI Desktop Duplication - acquire frames from desktop |
| `recording.c` | Direct-to-disk NVENC recording (thread, start/stop, state machine) |
| `gpu_converter.c` | D3D11 Video Processor BGRA→NV12 conversion (zero-copy GPU path) |
| `nvenc_encoder.c` | CUDA-based NVENC HEVC encoding |
| `frame_buffer.c` | Circular buffer of encoded video samples |
| `mp4_muxer.c` | Media Foundation IMFSinkWriter - passthrough mux to MP4 |
| `replay_buffer.c` | Orchestrates capture→encode→buffer→save pipeline (replay mode) |

## Audio

| File | Purpose |
|------|---------|
| `audio_device.c` | Enumerate/select WASAPI devices, owns shared IMMDeviceEnumerator |
| `audio_capture.c` | WASAPI capture + mixing from up to 3 sources |
| `aac_encoder.c` | Media Foundation AAC encoding |

## UI

| File | Purpose |
|------|---------|
| `overlay.c` | Recording indicator overlay window + hotkey handling |
| `settings_dialog.c` | Settings window UI creation and event handling |
| `action_toolbar.c` | Custom floating toolbar with smooth rounded corners for selection UI |
| `border.c` | Screen region selection border |
| `layered_window.c` | DIB + UpdateLayeredWindow helper for transparent overlays |
| `ui_draw.c` | Shared UI drawing utilities |
| `tray_icon.c` | System tray icon management |

## Infrastructure

| File | Purpose |
|------|---------|
| `config.c` | INI file read/write for settings |
| `logger.c` | Async file logging + heartbeat tracking |
| `util.c` | String helpers, path utilities |
| `constants.h` | Shared constants (buffer counts, sample rates) |
| `main.h` | Extern declarations for globals defined in main.c |

## Safety

| File | Purpose |
|------|---------|
| `mem_utils.h` | SAFE_* macros for cleanup, goto-cleanup pattern helpers |
| `leak_tracker.c` | Debug allocation tracking |
| `crash_handler.c` | Minidump on crash |

## External

| File | Purpose |
|------|---------|
| `gdiplus_api.c` | GDI+ initialization wrapper |
| `nvEncodeAPI.h` | NVIDIA Video Codec SDK header |
| `audio_guids.h` | WASAPI GUIDs |

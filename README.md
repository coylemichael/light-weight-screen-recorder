# Light Weight Screen Recorder

Lightweight screen recorder for Windows with instant replay. Requires an NVIDIA GPU.

- Capture area, window, or monitor to MP4/AVI/WMV
- Buffer as much time as your RAM allows, save instantly with F4
- Mix up to 3 audio sources with volume control

## Quick Start

1. Download the [latest release](https://github.com/coylemichael/light-weight-screen-recorder/releases/latest) and run `lwsr.exe`
2. Click the gear icon to configure replay duration, quality, and audio sources
3. Select a capture mode (Area, Window, Monitor, All Monitors)
4. For Area mode, draw a selection rectangle on screen
5. Click the red record button to start recording
6. Press F4 anytime to save the last N seconds as a replay

<p align="center">
  <img src="static/overlay.png" alt="LWSR Toolbar">
</p>

> [!IMPORTANT]
> The replay buffer stores encoded video in RAM. Higher durations and resolutions use more memory:
>
> | Duration | Resolution | Approx. RAM |
> |----------|------------|-------------|
> | 15 sec   | 1080p 30fps | ~50 MB     |
> | 1 min    | 1080p 30fps | ~200 MB    |
> | 5 min    | 1080p 60fps | ~1.5 GB    |
> | 20 min   | 1440p 60fps | ~8 GB      |
>
> If you're running low on memory, reduce the replay duration or resolution.

## Build

<details>
<summary>Build from source</summary>

Requires Visual Studio Build Tools (MSVC). The build script will prompt to install automatically if not found.

```batch
build.bat
```

Output: `bin\lwsr.exe`

</details>

## Verification

- ✅ **Attestation** - Releases are built on GitHub Actions with [build provenance](https://docs.github.com/en/actions/security-guides/using-artifact-attestations-to-establish-provenance-for-builds)
- ✅ **SHA256 hash** - Each release includes a hash for integrity verification
- ✅ **Open source** - Audit the code or build it yourself

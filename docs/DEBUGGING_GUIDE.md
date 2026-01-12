# LWSR Debugging Guide

## Overview
This documents the debugging workflow used to diagnose hangs/freezes in the screen recorder.

---

## Build Types vs Debug Logging

**Two separate things:**

1. **Build type** (`.\build.bat` vs `.\build.bat debug`)
   - `.\build.bat` - Release build (optimized, no symbols)
   - `.\build.bat debug` - Debug build (symbols for debugger, no optimization)

2. **Debug logging** (`--debug` CLI flag)
   - `.\lwsr.exe` - Normal operation, no logging
   - `.\lwsr.exe --debug` - Enables logging to `replay_debug.txt`

**For debugging issues, use BOTH:**
```powershell
.\build.bat debug              # Build with symbols
.\lwsr.exe --debug             # Run with logging enabled
```

---

## Step 1: Build Debug Version
```powershell
cd c:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder
.\build.bat debug
```

---

## Step 2: Start the App with Logging
Run the app with the `--debug` flag to enable logging:
```powershell
cd "C:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder\bin"
.\lwsr.exe --debug
```

This writes logs to `replay_debug.txt` in the bin folder.

---

## Step 3: When App Hangs - Capture Memory Dump

When the app freezes (not responding), capture a dump by getting the process ID first:

```powershell
Get-Process lwsr -ErrorAction SilentlyContinue | ForEach-Object { $id = $_.Id; cd "C:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder\bin"; .\procdump.exe -accepteula -ma $id . }
```

This creates a `.dmp` file in the bin folder.

### Alternative: Watch for Hangs Automatically
```powershell
cd "C:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder\bin"
.\procdump.exe -accepteula -ma -h -w lwsr.exe .
```
- `-ma` = full memory dump
- `-h` = trigger on hang (window not responding)
- `-w` = wait for process to start

---

## Step 4: Analyze the Dump with CDB

CDB is the console debugger that comes with WinDbg. Path:
```
C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2511.21001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe
```

### Get All Thread Stacks
```powershell
$cdb = "C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2511.21001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe"
$dump = "C:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder\bin\lwsr.exe_YYMMDD_HHMMSS.dmp"
& $cdb -z $dump -c "~*kn; q" 2>&1
```

### Filter for LWSR Symbols Only
```powershell
$cdb = "C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2511.21001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe"
$dump = "C:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder\bin\lwsr.exe_YYMMDD_HHMMSS.dmp"
& $cdb -z $dump -c "~*k; q" 2>&1 | Select-String -Pattern "lwsr!" -Context 2,0
```

---

## Step 5: Interpreting the Stack Traces

Look for threads stuck in:
- `NVENCODEAPI_Thunk` - NVENC driver calls
- `DXGIReleaseSync` / `DXGIAcquireSync` - DXGI synchronization
- `ReleaseFrame` / `AcquireNextFrame` - Desktop Duplication API
- `nvEncLockBitstream` / `nvEncUnlockBitstream` - NVENC bitstream operations

### Example of Thread Contention (the bug we found):
```
Thread 15 (BufferThread):
  lwsr!BufferThreadProc+0x...
  → ReleaseFrame
  → DXGIReleaseSync
  → NVENCODEAPI_Thunk  ← STUCK waiting for NVENC

Thread 16 (OutputThread):
  lwsr!OutputThreadProc+0x...
  → nvEncLockBitstream
  → NVENCODEAPI_Thunk  ← Also in NVENC driver
```

This shows both threads trying to use the same D3D11 device with NVENC simultaneously.

---

## Step 6: Check Debug Log

```powershell
Get-Content "C:\Users\BLACKHAND-CL1\OneDrive\__projects\lightweight-screen-recorder\bin\replay_debug.txt" -Tail 50
```

Or check for successful saves:
```powershell
(Select-String -Path "...\replay_debug.txt" -Pattern "SAVE OK").Count
```

---

## Step 7: Kill Stuck Process

If the process won't die:
```powershell
taskkill /f /im lwsr.exe
```

If that fails, run as admin:
```powershell
Start-Process powershell -Verb RunAs -ArgumentList "-Command taskkill /f /im lwsr.exe"
```

---

## Tools Required

1. **procdump.exe** - Download from Sysinternals:
   ```powershell
   Invoke-WebRequest -Uri "https://download.sysinternals.com/files/Procdump.zip" -OutFile "$env:TEMP\Procdump.zip"
   Expand-Archive -Path "$env:TEMP\Procdump.zip" -DestinationPath "$env:TEMP\Procdump" -Force
   Copy-Item "$env:TEMP\Procdump\procdump64.exe" -Destination ".\bin\procdump.exe"
   ```

2. **WinDbg Preview** - Install via winget:
   ```powershell
   winget install Microsoft.WinDbg --accept-source-agreements --accept-package-agreements
   ```

---

## Notes

- The debug build (`.\build.bat debug`) includes symbols for debuggers
- The `--debug` CLI flag enables logging to `replay_debug.txt`
- WinDbgX.exe (GUI) had issues launching the app directly - use procdump + cdb instead
- Dump files are large (~500MB-1GB) - delete after analysis

---

## Historical Issues & Fixes

### Thread Contention Between Desktop Duplication and NVENC (Fixed Jan 2026)

**Symptom:** App would freeze/hang after ~27+ saves over ~40 minutes. Save would timeout.

**Root Cause:** D3D11 device was shared between:
- BufferThread (Desktop Duplication: `AcquireNextFrame`/`ReleaseFrame`)
- OutputThread (NVENC: `nvEncLockBitstream`/`nvEncUnlockBitstream`)

D3D11 immediate context is not thread-safe. When both threads tried to use the device simultaneously, it caused a deadlock in the NVIDIA driver.

**Fix:** Created a separate D3D11 device for NVENC (`encDevice`) with shared textures using `IDXGIKeyedMutex` for synchronization between the capture device and encoder device.

**Reference:** NVIDIA Video Codec SDK Section 6.3 "Threading Model" explicitly warns about this issue.


# HealthMonitor Architecture Design

**Status**: ✅ IMPLEMENTED  
**Date**: 2026-01-29  
**Purpose**: Comprehensive audit of all threads and their relationship to HealthMonitor

---

## Table of Contents

1. [File Review Task List](#file-review-task-list)
2. [Thread Inventory Summary](#thread-inventory-summary)
3. [Current HealthMonitor Implementation (BEFORE Redesign)](#current-healthmonitor-implementation-before-redesign)
4. [Detailed File Analysis](#detailed-file-analysis)
5. [Questions for Design Discussion](#questions-for-design-discussion)
   - [Q1: Scope](#question-1-scope-of-healthmonitor--answered)
   - [Q2: Recovery Strategy](#question-2-recovery-strategy--answered)
   - [Q3: Detection Triggers](#question-3-stall-detection-triggers--answered)
   - [Q4: Repeated Failures](#question-4-repeated-failure-protection--answered)
   - [Q5: Timing Thresholds](#question-5-timing-thresholds--answered)
   - [Q6: Hot-Reload](#question-6-hot-reload-interaction--answered)
   - [Q7: Thread Slot](#question-7-thread_watchdog-slot-conflict--answered)
   - [Q8: NVENC Cleanup](#question-8-nvenc-session-cleanup-integration--answered)
   - [Q9: User Notification](#question-9-user-notification--answered)
   - [Q10: State Machine](#question-10-state-machine-clarity--answered)
   - [Q11: Recovery Execution](#question-11-recovery-execution---breaking-from-convention--answered)
   - [Q12: Cleanup Thread](#question-12-cleanup-thread-lifetime--answered)
   - [Q13: Debug Hooks](#question-13-testingdebugging-hooks--answered)
6. [All Design Questions Complete](#all-design-questions-complete) ⭐
7. [Final Architecture](#final-architecture) ⭐

---

## File Review Task List

| # | File | Has Threads? | Sends Heartbeat? | Needs Monitoring? | Status |
|---|------|-------------|------------------|-------------------|--------|
| 1 | health_monitor.c | ✅ Yes (1*) | ✅ THREAD_HEALTH_MONITOR | N/A (is the monitor) | ✅ Complete |
| 2 | logger.c | ✅ Yes (1) | ❌ No | ❌ No (infrastructure) | ✅ Complete |
| 3 | replay_buffer.c | ✅ Yes (1) | ✅ THREAD_BUFFER | ✅ Yes (MONITORED) | ✅ Complete |
| 4 | nvenc_encoder.c | ✅ Yes (1) | ✅ THREAD_NVENC_OUTPUT | ✅ Yes (MONITORED) | ✅ Complete |
| 5 | audio_capture.c | ✅ Yes (3) | ✅ THREAD_AUDIO_* | ✓ Out of scope (Q1) | ✅ Complete |
| 6 | audio_device.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 7 | aac_encoder.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 8 | main.c | ❌ No | ❌ No | N/A (UI thread) | ✅ Complete |
| 9 | overlay.c | ✅ Yes (1) | ❌ No | ✓ Out of scope (Q1) | ✅ Complete |
| 10 | capture.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 11 | encoder.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 12 | frame_buffer.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 13 | mp4_muxer.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 14 | gpu_converter.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 15 | config.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 16 | util.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 17 | border.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 18 | action_toolbar.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 19 | crash_handler.c | ✅ Yes (2) | ✅ THREAD_WATCHDOG | ❌ No (separate concern) | ✅ Complete |
| 20 | gdiplus_api.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 21 | leak_tracker.c | ❌ No | ❌ No | ❌ No | ✅ Complete |
| 22 | mem_utils.c | ❌ No | ❌ No | ❌ No | ✅ Complete |

> *CleanupThread removed per Q12

---

## Thread Inventory Summary

### Threads Found: 10 Total (after redesign)

| Thread | File | Heartbeat Slot | Monitored by HealthMonitor? |
|--------|------|----------------|------------------------------|
| **MonitorThread** | health_monitor.c | THREAD_HEALTH_MONITOR | N/A (IS the monitor) |
| ~~**CleanupThread**~~ | ~~health_monitor.c~~ | ~~None~~ | ❌ REMOVED (Q12) |
| **LoggerThreadProc** | logger.c | None | No (infrastructure) |
| **BufferThreadProc** | replay_buffer.c | THREAD_BUFFER | ✅ YES |
| **OutputThreadProc** | nvenc_encoder.c | THREAD_NVENC_OUTPUT | ✅ YES |
| **SourceCaptureThread** | audio_capture.c | THREAD_AUDIO_SRC1 | ✓ Out of scope (Q1) |
| **SourceCaptureThread** (2nd) | audio_capture.c | THREAD_AUDIO_SRC2 | ✓ Out of scope (Q1) |
| **MixCaptureThread** | audio_capture.c | THREAD_AUDIO_MIX | ✓ Out of scope (Q1) |
| **RecordingThread** | overlay.c | None | ✓ Out of scope (Q1) |
| **WatchdogThread** | crash_handler.c | THREAD_WATCHDOG | No (crash detection) |
| **DumpWriterThread** | crash_handler.c | None | No (crash handler) |

---

## Current HealthMonitor Implementation (BEFORE Redesign)

> ⚠️ **HISTORICAL:** This section documents the OLD implementation before the redesign. See [Final Architecture](#final-architecture) for what was actually implemented.

### What It Does Now

```
┌─────────────────────────────────────────────────────────┐
│               HEALTH MONITOR THREAD                      │
│  • Runs independently, checks every 500ms               │
│  • Soft warning at 2000ms stall                         │
│  • Hard recovery at 5000ms stall                        │
│  • Posts WM_WORKER_STALLED to UI                        │
│  • Spawns cleanup thread for orphaned resources         │
└─────────────────────────────────────────────────────────┘
              │                              │
              ▼                              ▼
    ┌─────────────────────┐    ┌─────────────────────────────┐
    │   CLEANUP THREAD    │    │   CURRENTLY MONITORED       │
    │  (fire-and-forget)  │    │                             │
    │  • Waits 60s for    │    │  ✅ THREAD_BUFFER           │
    │    hung thread      │    │  ✅ THREAD_NVENC_OUTPUT     │
    │  • Logs if leaked   │    │                             │
    └─────────────────────┘    └─────────────────────────────┘
```

### What It Does NOT Monitor (Intentionally - per Q1)

> **Decision:** HealthMonitor scope is replay buffer only. Audio and RecordingThread manage themselves.

```
┌─────────────────────────────────────────────────────────┐
│           OUT OF SCOPE (by design)                      │
├─────────────────────────────────────────────────────────┤
│  ✓  THREAD_AUDIO_MIX     - Has own recovery mechanism  │
│  ✓  THREAD_AUDIO_SRC1    - Has own recovery mechanism  │
│  ✓  THREAD_AUDIO_SRC2    - Has own recovery mechanism  │
│  ✓  RecordingThread      - Finite duration, user stops │
└─────────────────────────────────────────────────────────┘
```

---

## Detailed File Analysis

### 1. health_monitor.c ✅ Complete

**Threads Created:**
- `MonitorThread` - Main watchdog loop (500ms interval)
- ~~`CleanupThread`~~ - **REMOVING** (Q12: HealthMonitor does cleanup inline now)

**Heartbeats Sent:**
- `Logger_Heartbeat(THREAD_HEALTH_MONITOR)` every 500ms (**CHANGED** from THREAD_WATCHDOG per Q7)

**Key Constants:**
- `HEALTH_CHECK_INTERVAL_MS` = 500ms
- `HEALTH_SOFT_THRESHOLD_MS` = 2000ms
- `HEALTH_HARD_THRESHOLD_MS` = 5000ms
- ~~`CLEANUP_WAIT_TIMEOUT_MS` = 60000ms~~ **REMOVING** (Q12)
- `RESTART_GRACE_PERIOD_MS` = 10000ms

**Recovery Flow (NEW - per Q11):**
1. Detects heartbeat age > 5000ms
2. Checks `GetExitCodeThread()` - diagnose CRASHED vs HUNG (Q2)
3. Signals stop event, waits 5s
4. If CRASHED: attempts resource cleanup
5. Calls `ForceCleanupLeakedSessions()` (Q8)
6. Tracks recovery count (Q4: 3 in 5 min limit)
7. Posts `WM_WORKER_RESTART` or `WM_WORKER_FAILED` to UI

**Scope:** Replay buffer only (THREAD_BUFFER + THREAD_NVENC_OUTPUT) per Q1

---

### 2. logger.c ✅ Complete

**Threads Created:**
- `LoggerThreadProc` - Async log writer

**Purpose:** Infrastructure - writes logs to file from ring buffer

**Heartbeat System Provided:**
- `Logger_Heartbeat(ThreadId)` - register heartbeat
- `Logger_GetHeartbeatAge(ThreadId)` - check age in ms
- `Logger_ResetHeartbeat(ThreadId)` - clear stale data
- `Logger_IsThreadStalled(ThreadId)` - >10s check

**Thread Slots Defined:**
```c
typedef enum {
    THREAD_MAIN = 0,      // UI/main thread
    THREAD_BUFFER,        // Buffer capture thread
    THREAD_NVENC_OUTPUT,  // NVENC output thread
    THREAD_AUDIO_MIX,     // Audio mixer thread
    THREAD_AUDIO_SRC1,    // Audio source 1
    THREAD_AUDIO_SRC2,    // Audio source 2
    THREAD_WATCHDOG,      // HealthMonitor/Watchdog
    THREAD_MAX
} ThreadId;
```

**Note:** Logger thread itself doesn't need monitoring - it's infrastructure.

---

### 3. replay_buffer.c ✅ Complete

**Threads Created:**
- `BufferThreadProc` - Main capture/encode loop

**Heartbeats Sent:**
- `Logger_Heartbeat(THREAD_BUFFER)` every loop iteration

**Current Monitoring:** ✅ MONITORED by HealthMonitor

**Thread Lifecycle:**
1. Created in `ReplayBuffer_Start()`
2. Runs capture loop with heartbeats
3. Exits on stop event or fatal error
4. Closed in `ReplayBuffer_Stop()` (5s timeout)

**Recovery Behavior:**
- On device lost: exits cleanly (HealthMonitor detects via stale heartbeat)
- On transient failures: continues, HealthMonitor detects if stuck

---

### 4. nvenc_encoder.c ✅ Complete

**Threads Created:**
- `OutputThreadProc` - NVENC bitstream retrieval

**Heartbeats Sent:**
- `Logger_Heartbeat(THREAD_NVENC_OUTPUT)` every loop iteration

**Current Monitoring:** ✅ MONITORED by HealthMonitor

**Stall Risks:**
- `WaitForSingleObject(completionEvents[idx], EVENT_WAIT_TIMEOUT_MS)` - 100ms timeout
- `nvEncLockBitstream()` - can hang on device lost

**Recovery Behavior:**
- Checks `GetDeviceRemovedReason()` before NVENC calls
- Sets `deviceLost` flag and exits cleanly on device errors

---

### 5. audio_capture.c ✅ Complete

**Threads Created:**
- `SourceCaptureThread` (up to 2 instances) - Per-source audio capture
- `MixCaptureThread` - Audio mixing/encoding

**Heartbeats Sent:**
- `Logger_Heartbeat(THREAD_AUDIO_SRC1)` in SourceCaptureThread
- `Logger_Heartbeat(THREAD_AUDIO_MIX)` in MixCaptureThread

**Current Monitoring:** ⚠️ NOT MONITORED by HealthMonitor!

**Stall Risks:**
- WASAPI calls can block on device removal
- COM calls to audio service

**Self-Recovery:**
- Has own recovery mechanism for device invalidation
- Checks `AUDCLNT_E_DEVICE_INVALIDATED` and attempts reconnect

**Gap:** HealthMonitor ignores these threads entirely

---

### 6. audio_device.c ✅ Complete

**Threads Created:** None

**Purpose:** Audio device enumeration (COM-based)

**No monitoring needed** - synchronous utility functions

---

### 7. aac_encoder.c ✅ Complete

**Threads Created:** None

**Purpose:** AAC encoding via Media Foundation

**No monitoring needed** - called synchronously from audio threads

---

### 8. main.c ✅ Complete

**Threads Created:** None (creates UI message loop)

**Purpose:** Application entry point, global state

**Contains:**
- `g_replayBuffer` - global replay buffer state
- `g_capture` - DXGI capture state
- `g_config` - application configuration

**Heartbeat:** `THREAD_MAIN` slot exists but no heartbeats sent from main

---

### 9. overlay.c ✅ Complete

**Threads Created:**
- `RecordingThread` - Frame capture for direct recording

**Heartbeats Sent:** ❌ NONE

**Current Monitoring:** ⚠️ NOT MONITORED

**Thread Lifecycle:**
- Created in `Recording_Start()`
- Runs frame capture loop
- Stopped via `g_recording.stopRecording` flag

**Stall Risks:**
- `Capture_GetFrame()` - DXGI calls
- `Encoder_WriteFrame()` - Media Foundation calls

**Gap:** No heartbeat at all! No ThreadId slot for recording thread.

---

### 10-18. No-Thread Files ✅ Complete

Files with NO threads created:
- `capture.c` - DXGI capture (synchronous calls)
- `encoder.c` - Media Foundation encoder (synchronous)
- `frame_buffer.c` - Ring buffer management
- `mp4_muxer.c` - MP4 file writing
- `gpu_converter.c` - D3D11 compute shader conversion
- `config.c` - INI file handling
- `util.c` - Utility functions
- `border.c` - Recording border overlay
- `action_toolbar.c` - UI toolbar

---

### 19. crash_handler.c ✅ Complete

**Threads Created:**
- `WatchdogThread` - Main thread hang detection
- `DumpWriterThread` - Crash dump writer (on-demand)

**Heartbeats Sent:**
- `Logger_Heartbeat(THREAD_WATCHDOG)` in WatchdogThread

**Purpose:** Separate concern from HealthMonitor
- WatchdogThread monitors `g_heartbeatCounter` from main thread
- Triggers crash dump if main thread stops responding

**Note:** Uses same `THREAD_WATCHDOG` slot as HealthMonitor - **RESOLVED in Q7:** Adding `THREAD_HEALTH_MONITOR` slot.

---

### 20-22. Infrastructure Files ✅ Complete

Files with NO threads:
- `gdiplus_api.c` - GDI+ initialization
- `leak_tracker.c` - Memory leak tracking
- `mem_utils.c` - Memory allocation wrappers

---

## Questions for Design Discussion

### Question 1: Scope of HealthMonitor ✅ ANSWERED

**Decision: Option A - Replay Buffer Only**

Monitor only the replay buffer subsystem (THREAD_BUFFER + THREAD_NVENC_OUTPUT). Audio and RecordingThread manage themselves.

**Rationale:**
- Audio has its own recovery mechanism (device invalidation)
- RecordingThread is finite duration (user-initiated stop)
- Simpler scope = fewer edge cases
- Clear separation of concerns

---

### Question 2: Recovery Strategy ✅ ANSWERED

**Decision: Option B - Differentiated Recovery**

Check thread state with `GetExitCodeThread()`:
- **CRASHED** → Attempt resource cleanup (NVENC destroy, buffer free), then restart
- **HUNG** → Signal stop, wait 5 seconds, abandon resources (accept leak), restart

**Rationale:**
- Crashed threads are safe to clean up (no race conditions)
- Reclaiming ~100MB+ reduces cumulative memory leaks
- Hung threads are rarer; leaking them is acceptable trade-off for safety

---

### Question 3: Stall Detection Triggers ✅ ANSWERED

**Decision: Option A - Either Stalled**

If THREAD_BUFFER OR THREAD_NVENC_OUTPUT exceeds threshold → trigger recovery.

**Rationale:**
- NVENC can hang independently (LockBitstream stuck on GPU)
- Buffer could be fine but NVENC thread crashed
- Either failure means replay buffer is broken
- Better to recover early than wait for cascading failure

---

### Question 4: Repeated Failure Protection ✅ ANSWERED

**Decision: Option D - Time Window Limit**

Allow max 3 recoveries within 5 minutes. If exceeded, disable replay buffer and notify user.

**Rationale:**
- Single recovery is fine and expected
- 3 recoveries in 5 minutes = something fundamentally broken
- Prevents runaway restart loop
- User notification lets them check GPU/drivers
- Manual re-enable available later

---

### Question 5: Timing Thresholds ✅ ANSWERED

**Decision: Option A - Keep Current**

- Check interval: 500ms
- Soft warning: 2000ms
- Hard recovery: 5000ms
- Grace period: 10000ms

**Rationale:**
- 5 seconds is perceptible but not frustrating
- 10s grace covers NVENC initialization
- False positives are rare
- Can tune later with real-world data

---

### Question 6: Hot-Reload Interaction ✅ ANSWERED

**Decision: Option C - Event-Driven Grace**

`HealthMonitor_NotifyRestart()` called only AFTER successful Start. Grace period protects the new thread's initialization, not the Stop phase.

**Rationale:**
- If Stop hangs, we want to detect it (not hide in grace period)
- Grace protects NEW thread initialization
- More precise protection

---

### Question 7: THREAD_WATCHDOG Slot Conflict ✅ ANSWERED

**Decision: Option B - Add New Slot**

Add `THREAD_HEALTH_MONITOR` to the ThreadId enum. Separate tracking for each watchdog.

**Rationale:**
- Cleaner diagnostics in logs
- Can detect if specifically HealthMonitor died vs crash_handler
- Minimal code change (one enum value)

**Implementation:**
- Add `THREAD_HEALTH_MONITOR` before `THREAD_MAX` in logger.h
- Update health_monitor.c to use new slot
- crash_handler.c keeps `THREAD_WATCHDOG`

---

### Question 8: NVENC Session Cleanup Integration ✅ ANSWERED

**Decision: Option A - Always Call**

After any recovery (crashed or hung), call `ForceCleanupLeakedSessions()` before starting new encoder.

**Rationale:**
- Leaked sessions are a real problem (8 slot limit)
- After 5s stall, thread is already abandoned
- ForceCleanup is safer now (no D3D11 Flush)
- Clean slate prevents "out of sessions" failures

---

### Question 9: User Notification ✅ ANSWERED

**Decision: Option A - Silent (with sound on permanent failure)**

- Single recoveries: silent, log only
- Permanent failure (3 in 5 min): modal dialog + error sound

**Rationale:**
- Single recovery is seamless - user shouldn't care
- Notifications for working features are annoying
- Permanent failure NEEDS user attention → sound + dialog
- Sound ensures user notices even if app is in background

---

### Question 10: State Machine Clarity ✅ ANSWERED

**Decision: Option C - Rename to RECOVERING**

Change `REPLAY_STATE_STALLED` to `REPLAY_STATE_RECOVERING`.

**Rationale:**
- "STALLED" describes the problem, not the action
- "RECOVERING" describes what's happening
- Clearer transitions: CAPTURING → RECOVERING → STARTING
- Distinguishes from ERROR (permanent) vs RECOVERING (transient)

---


### Question 11: Recovery Execution - Breaking from Convention ✅ ANSWERED

**Decision: Split Execution**

HealthMonitor does diagnosis + cleanup directly. Posts to UI only for operations that truly require main thread.

#### The Analysis: Main Thread Coordination - Sensible or Cargo Cult?

**Why Windows apps traditionally use main thread:**

| Era | Reason | Still Valid? |
|-----|--------|--------------|
| Win16 (3.1) | No preemptive multithreading | ❌ Obsolete |
| Early Win32 | GDI objects weren't thread-safe | ❌ Obsolete |
| COM Apartments | STA model requires main thread for UI components | ⚠️ Partially valid |
| Convention | "That's how it's done" | ❌ Cargo cult |

**What actually requires main thread today:**
- Window handles (HWND) have thread affinity
- Some COM objects require STA (Single-Threaded Apartment)
- Modal dialogs must run on window's owning thread

**What does NOT require main thread:**
- Kernel calls (GetExitCodeThread, SetEvent, WaitForSingleObject)
- NVENC/D3D11 cleanup (thread-safe APIs)
- Most modern synchronization

#### Our Recovery Tasks Analyzed

| Task | Requires Main Thread? | Actual Reason |
|------|----------------------|---------------|
| `GetExitCodeThread()` | ❌ No | Kernel call, any thread |
| `SetEvent(hStopEvent)` | ❌ No | Kernel call, any thread |
| `WaitForSingleObject()` | ❌ No | Kernel call, any thread |
| `ForceCleanupLeakedSessions()` | ❌ No | NVENC/D3D11 are thread-safe |
| `ReplayBuffer_Start()` | ⚠️ Yes | Audio init uses COM (STA) |
| Modal dialog | ✅ Yes | Must be window's owning thread |
| Error sound | ❌ No | PlaySound works from any thread |

**Key insight:** The main thread isn't special. It's just another thread that happens to own windows. Use it only when you must.

#### The Split Architecture

```
HealthMonitor Thread (does the work):
  1. Detect stall via heartbeat age
  2. GetExitCodeThread() → diagnose CRASHED vs HUNG
  3. SetEvent(hStopEvent) → signal stop
  4. WaitForSingleObject(5s) → wait for graceful exit
  5. If CRASHED: attempt resource cleanup
  6. ForceCleanupLeakedSessions() → reclaim NVENC slots
  7. Track recovery count (3 in 5 min detection)
  8. Post WM_WORKER_RESTART to UI (or WM_WORKER_FAILED if limit hit)

UI Thread (minimal, only what requires it):
  9. ReplayBuffer_Start() → needs COM for audio
  10. HealthMonitor_NotifyRestart() → reset grace period
  11. If WM_WORKER_FAILED: show dialog + play error sound
```

#### Why This Matters (The LinkedIn Version)

Traditional Windows development wisdom says: "Coordinate everything through the main thread." But this advice is largely cargo cult - inherited from Win16/early Win32 limitations that no longer apply.

**We analyzed each recovery task and asked: "Does this ACTUALLY require main thread?"**

The answer for most operations was NO:
- Kernel synchronization primitives work from any thread
- Modern GPU APIs (D3D11, NVENC) are explicitly thread-safe
- Only COM-dependent initialization and UI dialogs truly need the main thread

**Message-passing to main thread adds:**
- Latency (message queue delay during critical recovery)
- Complexity (message constants, parameter packing)
- Indirection (logic split across files)
- False sense of "safety" (it's not actually safer)

**Direct execution provides:**
- Faster recovery (no queue delay)
- Cohesive logic (all recovery code in one place)
- Easier debugging (single code path)
- Principled design (based on actual requirements, not tradition)

**The principle:** Don't ask "how is this usually done?" Ask "what does this actually require?"

---

### Question 12: Cleanup Thread Lifetime ✅ ANSWERED

**Decision: Option A - Remove It**

HealthMonitor does cleanup directly. No separate cleanup thread needed.

**Rationale:**
- HealthMonitor now waits 5s inline
- If still hung after 5s, abandon and restart immediately
- No value in waiting 60s more - user experience matters
- Cleanup thread was workaround for not blocking UI; now we're not on UI thread
- Simpler architecture

---

### Question 13: Testing/Debugging Hooks ✅ ANSWERED

**Decision: Conditional Debug Hotkey**

Ctrl+Shift+D triggers test recovery, but only when debug logging checkbox is enabled.

**Rationale:**
- Power users who enable debug logging get access to testing
- Hidden from normal users (checkbox off by default)
- Works in release builds - useful for field troubleshooting
- Ties into existing UI option - no new config needed
- Can test full recovery flow without waiting for real stalls

---

## All Design Questions Complete

### Final Summary of Decisions

| # | Question | Decision |
|---|----------|----------|
| 1 | Scope | Replay buffer only (BUFFER + NVENC) |
| 2 | Recovery Strategy | Differentiated (crashed=cleanup, hung=abandon) |
| 3 | Detection Trigger | Either thread stalled → recover |
| 4 | Repeated Failures | 3 in 5 minutes → disable + notify |
| 5 | Timing Thresholds | Keep current (500ms/2s/5s/10s) |
| 6 | Hot-Reload | Grace period after Start only |
| 7 | Thread Slot | Add THREAD_HEALTH_MONITOR |
| 8 | NVENC Cleanup | Always call ForceCleanup before restart |
| 9 | User Notification | Silent + error sound on permanent failure |
| 10 | State Machine | Rename STALLED → RECOVERING |
| 11 | Recovery Execution | Split (HealthMonitor does work, UI only for COM/dialogs) |
| 12 | Cleanup Thread | Remove it (HealthMonitor does cleanup inline) |
| 13 | Debug Hooks | Ctrl+Shift+D when debug logging enabled |

---

## Final Architecture

### System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              APPLICATION                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────┐    heartbeats     ┌─────────────────────────────────┐  │
│  │  REPLAY BUFFER  │ ─────────────────▶│         LOGGER                  │  │
│  │                 │                   │   (heartbeat storage)           │  │
│  │  ┌───────────┐  │                   └─────────────────────────────────┘  │
│  │  │ BUFFER    │──┼─── THREAD_BUFFER           ▲                           │
│  │  │ THREAD    │  │                            │                           │
│  │  └───────────┘  │                            │ checks age                │
│  │                 │                            │                           │
│  │  ┌───────────┐  │                   ┌────────┴────────────────────────┐  │
│  │  │ NVENC     │──┼─── THREAD_NVENC   │      HEALTH MONITOR             │  │
│  │  │ OUTPUT    │  │                   │   ┌─────────────────────────┐   │  │
│  │  └───────────┘  │                   │   │ MonitorThread           │   │  │
│  │                 │                   │   │ • Check heartbeats      │   │  │
│  └────────▲────────┘                   │   │ • Diagnose crash/hung   │   │  │
│           │                            │   │ • Execute cleanup       │   │  │
│           │ Stop/Start                 │   │ • Track failure count   │   │  │
│           │                            │   └───────────┬─────────────┘   │  │
│           │                            └───────────────┼─────────────────┘  │
│           │                                            │                    │
│           │                            WM_WORKER_RESTART (success)          │
│           │                            WM_WORKER_FAILED (3 in 5 min)        │
│           │                                            │                    │
│  ┌────────┴──────────────────────────────────────────┬─▼────────────────┐   │
│  │                    UI THREAD (WndProc)            │                  │   │
│  │  • ReplayBuffer_Start() ─────────────────────────▶│ (COM required)   │  │
│  │  • HealthMonitor_NotifyRestart()                  │                  │   │
│  │  • Show error dialog + sound (on failure)         │                  │   │
│  └───────────────────────────────────────────────────┴──────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Recovery Flow (Step by Step)

**Phase 1: Detection** (HealthMonitor Thread)
```
1. MonitorThread checks heartbeat ages every 500ms
2. If THREAD_BUFFER or THREAD_NVENC_OUTPUT age > 5000ms:
   → Stall detected
```

**Phase 2: Diagnosis** (HealthMonitor Thread)
```
3. Call GetExitCodeThread(bufferThread)
4. If exitCode != STILL_ACTIVE:
   → Thread CRASHED (safe to clean up)
   Else:
   → Thread HUNG (abandon resources)
```

**Phase 3: Recovery** (HealthMonitor Thread)
```
5. Set state to REPLAY_STATE_RECOVERING
6. SetEvent(hStopEvent) - signal thread to stop
7. WaitForSingleObject(bufferThread, 5000ms)
8. If CRASHED: attempt resource cleanup
9. ForceCleanupLeakedSessions() - reclaim NVENC slots
10. Increment recovery counter, check 3-in-5-min limit
11. If limit exceeded:
    → Post WM_WORKER_FAILED to UI
    Else:
    → Post WM_WORKER_RESTART to UI
```

**Phase 4: Restart** (UI Thread - COM required)
```
12. WM_WORKER_RESTART handler:
    → ReplayBuffer_Start(&g_replayBuffer, &g_config)
    → HealthMonitor_NotifyRestart() - 10s grace period

13. WM_WORKER_FAILED handler:
    → Disable replay buffer
    → PlaySound(error)
    → Show modal dialog: "Replay buffer disabled due to repeated failures"
```

### State Transitions

```
Normal Operation:
  UNINITIALIZED → STARTING → CAPTURING → STOPPING → UNINITIALIZED
                                ↑            │
                                └────────────┘ (hot-reload)

Recovery (success):
  CAPTURING → RECOVERING → STARTING → CAPTURING
      │                        ↑
      └── stall detected ──────┘

Recovery (permanent failure):
  CAPTURING → RECOVERING → ERROR
      │                      │
      └── 3 in 5 min ────────┘
```

### Data Structures

**New/Modified in health_monitor.c:**
```c
/* Recovery tracking */
static DWORD g_recoveryTimestamps[3] = {0};  /* Ring buffer of last 3 recovery times */
static int g_recoveryIndex = 0;

/* Thread handle access (via extern g_replayBuffer) */
extern ReplayBufferState g_replayBuffer;
```

**New in logger.h:**
```c
typedef enum {
    THREAD_MAIN = 0,
    THREAD_BUFFER,
    THREAD_NVENC_OUTPUT,
    THREAD_AUDIO_MIX,
    THREAD_AUDIO_SRC1,
    THREAD_AUDIO_SRC2,
    THREAD_WATCHDOG,
    THREAD_HEALTH_MONITOR,  /* NEW */
    THREAD_MAX
} ThreadId;
```

**Modified in replay_buffer.h:**
```c
typedef enum {
    REPLAY_STATE_UNINITIALIZED = 0,
    REPLAY_STATE_STARTING,
    REPLAY_STATE_CAPTURING,
    REPLAY_STATE_SAVING,
    REPLAY_STATE_STOPPING,
    REPLAY_STATE_ERROR,
    REPLAY_STATE_RECOVERING   /* Renamed from STALLED */
} ReplayState;
```

### New Window Messages

```c
#define WM_WORKER_RESTART  (WM_USER + 301)  /* HealthMonitor requests restart */
#define WM_WORKER_FAILED   (WM_USER + 302)  /* Permanent failure, disable buffer */
```

### Files to Modify

| File | Changes |
|------|---------|
| health_monitor.h | Add new message defines, remove ScheduleCleanup |
| health_monitor.c | Implement split recovery, remove cleanup thread, add failure tracking |
| logger.h | Add THREAD_HEALTH_MONITOR enum |
| replay_buffer.h | Rename STALLED → RECOVERING |
| overlay.c | Handle WM_WORKER_RESTART, WM_WORKER_FAILED, add debug hotkey |

---

## Document Complete

**Status:** ✅ Implemented  
**Date:** 2026-01-29  
**Implementation:** Completed same day as design


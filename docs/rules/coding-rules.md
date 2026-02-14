# Coding Best Practice

Project-specific coding patterns and audit checklist. For architecture details, see [replay-buffer-architecture.md](../reference/replay-buffer-architecture.md).

## Contents

| Section | What's There |
|---------|-------------|
| [File Header Requirement](#file-header-requirement) | Docstring matching manifest |
| [Design Principles](#design-principles) | YAGNI, verify assumptions |
| [Audit Checklist](#audit-checklist) | Questions for code review |
| [C Best Practices](#c-best-practices) | Goto-cleanup, SAFE_* macros |
| [Thread Safety](#thread-safety) | Interlocked, Windows events |
| [COM Initialization](#com-initialization) | CoInitializeEx pattern |
| [Error Handling](#error-handling) | HRESULT, NULL checks |
| [Common Pitfalls](#common-pitfalls) | Known bugs to avoid |
| [Lessons Learned](#lessons-learned-r1r2-code-review) | Real bugs from R1/R2 review |

## File Header Requirement

Every `.c` file MUST have a docstring matching its [file-manifest.md](../reference/file-manifest.md) entry:

```c
/*
 * capture.c - DXGI Desktop Duplication - acquire frames from desktop
 */
```

If the docstring doesn't match the manifest, either:
1. The file has drifted from its purpose → refactor
2. The manifest is outdated → update it

## Design Principles

### Scope
- **YAGNI ruthlessly** - Remove unnecessary features, don't add "just in case" code

### Process  
- **Ask before adding** - One question at a time, prefer multiple choice
- **Explore alternatives** - Propose 2-3 approaches with trade-offs before implementing
- **Incremental validation** - Present designs in sections, validate each before continuing

### Implementation
- **Defend at point of use** - Each function validates its own preconditions. Don't rely on callers, other threads, or upstream code to have "already handled it."
- **Verify, don't assume** - Before relying on API behavior, grep for existing usage. Ask: *"If this assumption is wrong, what breaks?"*

---

# Audit Checklist

Questions to ask when reviewing any file. See [file-manifest.md](../reference/file-manifest.md) for what each file should do.

## YAGNI Questions

- Is every `#include` actually used?
- Is every function called from somewhere?
- Is every feature user-requested or "just in case"?
- Does the complexity match the actual need?
- Could this be simpler?

## DRY Detection

Watch for wrappers that don't add value:
- Does this module just forward calls to another? (HealthMonitor wrapped logger)
- Are there two places doing the same validation?
- Is there copied code that should be a function?

## Dead Code Signals

- Recovery logic for bugs fixed at root cause
- Commented-out code blocks
- Functions with no callers (grep for usage)
- `#ifdef` features nobody enables
- TODO comments from months ago

---

# C Best Practices

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

### Windows Events (not flag polling)
```c
// WRONG - busy polling, wastes CPU, race conditions
while (!g_stopFlag) { Sleep(10); }

// CORRECT - proper synchronization
HANDLE events[] = { state->hStopEvent, state->hSaveRequestEvent };
DWORD result = WaitForMultipleObjects(2, events, FALSE, frameTime);
```

---

## COM Initialization

```c
// Each thread using COM must initialize it
HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return FALSE;  // RPC_E_CHANGED_MODE = already initialized, that's OK
}
// ... use COM ...
CoUninitialize();  // Must match CoInitializeEx
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
---

# Lessons Learned (R1/R2 Code Review)

Real bugs discovered during systematic review of all 46 source files.

## Bugs That Shipped

| Bug | Version | Fix |
|-----|---------|-----|
| `Overlay_Destroy()` never called | v1.3.3 | Wire all `*_Destroy()` into main.c cleanup |
| Wall-clock timestamps → VFR stutter | v1.3.2 | Use synthetic: `frameNum × interval` for CFR |
| D3D11 device A + NVENC device B = deadlock | v1.3.0 | Use CUDA path for NVENC, not mixed D3D11 |
| `Lock()` HRESULT ignored | v1.2.18 | Check hr, track `locked` flag for cleanup |
| `GetProcAddress()` unchecked | v1.2.15 | Null-check every dynamic load |
| Field named `DeleteBrush` | v1.2.11 | Avoid Windows macro names (wingdi.h) |
| `CreateMutex` race | v1.2.15 | Check `ERROR_ALREADY_EXISTS` |
| Division by zero | v1.2.15 | Guard all divisions, even "impossible" ones |
| GDI+ not shutdown on early fail | v1.2.15 | Each init failure must cleanup prior inits |
| 500-line functions | v1.2.12 | Max 100 lines; extract helpers |

## Code Patterns (IMFMediaBuffer, Mutex, CFR)

```c
// IMFMediaBuffer Lock/Unlock - track locked state
BYTE* pData = NULL;
BOOL locked = FALSE;
hr = buffer->Lock(&pData, NULL, NULL);
if (FAILED(hr) || !pData) goto cleanup;
locked = TRUE;
// ...
cleanup:
    if (locked) buffer->Unlock();

// Single-instance mutex - check for existing
HANDLE mutex = CreateMutexA(NULL, TRUE, "MyAppMutex");
if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }

// CFR timestamps - synthetic, not wall-clock (v1.3.2 bug)
// Wall-clock captures jitter (4-20ms gaps) → VFR → player stutter
timestamp = frameNumber * frameInterval;  // NOT QueryPerformanceCounter
```

## Review Statistics (46 files audited)

| Finding | Count | Example |
|---------|-------|---------|
| Unused `<stdio.h>` | 10+ | Use `Logger_Log()` instead |
| Unused functions | 20+ | Grep for callers before shipping |
| Unused constants | 15+ | constants.h had 13 dead |
| Docstring drift | 6+ | Compare to FILE_MANIFEST.md |
| Internal fn in .h | 4+ | Make `static`, remove from header |
| Identical `#ifdef`/`#else` | 2 | Copy-paste error |
| Duplicate `#define` | 2 | AAC constants defined twice |
| Dead `TerminateThread` code | 50 lines | Doesn't release GPU/COM resources |

## YAGNI Scaffolding Signals

Delete if you see: structs with no instances, enums with unused values, "not implemented" stubs, `#if 0` blocks, `#ifdef FUTURE_FEATURE`, identical `#ifdef`/`#else` branches.

Example: main.h had 4 unused "future architecture" structs (175→22 lines after cleanup).
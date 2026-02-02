# Coding Best Practice

Project-specific coding patterns and audit checklist. For architecture details, see [REPLAY_BUFFER_ARCHITECTURE.md](REPLAY_BUFFER_ARCHITECTURE.md).

## File Header Requirement

Every `.c` file MUST have a docstring matching its [FILE_MANIFEST.md](FILE_MANIFEST.md) entry:

```c
/*
 * capture.c - DXGI Desktop Duplication - acquire frames from desktop
 */
```

If the docstring doesn't match the manifest, either:
1. The file has drifted from its purpose → refactor
2. The manifest is outdated → update it

## Design Principles

- **YAGNI ruthlessly** - Remove unnecessary features, don't add "just in case" code
- **Ask before adding** - One question at a time, prefer multiple choice
- **Explore alternatives** - Propose 2-3 approaches with trade-offs before implementing
- **Incremental validation** - Present designs in sections, validate each before continuing

---

# Audit Checklist

Questions to ask when reviewing any file. See [FILE_MANIFEST.md](FILE_MANIFEST.md) for what each file should do.

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

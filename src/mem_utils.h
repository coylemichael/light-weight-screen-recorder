/*
 * mem_utils.h - Memory Safety Utilities
 * ============================================================================
 * 
 * This header provides macros and utilities for safer memory management:
 * 
 *   1. SAFE_FREE / SAFE_RELEASE - Null-checked free that sets pointer to NULL
 *   2. Debug allocation tracking (optional, enabled via LWSR_DEBUG_MEMORY)
 *   3. Goto-cleanup pattern documentation and helpers
 * 
 * USAGE: Include this header in source files that allocate memory.
 * 
 * WHY THIS MATTERS:
 *   - Double-free bugs crash the program
 *   - Use-after-free bugs cause undefined behavior
 *   - Setting pointers to NULL after free prevents both issues
 *   - Debug tracking helps identify leaks during development
 */

#ifndef MEM_UTILS_H
#define MEM_UTILS_H

#include <stdlib.h>
#include <windows.h>

/* ============================================================================
 * SAFE FREE MACROS
 * ============================================================================
 * 
 * These macros wrap free() to:
 *   1. Check for NULL before freeing (safe but redundant - free(NULL) is safe)
 *   2. Set the pointer to NULL after freeing (prevents use-after-free)
 * 
 * The (void)0 in the else branch silences "empty statement" warnings.
 * 
 * USAGE:
 *   SAFE_FREE(buffer);           // Instead of: free(buffer); buffer = NULL;
 *   SAFE_RELEASE(comObject);     // For COM objects with Release() method
 */

#define SAFE_FREE(ptr) \
    do { \
        if ((ptr) != NULL) { \
            free(ptr); \
            (ptr) = NULL; \
        } \
    } while (0)

/* For arrays allocated with calloc/malloc */
#define SAFE_FREE_ARRAY(arr, count, freeFunc) \
    do { \
        if ((arr) != NULL) { \
            for (int _i = 0; _i < (count); _i++) { \
                freeFunc(&(arr)[_i]); \
            } \
            free(arr); \
            (arr) = NULL; \
        } \
    } while (0)

/* For COM objects that have Release() method */
#define SAFE_RELEASE(ptr) \
    do { \
        if ((ptr) != NULL) { \
            (ptr)->lpVtbl->Release(ptr); \
            (ptr) = NULL; \
        } \
    } while (0)

/* For CoTaskMemAlloc'd memory (COM allocator) */
#define SAFE_COTASKMEM_FREE(ptr) \
    do { \
        if ((ptr) != NULL) { \
            CoTaskMemFree(ptr); \
            (ptr) = NULL; \
        } \
    } while (0)

/* For handles (events, threads, etc.) */
#define SAFE_CLOSE_HANDLE(h) \
    do { \
        if ((h) != NULL) { \
            CloseHandle(h); \
            (h) = NULL; \
        } \
    } while (0)

/* ============================================================================
 * ALLOCATION RESULT CHECKING
 * ============================================================================
 * 
 * CHECK_ALLOC: Verify allocation succeeded, goto cleanup label if not.
 * This enables the goto-cleanup pattern for multi-resource functions.
 * 
 * USAGE:
 *   buffer = malloc(size);
 *   CHECK_ALLOC(buffer, cleanup);  // Jumps to 'cleanup' label if NULL
 */

#define CHECK_ALLOC(ptr, cleanupLabel) \
    do { \
        if ((ptr) == NULL) { \
            goto cleanupLabel; \
        } \
    } while (0)

#define CHECK_ALLOC_LOG(ptr, cleanupLabel, msg) \
    do { \
        if ((ptr) == NULL) { \
            Logger_Log("ALLOC FAILED: %s\n", msg); \
            goto cleanupLabel; \
        } \
    } while (0)

/* ============================================================================
 * COM/HRESULT ERROR CHECKING
 * ============================================================================
 * 
 * CHECK_HR: Verify HRESULT succeeded, goto cleanup label if not.
 * This enables consistent error handling for COM/Media Foundation calls.
 * 
 * PATTERN FOR MFCreate* CALLS:
 *   1. Always check HRESULT before using the created object
 *   2. Initialize all COM pointers to NULL at function start
 *   3. Release in reverse order of creation in cleanup
 *   4. Use SAFE_RELEASE which checks for NULL
 * 
 * USAGE:
 *   HRESULT hr = MFCreateMediaType(&type);
 *   CHECK_HR(hr, cleanup);  // Jumps to 'cleanup' label if FAILED
 *   
 *   hr = MFCreateSample(&sample);
 *   CHECK_HR_LOG(hr, cleanup, "MFCreateSample");  // Also logs on failure
 */

#define CHECK_HR(hr, cleanupLabel) \
    do { \
        if (FAILED(hr)) { \
            goto cleanupLabel; \
        } \
    } while (0)

#define CHECK_HR_LOG(hr, cleanupLabel, context) \
    do { \
        if (FAILED(hr)) { \
            Logger_Log("%s failed (0x%08X)\n", context, hr); \
            goto cleanupLabel; \
        } \
    } while (0)

/* Combined: Create and check in one statement */
#define CHECK_MF_CREATE(createExpr, cleanupLabel) \
    do { \
        HRESULT _hr = (createExpr); \
        if (FAILED(_hr)) { \
            goto cleanupLabel; \
        } \
    } while (0)

#define CHECK_MF_CREATE_LOG(createExpr, cleanupLabel, context) \
    do { \
        HRESULT _hr = (createExpr); \
        if (FAILED(_hr)) { \
            Logger_Log("%s failed (0x%08X)\n", context, _hr); \
            goto cleanupLabel; \
        } \
    } while (0)

/* ============================================================================
 * DEBUG MEMORY TRACKING (Optional)
 * ============================================================================
 * 
 * When LWSR_DEBUG_MEMORY is defined, these macros track all allocations
 * and can report leaks at shutdown.
 * 
 * In release builds, they expand to regular malloc/free with no overhead.
 */

#ifdef LWSR_DEBUG_MEMORY

/* Forward declarations for debug tracking functions */
void* MemDebug_Malloc(size_t size, const char* file, int line);
void* MemDebug_Calloc(size_t count, size_t size, const char* file, int line);
void* MemDebug_Realloc(void* ptr, size_t size, const char* file, int line);
void  MemDebug_Free(void* ptr, const char* file, int line);
void  MemDebug_ReportLeaks(void);
void  MemDebug_Init(void);
void  MemDebug_Shutdown(void);

#define DEBUG_MALLOC(size)          MemDebug_Malloc((size), __FILE__, __LINE__)
#define DEBUG_CALLOC(count, size)   MemDebug_Calloc((count), (size), __FILE__, __LINE__)
#define DEBUG_REALLOC(ptr, size)    MemDebug_Realloc((ptr), (size), __FILE__, __LINE__)
#define DEBUG_FREE(ptr)             MemDebug_Free((ptr), __FILE__, __LINE__)

#else

/* Release build: zero overhead, maps directly to stdlib functions */
#define DEBUG_MALLOC(size)          malloc(size)
#define DEBUG_CALLOC(count, size)   calloc((count), (size))
#define DEBUG_REALLOC(ptr, size)    realloc((ptr), (size))
#define DEBUG_FREE(ptr)             free(ptr)
#define MemDebug_ReportLeaks()      ((void)0)
#define MemDebug_Init()             ((void)0)
#define MemDebug_Shutdown()         ((void)0)

#endif /* LWSR_DEBUG_MEMORY */

/* ============================================================================
 * GOTO-CLEANUP PATTERN DOCUMENTATION
 * ============================================================================
 * 
 * The goto-cleanup pattern is the standard C idiom for functions that 
 * acquire multiple resources. It ensures all resources are freed on any
 * error path without code duplication.
 * 
 * PATTERN FOR COM/MEDIA FOUNDATION:
 * 
 *   BOOL CreateMediaResources(void) {
 *       BOOL result = FALSE;
 *       IMFMediaType* typeA = NULL;
 *       IMFMediaType* typeB = NULL;
 *       IMFSample* sample = NULL;
 *       IMFMediaBuffer* buffer = NULL;
 *       
 *       // CREATE AND CHECK - never use object before checking
 *       HRESULT hr = MFCreateMediaType(&typeA);
 *       CHECK_HR_LOG(hr, cleanup, "MFCreateMediaType(A)");
 *       
 *       hr = MFCreateMediaType(&typeB);
 *       CHECK_HR_LOG(hr, cleanup, "MFCreateMediaType(B)");
 *       
 *       // For buffer+sample pairs, check each before proceeding
 *       hr = MFCreateMemoryBuffer(size, &buffer);
 *       CHECK_HR(hr, cleanup);
 *       
 *       hr = MFCreateSample(&sample);
 *       CHECK_HR(hr, cleanup);
 *       
 *       // ... use the objects ...
 *       
 *       result = TRUE;
 *       
 *   cleanup:
 *       // RELEASE IN REVERSE ORDER of creation
 *       SAFE_RELEASE(sample);
 *       SAFE_RELEASE(buffer);
 *       SAFE_RELEASE(typeB);
 *       SAFE_RELEASE(typeA);
 *       return result;
 *   }
 * 
 * PATTERN FOR REGULAR ALLOCATIONS:
 * 
 *   BOOL DoSomething(void) {
 *       BOOL result = FALSE;
 *       ResourceA* a = NULL;
 *       ResourceB* b = NULL;
 *       
 *       a = CreateResourceA();
 *       CHECK_ALLOC(a, cleanup);
 *       
 *       b = CreateResourceB();
 *       CHECK_ALLOC(b, cleanup);
 *       
 *       result = TRUE;
 *       
 *   cleanup:
 *       SAFE_FREE(b);
 *       SAFE_FREE(a);
 *       return result;
 *   }
 * 
 * KEY PRINCIPLES:
 *   1. Initialize ALL resource pointers to NULL at function start
 *   2. ALWAYS check creation result before using the object
 *   3. Use a single cleanup label at the end
 *   4. Cleanup in REVERSE ORDER of acquisition
 *   5. Use SAFE_* macros that check for NULL
 *   6. Set success flag only after all operations complete
 *
 * See main.c for project-wide error handling standards.
 */

#endif /* MEM_UTILS_H */

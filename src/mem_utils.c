/*
 * mem_utils.c - Debug Memory Tracking Implementation
 * ============================================================================
 * 
 * This file implements allocation tracking for debugging memory leaks.
 * Only compiled when LWSR_DEBUG_MEMORY is defined.
 * 
 * HOW IT WORKS:
 *   - Each allocation is recorded in a hash table with file/line info
 *   - Each free removes the corresponding entry
 *   - At shutdown, any remaining entries are reported as leaks
 * 
 * LIMITATIONS:
 *   - Adds overhead (~40 bytes per allocation + hash table operations)
 *   - Not thread-safe (use critical section wrapper in multi-threaded code)
 *   - Only tracks DEBUG_MALLOC/FREE, not direct malloc/free calls
 * 
 * TO ENABLE:
 *   1. Define LWSR_DEBUG_MEMORY in your build (e.g., -DLWSR_DEBUG_MEMORY)
 *   2. Call MemDebug_Init() at program start
 *   3. Call MemDebug_ReportLeaks() before program exit
 *   4. Call MemDebug_Shutdown() at very end
 */

#ifdef LWSR_DEBUG_MEMORY

#include "mem_utils.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * ALLOCATION TRACKING DATA STRUCTURES
 * ============================================================================ */

#define ALLOC_TABLE_SIZE 4096  /* Hash table size, power of 2 for fast modulo */
#define MAX_FILENAME_LEN 64    /* Truncate long paths */

typedef struct AllocEntry {
    void* ptr;                      /* Allocated pointer */
    size_t size;                    /* Allocation size */
    char file[MAX_FILENAME_LEN];    /* Source file (truncated) */
    int line;                       /* Line number */
    struct AllocEntry* next;        /* Hash chain */
} AllocEntry;

/* ============================================================================
 * MEMORY TRACKING STATE
 * ============================================================================
 * Protected by g_allocLock critical section.
 * Thread Access: [Any thread - uses critical section]
 * Note: g_initialized uses InterlockedCompareExchange for thread-safe init.
 */
static AllocEntry* g_allocTable[ALLOC_TABLE_SIZE] = {0};
static CRITICAL_SECTION g_allocLock;
static volatile LONG g_initialized = FALSE;  /* Thread-safe: use InterlockedCompareExchange */
static size_t g_totalAllocated = 0;
static size_t g_totalFreed = 0;
static size_t g_peakUsage = 0;
static int g_allocCount = 0;
static int g_freeCount = 0;

/* Simple hash function for pointer addresses */
static unsigned int HashPtr(void* ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    return (unsigned int)((addr >> 4) % ALLOC_TABLE_SIZE);
}

/* Extract just the filename from a full path */
static const char* GetFilename(const char* path) {
    const char* lastSlash = strrchr(path, '\\');
    if (!lastSlash) lastSlash = strrchr(path, '/');
    return lastSlash ? lastSlash + 1 : path;
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

void MemDebug_Init(void) {
    /* Thread-safe initialization using atomic compare-exchange */
    if (InterlockedCompareExchange(&g_initialized, FALSE, FALSE)) return;
    
    /* Use a static INIT_ONCE to ensure single initialization */
    static INIT_ONCE s_initOnce = INIT_ONCE_STATIC_INIT;
    static volatile LONG s_initStarted = FALSE;
    
    /* Prevent multiple threads from initializing simultaneously */
    if (InterlockedCompareExchange(&s_initStarted, TRUE, FALSE) == FALSE) {
        InitializeCriticalSection(&g_allocLock);
        memset(g_allocTable, 0, sizeof(g_allocTable));
        g_totalAllocated = 0;
        g_totalFreed = 0;
        g_peakUsage = 0;
        g_allocCount = 0;
        g_freeCount = 0;
        InterlockedExchange(&g_initialized, TRUE);
        Logger_Log("MemDebug: Initialized allocation tracking\n");
    } else {
        /* Another thread is initializing - spin until done */
        while (!InterlockedCompareExchange(&g_initialized, FALSE, FALSE)) {
            Sleep(1);
        }
    }
}

void MemDebug_Shutdown(void) {
    /* Thread-safe check using atomic read */
    if (!InterlockedCompareExchange(&g_initialized, FALSE, FALSE)) return;
    
    /* Free any remaining entries (they're leaks, but clean up anyway) */
    for (int i = 0; i < ALLOC_TABLE_SIZE; i++) {
        AllocEntry* entry = g_allocTable[i];
        while (entry) {
            AllocEntry* next = entry->next;
            free(entry);  /* Free the tracking entry, not the leaked memory */
            entry = next;
        }
        g_allocTable[i] = NULL;
    }
    
    DeleteCriticalSection(&g_allocLock);
    InterlockedExchange(&g_initialized, FALSE);
}

void* MemDebug_Malloc(size_t size, const char* file, int line) {
    /* Thread-safe initialization check using atomic read */
    if (!InterlockedCompareExchange(&g_initialized, FALSE, FALSE)) MemDebug_Init();
    
    void* ptr = malloc(size);
    if (!ptr) return NULL;
    
    AllocEntry* entry = (AllocEntry*)malloc(sizeof(AllocEntry));
    if (!entry) {
        /* Can't track, but still return the allocation */
        return ptr;
    }
    
    entry->ptr = ptr;
    entry->size = size;
    strncpy(entry->file, GetFilename(file), MAX_FILENAME_LEN - 1);
    entry->file[MAX_FILENAME_LEN - 1] = '\0';
    entry->line = line;
    
    EnterCriticalSection(&g_allocLock);
    
    unsigned int hash = HashPtr(ptr);
    entry->next = g_allocTable[hash];
    g_allocTable[hash] = entry;
    
    g_totalAllocated += size;
    g_allocCount++;
    
    size_t currentUsage = g_totalAllocated - g_totalFreed;
    if (currentUsage > g_peakUsage) {
        g_peakUsage = currentUsage;
    }
    
    LeaveCriticalSection(&g_allocLock);
    
    return ptr;
}

void* MemDebug_Calloc(size_t count, size_t size, const char* file, int line) {
    // Check for integer overflow before multiplication
    if (count != 0 && size > SIZE_MAX / count) {
        Logger_Log("MemDebug: Calloc overflow at %s:%d (count=%zu, size=%zu)\n",
                   GetFilename(file), line, count, size);
        return NULL;
    }
    
    size_t total = count * size;
    void* ptr = MemDebug_Malloc(total, file, line);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* MemDebug_Realloc(void* ptr, size_t size, const char* file, int line) {
    if (!ptr) {
        return MemDebug_Malloc(size, file, line);
    }
    if (size == 0) {
        MemDebug_Free(ptr, file, line);
        return NULL;
    }
    
    /* Find and remove old entry */
    size_t oldSize = 0;
    
    EnterCriticalSection(&g_allocLock);
    
    unsigned int oldHash = HashPtr(ptr);
    AllocEntry** pp = &g_allocTable[oldHash];
    while (*pp) {
        if ((*pp)->ptr == ptr) {
            AllocEntry* old = *pp;
            oldSize = old->size;
            *pp = old->next;
            free(old);
            g_freeCount++;
            g_totalFreed += oldSize;
            break;
        }
        pp = &(*pp)->next;
    }
    
    LeaveCriticalSection(&g_allocLock);
    
    /* Perform realloc */
    void* newPtr = realloc(ptr, size);
    if (!newPtr) return NULL;
    
    /* Add new entry */
    AllocEntry* entry = (AllocEntry*)malloc(sizeof(AllocEntry));
    if (entry) {
        entry->ptr = newPtr;
        entry->size = size;
        strncpy(entry->file, GetFilename(file), MAX_FILENAME_LEN - 1);
        entry->file[MAX_FILENAME_LEN - 1] = '\0';
        entry->line = line;
        
        EnterCriticalSection(&g_allocLock);
        
        unsigned int hash = HashPtr(newPtr);
        entry->next = g_allocTable[hash];
        g_allocTable[hash] = entry;
        
        g_totalAllocated += size;
        g_allocCount++;
        
        size_t currentUsage = g_totalAllocated - g_totalFreed;
        if (currentUsage > g_peakUsage) {
            g_peakUsage = currentUsage;
        }
        
        LeaveCriticalSection(&g_allocLock);
    }
    
    return newPtr;
}

void MemDebug_Free(void* ptr, const char* file, int line) {
    if (!ptr) return;
    /* Thread-safe check using atomic read */
    if (!InterlockedCompareExchange(&g_initialized, FALSE, FALSE)) {
        free(ptr);  /* Not tracking, just free */
        return;
    }
    
    EnterCriticalSection(&g_allocLock);
    
    unsigned int hash = HashPtr(ptr);
    AllocEntry** pp = &g_allocTable[hash];
    BOOL found = FALSE;
    
    while (*pp) {
        if ((*pp)->ptr == ptr) {
            AllocEntry* entry = *pp;
            *pp = entry->next;
            
            g_totalFreed += entry->size;
            g_freeCount++;
            
            free(entry);
            found = TRUE;
            break;
        }
        pp = &(*pp)->next;
    }
    
    LeaveCriticalSection(&g_allocLock);
    
    if (!found) {
        Logger_Log("MemDebug: WARNING - Free of untracked pointer %p at %s:%d\n",
                   ptr, GetFilename(file), line);
    }
    
    free(ptr);
}

void MemDebug_ReportLeaks(void) {
    /* Thread-safe check using atomic read */
    if (!InterlockedCompareExchange(&g_initialized, FALSE, FALSE)) return;
    
    EnterCriticalSection(&g_allocLock);
    
    int leakCount = 0;
    size_t leakBytes = 0;
    
    Logger_Log("\n");
    Logger_Log("=== MEMORY LEAK REPORT ===\n");
    Logger_Log("Total allocations: %d\n", g_allocCount);
    Logger_Log("Total frees: %d\n", g_freeCount);
    Logger_Log("Peak memory usage: %zu bytes (%.2f MB)\n", 
               g_peakUsage, g_peakUsage / (1024.0 * 1024.0));
    Logger_Log("\n");
    
    for (int i = 0; i < ALLOC_TABLE_SIZE; i++) {
        AllocEntry* entry = g_allocTable[i];
        while (entry) {
            Logger_Log("LEAK: %zu bytes at %p - allocated at %s:%d\n",
                       entry->size, entry->ptr, entry->file, entry->line);
            leakCount++;
            leakBytes += entry->size;
            entry = entry->next;
        }
    }
    
    if (leakCount == 0) {
        Logger_Log("No memory leaks detected!\n");
    } else {
        Logger_Log("\nTotal leaks: %d allocations, %zu bytes (%.2f MB)\n",
                   leakCount, leakBytes, leakBytes / (1024.0 * 1024.0));
    }
    Logger_Log("=== END LEAK REPORT ===\n\n");
    
    LeaveCriticalSection(&g_allocLock);
}

#endif /* LWSR_DEBUG_MEMORY */

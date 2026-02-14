/*
 * frame_buffer.c - Circular Frame Buffer for Replay
 * 
 * USED BY: replay_buffer.c ONLY
 * 
 * Thread-safe circular buffer storing encoded HEVC frames.
 * Maintains a rolling window of the last N seconds for instant replay saves.
 * Recording uses StreamingMuxer instead (no buffering needed).
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation/precondition checks
 * - No HRESULT usage - pure memory buffer operations
 * - Allocation failures logged and return FALSE
 * - Thread-safe access via CRITICAL_SECTION
 * - Returns BOOL to propagate errors; callers must check
 */

#include "frame_buffer.h"
#include "mp4_muxer.h"
#include "logger.h"
#include "constants.h"
#include "leak_tracker.h"
#include "mem_utils.h"

// Alias for logging
#define BufLog Logger_Log

// Free a single frame
static void FreeFrame(BufferedFrame* frame) {
    LWSR_ASSERT(frame != NULL);
    if (frame->data) {
        LEAK_TRACK_FRAME_BUFFER_FREE();
        free(frame->data);
        frame->data = NULL;
    }
    frame->size = 0;
    frame->timestamp = 0;
    frame->duration = 0;
    frame->isKeyframe = FALSE;
}

// Evict oldest frames until buffer duration is under maxDuration
// Uses real timestamps: newest_timestamp - oldest_timestamp
static void EvictOldFrames(FrameBuffer* buf, LONGLONG newTimestamp) {
    if (buf->count == 0) return;
    
    int evicted = 0;
    
    // Keep evicting while (newest - oldest) > maxDuration
    while (buf->count > 0) {
        BufferedFrame* oldest = &buf->frames[buf->tail];
        
        // Calculate current buffer span using real timestamps
        LONGLONG bufferSpan = newTimestamp - oldest->timestamp;
        
        if (bufferSpan <= buf->maxDuration) {
            break;  // Within limit, stop evicting
        }
        
        // Evict oldest frame
        FreeFrame(oldest);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
        evicted++;
    }
    
    // Also check capacity limit
    while (buf->count >= buf->capacity) {
        BufferedFrame* oldest = &buf->frames[buf->tail];
        FreeFrame(oldest);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
        evicted++;
    }
    
    // Log eviction occasionally to show buffer is working
    static int evictLogCounter = 0;
    evictLogCounter++;
    if (evicted > 0 && (evictLogCounter % EVICT_LOG_INTERVAL) == 0 && buf->count > 0) {
        double span = (double)(newTimestamp - buf->frames[buf->tail].timestamp) / 10000000.0;
        BufLog("Eviction: removed %d frames, count now %d, span=%.2fs\n", 
               evicted, buf->count, span);
    }
}

/*
 * MULTI-RESOURCE FUNCTION: FrameBuffer_Init
 * Resources: 3 - frames array (calloc), critical section, initialized flag
 * Pattern: goto-cleanup with SAFE_FREE
 * Init: ZeroMemory ensures NULL initialization
 */
BOOL FrameBuffer_Init(FrameBuffer* buf, int durationSeconds, int fps,
                      int width, int height, QualityPreset quality) {
    // Preconditions
    LWSR_ASSERT(buf != NULL);
    LWSR_ASSERT(durationSeconds > 0);
    LWSR_ASSERT(fps > 0);
    LWSR_ASSERT(width > 0);
    LWSR_ASSERT(height > 0);
    
    if (!buf) return FALSE;
    if (durationSeconds <= 0 || fps <= 0 || width <= 0 || height <= 0) return FALSE;
    
    ZeroMemory(buf, sizeof(FrameBuffer));
    BOOL csInitialized = FALSE;
    
    // Calculate capacity: frames for 1.5x duration (headroom)
    // Use size_t to prevent overflow during calculation
    size_t rawCapacity = (size_t)durationSeconds * (size_t)fps;
    rawCapacity = (size_t)(rawCapacity * BUFFER_CAPACITY_HEADROOM);
    
    int capacity;
    if (rawCapacity < MIN_BUFFER_CAPACITY) {
        capacity = MIN_BUFFER_CAPACITY;
    } else if (rawCapacity > MAX_BUFFER_CAPACITY) {
        capacity = MAX_BUFFER_CAPACITY;  // ~27 min at 60fps
    } else {
        capacity = (int)rawCapacity;
    }
    
    // Verify allocation won't overflow
    size_t allocSize = (size_t)capacity * sizeof(BufferedFrame);
    if (allocSize / sizeof(BufferedFrame) != (size_t)capacity) {
        BufLog("FrameBuffer_Init: allocation size overflow\n");
        goto cleanup;
    }
    
    buf->frames = (BufferedFrame*)calloc(capacity, sizeof(BufferedFrame));
    if (!buf->frames) {
        BufLog("Failed to allocate %d frames\n", capacity);
        goto cleanup;
    }
    
    buf->capacity = capacity;
    buf->count = 0;
    buf->head = 0;
    buf->tail = 0;
    buf->maxDuration = (LONGLONG)durationSeconds * MF_UNITS_PER_SECOND;
    buf->width = width;
    buf->height = height;
    buf->fps = fps;
    buf->quality = quality;
    
    InitializeCriticalSection(&buf->lock);
    csInitialized = TRUE;
    buf->initialized = TRUE;
    
    BufLog("FrameBuffer_Init: capacity=%d, maxDuration=%llds\n", 
           capacity, buf->maxDuration / 10000000LL);
    
    return TRUE;
    
cleanup:
    SAFE_FREE(buf->frames);
    if (csInitialized) DeleteCriticalSection(&buf->lock);
    return FALSE;
}

void FrameBuffer_Shutdown(FrameBuffer* buf) {
    if (!buf) return;
    
    if (buf->initialized) {
        EnterCriticalSection(&buf->lock);
        
        // Free all frames
        for (int i = 0; i < buf->capacity; i++) {
            FreeFrame(&buf->frames[i]);
        }
        
        SAFE_FREE(buf->frames);
        
        LeaveCriticalSection(&buf->lock);
        DeleteCriticalSection(&buf->lock);
    }
    
    buf->initialized = FALSE;
    
    // Log cleanup is handled by replay_buffer.c
}

BOOL FrameBuffer_Add(FrameBuffer* buf, EncodedFrame* frame) {
    // Preconditions
    LWSR_ASSERT(buf != NULL);
    LWSR_ASSERT(frame != NULL);
    
    if (!buf || !buf->initialized || !frame || !frame->data) return FALSE;
    
    // Invariant checks
    LWSR_ASSERT(buf->count >= 0);
    LWSR_ASSERT(buf->count <= buf->capacity);
    LWSR_ASSERT(buf->head >= 0 && buf->head < buf->capacity);
    LWSR_ASSERT(buf->tail >= 0 && buf->tail < buf->capacity);
    
    EnterCriticalSection(&buf->lock);
    
    // Evict old frames based on timestamp (keeps last maxDuration seconds)
    EvictOldFrames(buf, frame->timestamp);
    
    // Add to buffer (take ownership of data)
    // Note: This transfers ownership from NVENC (allocated there) to FrameBuffer
    // Track as: NVENC free (releasing) + FrameBuffer alloc (acquiring)
    BufferedFrame* slot = &buf->frames[buf->head];
    
    // Free any existing data in slot (shouldn't happen after eviction)
    if (slot->data) {
        LEAK_TRACK_FRAME_BUFFER_FREE();
        free(slot->data);
    }
    
    // Transfer ownership: NVENC releases, FrameBuffer acquires
    LEAK_TRACK_NVENC_FRAME_FREE();
    LEAK_TRACK_FRAME_BUFFER_ALLOC();
    
    slot->data = frame->data;
    slot->size = frame->size;
    slot->timestamp = frame->timestamp;
    slot->duration = frame->duration;
    slot->isKeyframe = frame->isKeyframe;
    
    // Clear frame's pointer (we own it now)
    frame->data = NULL;
    frame->size = 0;
    
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count++;
    
    LeaveCriticalSection(&buf->lock);
    
    return TRUE;
}

double FrameBuffer_GetDuration(FrameBuffer* buf) {
    // Precondition
    LWSR_ASSERT(buf != NULL);
    
    if (!buf || !buf->initialized || buf->count == 0) return 0.0;
    
    EnterCriticalSection(&buf->lock);
    
    // Calculate duration from timestamps: newest - oldest
    int newestIdx = (buf->head - 1 + buf->capacity) % buf->capacity;
    BufferedFrame* newest = &buf->frames[newestIdx];
    BufferedFrame* oldest = &buf->frames[buf->tail];
    
    double duration = (double)(newest->timestamp - oldest->timestamp) / 10000000.0;
    
    LeaveCriticalSection(&buf->lock);
    
    return duration;
}

int FrameBuffer_GetCount(FrameBuffer* buf) {
    // Precondition
    LWSR_ASSERT(buf != NULL);
    
    if (!buf || !buf->initialized) return 0;
    
    EnterCriticalSection(&buf->lock);
    int count = buf->count;
    LeaveCriticalSection(&buf->lock);
    
    return count;
}

size_t FrameBuffer_GetMemoryUsage(FrameBuffer* buf) {
    // Precondition
    LWSR_ASSERT(buf != NULL);
    
    if (!buf || !buf->initialized) return 0;
    
    EnterCriticalSection(&buf->lock);
    
    size_t total = 0;
    int idx = buf->tail;
    for (int i = 0; i < buf->count; i++) {
        total += buf->frames[idx].size;
        idx = (idx + 1) % buf->capacity;
    }
    
    LeaveCriticalSection(&buf->lock);
    
    return total;
}

// Get copies of frames for external muxing (caller must free)
// Deep copies all data under lock to prevent use-after-free from eviction
// CRITICAL: Always starts from the first keyframe to ensure valid HEVC decoding
BOOL FrameBuffer_GetFramesForMuxing(FrameBuffer* buf, MuxerSample** outFrames, int* outCount) {
    // Preconditions
    LWSR_ASSERT(buf != NULL);
    LWSR_ASSERT(outFrames != NULL);
    LWSR_ASSERT(outCount != NULL);
    
    if (!buf || !buf->initialized || !outFrames || !outCount) return FALSE;
    
    *outFrames = NULL;
    *outCount = 0;
    
    EnterCriticalSection(&buf->lock);
    
    int count = buf->count;
    if (count == 0) {
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // CRITICAL FIX: Find the first keyframe (IDR) to start the clip
    // Without this, clips extracted after long encoding sessions have invalid
    // POC (Picture Order Count) references and decode with severe artifacts.
    // The decoder expects the clip to start with an IDR that resets POC.
    int startOffset = 0;
    for (int i = 0; i < count; i++) {
        BufferedFrame* src = &buf->frames[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0 && src->isKeyframe) {
            startOffset = i;
            BufLog("GetFramesForMuxing: Starting at keyframe offset %d (skipped %d frames)\n", 
                   startOffset, startOffset);
            break;
        }
    }
    
    // Calculate actual frame count after skipping to keyframe
    int actualCount = count - startOffset;
    if (actualCount <= 0) {
        LeaveCriticalSection(&buf->lock);
        BufLog("GetFramesForMuxing: No keyframe found in buffer!\n");
        return FALSE;
    }
    
    // Allocate output array with overflow check
    size_t allocSize = (size_t)actualCount * sizeof(MuxerSample);
    if (actualCount > 0 && allocSize / (size_t)actualCount != sizeof(MuxerSample)) {
        LeaveCriticalSection(&buf->lock);
        return FALSE;  // Overflow
    }
    MuxerSample* frames = (MuxerSample*)malloc(allocSize);
    if (!frames) {
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // Find first timestamp for normalization (from the keyframe we're starting at)
    LONGLONG firstTimestamp = 0;
    BufferedFrame* firstFrame = &buf->frames[(buf->tail + startOffset) % buf->capacity];
    if (firstFrame->data && firstFrame->size > 0) {
        firstTimestamp = firstFrame->timestamp;
    }
    
    // Zero-initialize to ensure safe cleanup on partial allocation failure
    memset(frames, 0, allocSize);
    
    // Deep copy frames starting from keyframe while holding lock (prevents use-after-free)
    int copiedCount = 0;
    BOOL allocFailed = FALSE;
    for (int i = startOffset; i < count && !allocFailed; i++) {
        BufferedFrame* src = &buf->frames[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0) {
            frames[copiedCount].data = (BYTE*)malloc(src->size);
            if (frames[copiedCount].data) {
                memcpy(frames[copiedCount].data, src->data, src->size);
                frames[copiedCount].size = src->size;
                frames[copiedCount].timestamp = src->timestamp - firstTimestamp;
                frames[copiedCount].duration = src->duration;
                frames[copiedCount].isKeyframe = src->isKeyframe;
                copiedCount++;
            } else {
                // Allocation failed - will clean up after releasing lock
                allocFailed = TRUE;
            }
        }
    }
    
    LeaveCriticalSection(&buf->lock);
    
    // Free the array if no frames were successfully copied or allocation failed
    if (copiedCount == 0 || allocFailed) {
        // Free any frames that were copied before failure
        for (int i = 0; i < copiedCount; i++) {
            if (frames[i].data) free(frames[i].data);
        }
        free(frames);
        return FALSE;
    }
    
    *outFrames = frames;
    *outCount = copiedCount;
    return TRUE;
}

void FrameBuffer_SetSequenceHeader(FrameBuffer* buf, const BYTE* header, DWORD size) {
    // Preconditions
    LWSR_ASSERT(buf != NULL);
    LWSR_ASSERT(header != NULL);
    LWSR_ASSERT(size > 0);
    LWSR_ASSERT(size <= sizeof(buf->seqHeader));
    
    if (!buf || !header || size == 0 || size > sizeof(buf->seqHeader)) return;
    
    EnterCriticalSection(&buf->lock);
    memcpy(buf->seqHeader, header, size);
    buf->seqHeaderSize = size;
    LeaveCriticalSection(&buf->lock);
    BufLog("SetSequenceHeader: %u bytes\n", size);
}

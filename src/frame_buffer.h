/*
 * frame_buffer.h - Circular Frame Buffer for Replay
 * 
 * USED BY: replay_buffer.c ONLY
 * 
 * Thread-safe ring buffer storing last N seconds of encoded HEVC frames.
 * Recording uses StreamingMuxer instead (writes directly to disk).
 */

#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <windows.h>
#include "nvenc_encoder.h"
#include "config.h"
#include "mp4_muxer.h"
#include "constants.h"

// Stored frame in the buffer
typedef struct {
    BYTE* data;             // HEVC NAL unit data
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Presentation time (100-ns units)
    LONGLONG duration;      // Frame duration (100-ns units)
    BOOL isKeyframe;        // TRUE if IDR frame
} BufferedFrame;

// Circular frame buffer
typedef struct {
    BufferedFrame* frames;      // Array of frames
    int capacity;               // Max frames in buffer
    int count;                  // Current frame count
    int head;                   // Next write position
    int tail;                   // Oldest frame position
    
    LONGLONG maxDuration;       // Target max duration (100-ns units)
    
    int width;                  // Video width
    int height;                 // Video height
    int fps;                    // Frame rate
    QualityPreset quality;      // Quality preset
    
    BYTE seqHeader[MAX_SEQ_HEADER_SIZE];  // HEVC VPS/SPS/PPS sequence header
    DWORD seqHeaderSize;        // Sequence header size
    
    CRITICAL_SECTION lock;      // Thread safety
    BOOL initialized;
    
} FrameBuffer;

// Initialize buffer for given duration
BOOL FrameBuffer_Init(FrameBuffer* buf, int durationSeconds, int fps, 
                      int width, int height, QualityPreset quality);

// Shutdown and free all resources
void FrameBuffer_Shutdown(FrameBuffer* buf);

// Add an encoded frame to the buffer
// Takes ownership of frame->data, caller should not free it
BOOL FrameBuffer_Add(FrameBuffer* buf, EncodedFrame* frame);

// Get current buffered duration in seconds
double FrameBuffer_GetDuration(FrameBuffer* buf);

// Get current frame count
int FrameBuffer_GetCount(FrameBuffer* buf);

// Get total memory usage in bytes
size_t FrameBuffer_GetMemoryUsage(FrameBuffer* buf);

// Get copies of frames for external muxing (caller must free)
// Used for audio+video muxing
BOOL FrameBuffer_GetFramesForMuxing(FrameBuffer* buf, MuxerSample** frames, int* count);

// Set HEVC sequence header (VPS/SPS/PPS) for muxing
void FrameBuffer_SetSequenceHeader(FrameBuffer* buf, const BYTE* header, DWORD size);

#endif // FRAME_BUFFER_H

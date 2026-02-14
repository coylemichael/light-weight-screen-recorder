/*
 * mp4_muxer.h - MP4 Container Muxer (HEVC Passthrough)
 * 
 * SHARED BY: replay_buffer.c (batch API), recording.c (streaming API)
 * 
 * Writes HEVC-encoded frames to MP4 via IMFSinkWriter.
 */

#ifndef MP4_MUXER_H
#define MP4_MUXER_H

#include <windows.h>
#include "config.h"

// Sample data for muxing (copies data from buffer)
typedef struct {
    BYTE* data;             // HEVC NAL unit data
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Sample time (100-ns units)
    LONGLONG duration;      // Sample duration (100-ns units)
    BOOL isKeyframe;        // TRUE if IDR frame
} MuxerSample;

// Audio sample for muxing
typedef struct {
    BYTE* data;             // AAC frame data
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Sample time (100-ns units)
    LONGLONG duration;      // Sample duration (100-ns units)
} MuxerAudioSample;

// Muxer configuration
typedef struct {
    int width;              // Video width
    int height;             // Video height
    int fps;                // Frame rate
    QualityPreset quality;  // For bitrate calculation
    BYTE* seqHeader;        // HEVC VPS/SPS/PPS sequence header
    DWORD seqHeaderSize;    // Size of sequence header
} MuxerConfig;

// Audio configuration
typedef struct {
    int sampleRate;         // e.g. 48000
    int channels;           // e.g. 2
    int bitrate;            // e.g. 192000
    BYTE* configData;       // AAC AudioSpecificConfig
    int configSize;
} MuxerAudioConfig;

// Write an array of samples to an MP4 file (video only)
// Uses HEVC passthrough muxing (no re-encoding)
// Returns TRUE on success
BOOL MP4Muxer_WriteFile(
    const char* outputPath,
    const MuxerSample* samples,
    int sampleCount,
    const MuxerConfig* config
);

// Write video and audio to MP4 file
BOOL MP4Muxer_WriteFileWithAudio(
    const char* outputPath,
    const MuxerSample* videoSamples,
    int videoSampleCount,
    const MuxerConfig* videoConfig,
    const MuxerAudioSample* audioSamples,
    int audioSampleCount,
    const MuxerAudioConfig* audioConfig
);

/* ============================================================================
 * STREAMING MUXER API
 * ============================================================================
 * For real-time recording where frames are written as they arrive.
 * Unlike batch API above, this keeps the file open and writes incrementally.
 */

// Opaque handle for streaming muxer
typedef struct StreamingMuxer StreamingMuxer;

// Create and open a streaming muxer for video-only recording
// Returns NULL on failure
StreamingMuxer* StreamingMuxer_Create(
    const char* outputPath,
    const MuxerConfig* videoConfig
);

// Create and open a streaming muxer with audio support
// audioConfig may be NULL if audio not needed initially
// Returns NULL on failure
StreamingMuxer* StreamingMuxer_CreateWithAudio(
    const char* outputPath,
    const MuxerConfig* videoConfig,
    const MuxerAudioConfig* audioConfig
);

// Write a single video sample (thread-safe)
// Returns TRUE on success
BOOL StreamingMuxer_WriteVideo(StreamingMuxer* muxer, const MuxerSample* sample);

// Write a single audio sample (thread-safe)
// Returns TRUE on success
BOOL StreamingMuxer_WriteAudio(StreamingMuxer* muxer, const MuxerAudioSample* sample);

// Finalize and close the muxer (writes moov atom, releases resources)
// Returns TRUE if file was written successfully
// After this call, the muxer handle is invalid
BOOL StreamingMuxer_Close(StreamingMuxer* muxer);

// Abort without finalizing (corrupted file, but fast cleanup)
void StreamingMuxer_Abort(StreamingMuxer* muxer);

#endif // MP4_MUXER_H

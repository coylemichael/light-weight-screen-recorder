/*
 * aac_encoder.h - Media Foundation AAC encoding
 */

#ifndef AAC_ENCODER_H
#define AAC_ENCODER_H

#include <windows.h>

// Forward declaration
typedef struct AACEncoder AACEncoder;

// Encoded AAC sample
typedef struct {
    BYTE* data;
    int size;
    LONGLONG timestamp;   // 100ns units
    LONGLONG duration;    // 100ns units
} AACSample;

// Callback for encoded samples
typedef void (*AACEncoderCallback)(const AACSample* sample, void* userData);

// Error codes for AAC encoder creation
typedef enum {
    AAC_OK = 0,                    // Success
    AAC_ERR_MEMORY,                // Memory allocation failed
    AAC_ERR_ENCODER_NOT_FOUND,     // No AAC encoder available (codec not installed)
    AAC_ERR_TYPE_NEGOTIATION,      // Failed to set input/output types
    AAC_ERR_START_STREAM,          // Failed to start encoder stream
    AAC_ERR_UNKNOWN                // Unknown error
} AACEncoderError;

// Check if AAC encoder is available (quick check for UI validation)
BOOL AACEncoder_IsAvailable(void);

// Create AAC encoder with error reporting
// Returns NULL on failure, error code in *outError if non-NULL
// Precondition: Caller must have initialized COM (CoInitializeEx) and
// Media Foundation (MFStartup) on this thread before calling.
AACEncoder* AACEncoder_CreateEx(AACEncoderError* outError);

// Destroy encoder
void AACEncoder_Destroy(AACEncoder* encoder);

// Set callback for encoded samples
void AACEncoder_SetCallback(AACEncoder* encoder, AACEncoderCallback callback, void* userData);

// Feed PCM samples to encoder
// pcmData: 16-bit stereo PCM at 48kHz
// pcmSize: size in bytes
// timestamp: presentation time in 100ns units
// Precondition: Caller must have initialized COM and Media Foundation on
// this thread. Not thread-safe; serialize calls from a single feeder thread.
BOOL AACEncoder_Feed(AACEncoder* encoder, const BYTE* pcmData, int pcmSize, LONGLONG timestamp);

// Get encoder info for muxer
BOOL AACEncoder_GetConfig(AACEncoder* encoder, BYTE** configData, int* configSize);

// Diagnostic accessors for A/V sync investigation.
// Thread-safety: 64-bit reads are atomic on x64 (LWSR's only build target).
// If ever ported to x86, add InterlockedRead64 or equivalent synchronization
// when called from threads other than the audio feeder thread.
// Total bytes of PCM ingested via AACEncoder_Feed (monotonic).
LONGLONG AACEncoder_GetPcmBytesIngested(AACEncoder* encoder);
// PTS (100ns units) of the most recently emitted AAC frame; 0 if none yet.
LONGLONG AACEncoder_GetLastEmittedTimestamp(AACEncoder* encoder);
// Total AAC frames emitted to the callback (monotonic).
LONGLONG AACEncoder_GetFramesEmitted(AACEncoder* encoder);

#endif // AAC_ENCODER_H

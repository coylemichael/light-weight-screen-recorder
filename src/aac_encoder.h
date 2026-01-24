/*
 * AAC Audio Encoder
 * Uses Media Foundation AAC encoder
 */

#ifndef AAC_ENCODER_H
#define AAC_ENCODER_H

#include <windows.h>
#include "audio_capture.h"
#include "constants.h"

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

// Create AAC encoder (legacy - no error info)
AACEncoder* AACEncoder_Create(void);

// Create AAC encoder with error reporting
// Returns NULL on failure, error code in *outError if non-NULL
AACEncoder* AACEncoder_CreateEx(AACEncoderError* outError);

// Destroy encoder
void AACEncoder_Destroy(AACEncoder* encoder);

// Set callback for encoded samples
void AACEncoder_SetCallback(AACEncoder* encoder, AACEncoderCallback callback, void* userData);

// Feed PCM samples to encoder
// pcmData: 16-bit stereo PCM at 48kHz
// pcmSize: size in bytes
// timestamp: presentation time in 100ns units
BOOL AACEncoder_Feed(AACEncoder* encoder, const BYTE* pcmData, int pcmSize, LONGLONG timestamp);

// Flush any remaining samples
void AACEncoder_Flush(AACEncoder* encoder);

// Get encoder info for muxer
BOOL AACEncoder_GetConfig(AACEncoder* encoder, BYTE** configData, int* configSize);

#endif // AAC_ENCODER_H

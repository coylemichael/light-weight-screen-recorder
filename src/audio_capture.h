/*
 * audio_capture.h - WASAPI capture + mixing from up to 3 sources
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <windows.h>

// Audio format (fixed for simplicity - all sources resampled to this)
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS          2
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_BLOCK_ALIGN       (AUDIO_CHANNELS * AUDIO_BITS_PER_SAMPLE / 8)
#define AUDIO_BYTES_PER_SEC     (AUDIO_SAMPLE_RATE * AUDIO_BLOCK_ALIGN)

// Maximum capture sources
#define MAX_AUDIO_SOURCES 3

// Forward declaration
typedef struct AudioCaptureSource AudioCaptureSource;

// Audio capture context (manages all sources)
typedef struct {
    AudioCaptureSource* sources[MAX_AUDIO_SOURCES];
    int sourceCount;
    
    // Per-source volume (0-100)
    int volumes[MAX_AUDIO_SOURCES];
    
    // Mixed audio buffer (ring buffer)
    BYTE* mixBuffer;
    int mixBufferSize;
    int mixBufferWritePos;
    int mixBufferReadPos;
    int mixBufferAvailable;
    CRITICAL_SECTION mixLock;
    
    // Capture thread
    HANDLE captureThread;
    volatile LONG running;  // Thread-safe: use InterlockedExchange
    
    // Timing
    LARGE_INTEGER startTime;
    LARGE_INTEGER perfFreq;
} AudioCaptureContext;

// Initialize audio capture system
BOOL AudioCapture_Init(void);

// Shutdown audio capture system
void AudioCapture_Shutdown(void);

// Create capture context with specified sources
// deviceIds can be NULL or empty string to skip that source
// volumes are 0-100 (percentage)
AudioCaptureContext* AudioCapture_Create(
    const char* deviceId1, int volume1,
    const char* deviceId2, int volume2,
    const char* deviceId3, int volume3
);

// Destroy capture context
void AudioCapture_Destroy(AudioCaptureContext* ctx);

// Start capturing audio
BOOL AudioCapture_Start(AudioCaptureContext* ctx);

// Stop capturing audio
void AudioCapture_Stop(AudioCaptureContext* ctx);

// Read mixed audio data (returns bytes read)
// Timestamp is in 100ns units (same as video)
int AudioCapture_Read(AudioCaptureContext* ctx, BYTE* buffer, int maxBytes, LONGLONG* timestamp);

#endif // AUDIO_CAPTURE_H

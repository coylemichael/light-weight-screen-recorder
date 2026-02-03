/*
 * encoder.h - Media Foundation H.264/HEVC/WMV sink writer for traditional recording
 */

#ifndef ENCODER_H
#define ENCODER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "config.h"

typedef struct {
    IMFSinkWriter* sinkWriter;
    DWORD videoStreamIndex;
    
    int width;
    int height;
    int fps;
    
    UINT64 frameDuration;
    UINT64 frameCount;
    UINT64 startTime;
    
    OutputFormat format;
    QualityPreset quality;
    
    volatile LONG initialized;  // Thread-safe: use InterlockedExchange
    volatile LONG recording;    // Thread-safe: use InterlockedExchange
    
    char outputPath[MAX_PATH];
    
} EncoderState;

// Initialize encoder with output file and settings
BOOL Encoder_Init(EncoderState* state, const char* outputPath, 
                  int width, int height, int fps,
                  OutputFormat format, QualityPreset quality);

// Write a frame (BGRA format)
BOOL Encoder_WriteFrame(EncoderState* state, const BYTE* frameData, UINT64 timestamp);

// Finalize and close the output file
void Encoder_Finalize(EncoderState* state);

// Generate output filename with timestamp
void Encoder_GenerateFilename(char* buffer, size_t size, 
                               const char* basePath, OutputFormat format);

#endif // ENCODER_H

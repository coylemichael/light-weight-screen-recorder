/*
 * Sample Buffer Implementation
 * Thread-safe circular buffer with MP4 passthrough muxing
 */

#include "sample_buffer.h"
#include "util.h"
#include "logger.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>

// Alias for logging
#define BufLog Logger_Log

// Free a single sample
static void FreeSample(BufferedSample* sample) {
    if (sample->data) {
        free(sample->data);
        sample->data = NULL;
    }
    sample->size = 0;
    sample->timestamp = 0;
    sample->duration = 0;
    sample->isKeyframe = FALSE;
}

// Evict oldest samples until we're under maxDuration
static void EvictOldSamples(SampleBuffer* buf, LONGLONG newSampleDuration) {
    // Keep evicting until we have room
    while (buf->count > 0 && (buf->totalDuration + newSampleDuration > buf->maxDuration)) {
        // Find next keyframe after tail to avoid partial GOPs
        // For simplicity, just evict one at a time
        BufferedSample* oldest = &buf->samples[buf->tail];
        
        buf->totalDuration -= oldest->duration;
        FreeSample(oldest);
        
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
    }
    
    // Also check capacity limit
    while (buf->count >= buf->capacity) {
        BufferedSample* oldest = &buf->samples[buf->tail];
        buf->totalDuration -= oldest->duration;
        FreeSample(oldest);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
    }
}

BOOL SampleBuffer_Init(SampleBuffer* buf, int durationSeconds, int fps,
                        int width, int height, QualityPreset quality) {
    if (!buf) return FALSE;
    
    ZeroMemory(buf, sizeof(SampleBuffer));
    
    // Calculate capacity: frames for 1.5x duration (headroom)
    int capacity = (int)(durationSeconds * fps * 1.5);
    if (capacity < 100) capacity = 100;
    if (capacity > 100000) capacity = 100000;  // ~27 min at 60fps
    
    buf->samples = (BufferedSample*)calloc(capacity, sizeof(BufferedSample));
    if (!buf->samples) {
        BufLog("Failed to allocate %d samples\n", capacity);
        return FALSE;
    }
    
    buf->capacity = capacity;
    buf->count = 0;
    buf->head = 0;
    buf->tail = 0;
    buf->totalDuration = 0;
    buf->maxDuration = (LONGLONG)durationSeconds * 10000000LL;  // 100-ns units
    buf->width = width;
    buf->height = height;
    buf->fps = fps;
    buf->quality = quality;
    
    InitializeCriticalSection(&buf->lock);
    buf->initialized = TRUE;
    
    BufLog("SampleBuffer_Init: capacity=%d, maxDuration=%llds\n", 
           capacity, buf->maxDuration / 10000000LL);
    
    return TRUE;
}

void SampleBuffer_Shutdown(SampleBuffer* buf) {
    if (!buf) return;
    
    if (buf->initialized) {
        EnterCriticalSection(&buf->lock);
        
        // Free all samples
        for (int i = 0; i < buf->capacity; i++) {
            FreeSample(&buf->samples[i]);
        }
        
        free(buf->samples);
        buf->samples = NULL;
        
        LeaveCriticalSection(&buf->lock);
        DeleteCriticalSection(&buf->lock);
    }
    
    buf->initialized = FALSE;
    
    // Log cleanup is handled by replay_buffer.c
}

BOOL SampleBuffer_Add(SampleBuffer* buf, EncodedFrame* frame) {
    if (!buf || !buf->initialized || !frame || !frame->data) return FALSE;
    
    EnterCriticalSection(&buf->lock);
    
    // Evict old samples if needed
    EvictOldSamples(buf, frame->duration);
    
    // Add to buffer (take ownership of data)
    BufferedSample* slot = &buf->samples[buf->head];
    
    // Free any existing data in slot (shouldn't happen after eviction)
    if (slot->data) {
        free(slot->data);
    }
    
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
    buf->totalDuration += slot->duration;
    
    LeaveCriticalSection(&buf->lock);
    
    return TRUE;
}

double SampleBuffer_GetDuration(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return 0.0;
    
    EnterCriticalSection(&buf->lock);
    double duration = (double)buf->totalDuration / 10000000.0;
    LeaveCriticalSection(&buf->lock);
    
    return duration;
}

int SampleBuffer_GetCount(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return 0;
    
    EnterCriticalSection(&buf->lock);
    int count = buf->count;
    LeaveCriticalSection(&buf->lock);
    
    return count;
}

size_t SampleBuffer_GetMemoryUsage(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return 0;
    
    EnterCriticalSection(&buf->lock);
    
    size_t total = 0;
    int idx = buf->tail;
    for (int i = 0; i < buf->count; i++) {
        total += buf->samples[idx].size;
        idx = (idx + 1) % buf->capacity;
    }
    
    LeaveCriticalSection(&buf->lock);
    
    return total;
}

void SampleBuffer_Clear(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return;
    
    EnterCriticalSection(&buf->lock);
    
    for (int i = 0; i < buf->capacity; i++) {
        FreeSample(&buf->samples[i]);
    }
    
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->totalDuration = 0;
    
    LeaveCriticalSection(&buf->lock);
}

// Write buffered samples to MP4 file using SinkWriter
// This re-encodes because passthrough muxing with raw H.264 NALs is complex
// For true passthrough, we'd need to write the MP4 manually
BOOL SampleBuffer_WriteToFile(SampleBuffer* buf, const char* outputPath) {
    if (!buf || !buf->initialized || !outputPath) return FALSE;
    
    EnterCriticalSection(&buf->lock);
    
    if (buf->count == 0) {
        LeaveCriticalSection(&buf->lock);
        BufLog("WriteToFile: buffer is empty\n");
        return FALSE;
    }
    
    BufLog("WriteToFile: %d samples, %.1fs to %s\n", 
           buf->count, (double)buf->totalDuration / 10000000.0, outputPath);
    
    // Convert path to wide string
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create SinkWriter
    IMFSinkWriter* writer = NULL;
    IMFAttributes* attrs = NULL;
    
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) {
        attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->lpVtbl->SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
    }
    
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attrs, &writer);
    if (attrs) attrs->lpVtbl->Release(attrs);
    
    if (FAILED(hr)) {
        BufLog("WriteToFile: MFCreateSinkWriterFromURL failed 0x%08X\n", hr);
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // Configure output type (H.264)
    IMFMediaType* outputType = NULL;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        writer->lpVtbl->Release(writer);
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // Calculate bitrate using shared utility
    UINT32 bitrate = Util_CalculateBitrate(buf->width, buf->height, buf->fps, buf->quality);
    
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_AVG_BITRATE, bitrate);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_MPEG2_PROFILE, 100);
    
    UINT64 frameSize = ((UINT64)buf->width << 32) | buf->height;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_SIZE, frameSize);
    
    // Use exact frame rate fraction for proper timing
    UINT64 frameRate = ((UINT64)buf->fps << 32) | 1;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_RATE, frameRate);
    
    // Set pixel aspect ratio (square pixels)
    UINT64 pixelAspect = ((UINT64)1 << 32) | 1;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_PIXEL_ASPECT_RATIO, pixelAspect);
    
    BufLog("WriteToFile: Output type: %dx%d @ %d fps, bitrate=%u\n", 
           buf->width, buf->height, buf->fps, bitrate);
    
    DWORD streamIndex = 0;
    hr = writer->lpVtbl->AddStream(writer, outputType, &streamIndex);
    
    if (FAILED(hr)) {
        BufLog("WriteToFile: AddStream failed 0x%08X\n", hr);
        outputType->lpVtbl->Release(outputType);
        writer->lpVtbl->Release(writer);
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // For H.264 passthrough, use the SAME type for input as output
    // The SinkWriter will just mux without transcoding
    hr = writer->lpVtbl->SetInputMediaType(writer, streamIndex, outputType, NULL);
    outputType->lpVtbl->Release(outputType);
    
    if (FAILED(hr)) {
        BufLog("WriteToFile: SetInputMediaType failed 0x%08X\n", hr);
        writer->lpVtbl->Release(writer);
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    BufLog("WriteToFile: Stream configured for H.264 passthrough\n");
    
    // Begin writing
    hr = writer->lpVtbl->BeginWriting(writer);
    if (FAILED(hr)) {
        BufLog("WriteToFile: BeginWriting failed 0x%08X\n", hr);
        writer->lpVtbl->Release(writer);
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // Write all samples with sequential timestamps
    // Use precise calculation to avoid rounding errors: time = (frame * 10000000) / fps
    // This ensures exact timing without cumulative drift
    int samplesWritten = 0;
    int idx = buf->tail;
    int frameNumber = 0;
    
    BufLog("WriteToFile: Writing %d samples at %d fps\n", buf->count, buf->fps);
    
    int keyframeCount = 0;
    for (int i = 0; i < buf->count; i++) {
        BufferedSample* sample = &buf->samples[idx];
        
        if (sample->data && sample->size > 0) {
            // Create MF buffer
            IMFMediaBuffer* mfBuffer = NULL;
            hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
            
            if (SUCCEEDED(hr)) {
                BYTE* bufData = NULL;
                hr = mfBuffer->lpVtbl->Lock(mfBuffer, &bufData, NULL, NULL);
                if (SUCCEEDED(hr)) {
                    memcpy(bufData, sample->data, sample->size);
                    mfBuffer->lpVtbl->Unlock(mfBuffer);
                    mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
                    
                    // Create MF sample with precise timestamps (no rounding accumulation)
                    IMFSample* mfSample = NULL;
                    hr = MFCreateSample(&mfSample);
                    if (SUCCEEDED(hr)) {
                        // Precise timestamp using utility function
                        LONGLONG sampleTime = Util_CalculateTimestamp(frameNumber, buf->fps);
                        LONGLONG sampleDuration = Util_CalculateFrameDuration(frameNumber, buf->fps);
                        
                        mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
                        mfSample->lpVtbl->SetSampleTime(mfSample, sampleTime);
                        mfSample->lpVtbl->SetSampleDuration(mfSample, sampleDuration);
                        
                        if (sample->isKeyframe) {
                            mfSample->lpVtbl->SetUINT32(mfSample, &MFSampleExtension_CleanPoint, TRUE);
                            keyframeCount++;
                        }
                        
                        hr = writer->lpVtbl->WriteSample(writer, streamIndex, mfSample);
                        if (SUCCEEDED(hr)) {
                            samplesWritten++;
                            frameNumber++;
                        } else {
                            BufLog("WriteSample failed at %d: 0x%08X\n", i, hr);
                        }
                        
                        mfSample->lpVtbl->Release(mfSample);
                    }
                }
                mfBuffer->lpVtbl->Release(mfBuffer);
            }
        }
        
        idx = (idx + 1) % buf->capacity;
    }
    
    // Final duration is exactly (frameCount * 10000000) / fps
    LONGLONG finalDuration = (LONGLONG)frameNumber * 10000000LL / buf->fps;
    BufLog("WriteToFile: Final timestamp: %lld (%.3fs), keyframes: %d\n", 
           finalDuration, (double)finalDuration / 10000000.0, keyframeCount);
    
    // Finalize
    hr = writer->lpVtbl->Finalize(writer);
    writer->lpVtbl->Release(writer);
    
    LeaveCriticalSection(&buf->lock);
    
    BufLog("WriteToFile: wrote %d/%d samples, finalize=%s\n", 
           samplesWritten, buf->count, SUCCEEDED(hr) ? "OK" : "FAILED");
    
    return SUCCEEDED(hr) && samplesWritten > 0;
}

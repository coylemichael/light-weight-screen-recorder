/*
 * Media Foundation Encoder Implementation
 * Using proper C-style COM vtable calls
 */

#include "encoder.h"
#include "util.h"
#include "constants.h"
#include "mem_utils.h"
#include <mferror.h>
#include <stdio.h>
#include <time.h>

static GUID GetVideoFormat(OutputFormat format) {
    switch (format) {
        case FORMAT_MP4: return MFVideoFormat_H264;
        case FORMAT_WMV: return MFVideoFormat_WMV3;
        case FORMAT_AVI: return MFVideoFormat_H264;
        default:         return MFVideoFormat_H264;
    }
}

void Encoder_GenerateFilename(char* buffer, size_t size, 
                               const char* basePath, OutputFormat format) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm_info);
    
    snprintf(buffer, size, "%s\\Recording_%s%s", 
             basePath, timestamp, Config_GetFormatExtension(format));
}

BOOL Encoder_Init(EncoderState* state, const char* outputPath,
                  int width, int height, int fps,
                  OutputFormat format, QualityPreset quality) {
    BOOL result = FALSE;
    IMFAttributes* attributes = NULL;
    IMFMediaType* outputType = NULL;
    IMFMediaType* inputType = NULL;
    
    ZeroMemory(state, sizeof(EncoderState));
    
    state->width = width;
    state->height = height;
    state->fps = fps;
    state->format = format;
    state->quality = quality;
    state->frameDuration = MF_UNITS_PER_SECOND / fps;
    
    strncpy(state->outputPath, outputPath, MAX_PATH - 1);
    state->outputPath[MAX_PATH - 1] = '\0';
    
    // Ensure output directory exists
    char dirPath[MAX_PATH];
    strncpy(dirPath, outputPath, MAX_PATH - 1);
    dirPath[MAX_PATH - 1] = '\0';
    char* lastSlash = strrchr(dirPath, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
        CreateDirectoryA(dirPath, NULL);
    }
    
    // Convert path to wide string (use ACP, not UTF8)
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create sink writer attributes
    HRESULT hr = MFCreateAttributes(&attributes, 3);
    if (FAILED(hr)) goto cleanup;
    
    // Enable hardware encoding (NVENC, Intel QSV, AMD VCE)
    attributes->lpVtbl->SetUINT32(attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    
    // Low latency mode - faster encoding, essential for real-time replay buffer
    attributes->lpVtbl->SetUINT32(attributes, &MF_LOW_LATENCY, TRUE);
    
    // Create sink writer
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attributes, &state->sinkWriter);
    if (FAILED(hr)) goto cleanup;
    
    // Configure output media type (encoded)
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) goto cleanup;
    
    GUID videoFormat = GetVideoFormat(format);
    UINT32 bitrate = Util_CalculateBitrate(width, height, fps, quality);
    
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_SUBTYPE, &videoFormat);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_AVG_BITRATE, bitrate);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    
    // Set H.264 profile (let encoder pick level automatically)
    if (format == FORMAT_MP4 || format == FORMAT_AVI) {
        // eAVEncH264VProfile_High = 100
        outputType->lpVtbl->SetUINT32(outputType, &MF_MT_MPEG2_PROFILE, H264_PROFILE_HIGH);
        // Don't set level - let encoder determine based on resolution/fps
    }
    
    // Set frame size
    UINT64 frameSize = ((UINT64)width << 32) | height;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_SIZE, frameSize);
    
    // Set frame rate
    UINT64 frameRate = ((UINT64)fps << 32) | 1;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_RATE, frameRate);
    
    // Set pixel aspect ratio
    UINT64 aspectRatio = ((UINT64)1 << 32) | 1;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_PIXEL_ASPECT_RATIO, aspectRatio);
    
    hr = state->sinkWriter->lpVtbl->AddStream(state->sinkWriter, outputType, &state->videoStreamIndex);
    if (FAILED(hr)) goto cleanup;
    
    // Configure input media type (raw BGRA)
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) goto cleanup;
    
    inputType->lpVtbl->SetGUID(inputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    inputType->lpVtbl->SetGUID(inputType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    inputType->lpVtbl->SetUINT32(inputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_FRAME_SIZE, frameSize);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_FRAME_RATE, frameRate);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_PIXEL_ASPECT_RATIO, aspectRatio);
    
    // Calculate stride (negative for top-down DIB)
    LONG stride = -((LONG)width * BYTES_PER_PIXEL_BGRA);
    inputType->lpVtbl->SetUINT32(inputType, &MF_MT_DEFAULT_STRIDE, (UINT32)stride);
    
    hr = state->sinkWriter->lpVtbl->SetInputMediaType(state->sinkWriter, state->videoStreamIndex, 
                                                       inputType, NULL);
    if (FAILED(hr)) goto cleanup;
    
    // Start writing
    hr = state->sinkWriter->lpVtbl->BeginWriting(state->sinkWriter);
    if (FAILED(hr)) goto cleanup;
    
    state->initialized = TRUE;
    state->recording = TRUE;
    state->frameCount = 0;
    state->startTime = 0;
    result = TRUE;
    
cleanup:
    SAFE_RELEASE(inputType);
    SAFE_RELEASE(outputType);
    SAFE_RELEASE(attributes);
    
    if (!result) {
        SAFE_RELEASE(state->sinkWriter);
    }
    
    return result;
}

BOOL Encoder_WriteFrame(EncoderState* state, const BYTE* frameData, UINT64 timestamp) {
    (void)timestamp;
    BOOL result = FALSE;
    IMFMediaBuffer* buffer = NULL;
    IMFSample* sample = NULL;
    BYTE* bufferData = NULL;
    
    if (!state->initialized || !state->recording) return FALSE;
    
    // Create media buffer
    DWORD bufferSize = state->width * state->height * BYTES_PER_PIXEL_BGRA;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &buffer);
    if (FAILED(hr)) goto cleanup;
    
    // Lock buffer and copy frame data
    hr = buffer->lpVtbl->Lock(buffer, &bufferData, NULL, NULL);
    if (FAILED(hr)) goto cleanup;
    
    // Copy frame data (flip vertically for Media Foundation)
    const BYTE* src = frameData + (state->height - 1) * state->width * BYTES_PER_PIXEL_BGRA;
    BYTE* dst = bufferData;
    int rowBytes = state->width * BYTES_PER_PIXEL_BGRA;
    
    for (int y = 0; y < state->height; y++) {
        memcpy(dst, src, rowBytes);
        src -= rowBytes;
        dst += rowBytes;
    }
    
    buffer->lpVtbl->Unlock(buffer);
    bufferData = NULL;  // Mark as unlocked
    buffer->lpVtbl->SetCurrentLength(buffer, bufferSize);
    
    // Create sample
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) goto cleanup;
    
    hr = sample->lpVtbl->AddBuffer(sample, buffer);
    if (FAILED(hr)) goto cleanup;
    
    // Set sample time
    UINT64 sampleTime = state->frameCount * state->frameDuration;
    sample->lpVtbl->SetSampleTime(sample, sampleTime);
    sample->lpVtbl->SetSampleDuration(sample, state->frameDuration);
    
    // Write sample
    hr = state->sinkWriter->lpVtbl->WriteSample(state->sinkWriter, state->videoStreamIndex, sample);
    if (SUCCEEDED(hr)) {
        state->frameCount++;
        result = TRUE;
    }
    
cleanup:
    if (bufferData && buffer) buffer->lpVtbl->Unlock(buffer);
    SAFE_RELEASE(sample);
    SAFE_RELEASE(buffer);
    
    return result;
}

void Encoder_Finalize(EncoderState* state) {
    if (!state->initialized) return;
    
    if (state->sinkWriter) {
        if (state->recording) {
            state->sinkWriter->lpVtbl->Finalize(state->sinkWriter);
        }
        state->sinkWriter->lpVtbl->Release(state->sinkWriter);
        state->sinkWriter = NULL;
    }
    
    state->initialized = FALSE;
    state->recording = FALSE;
}

const char* Encoder_GetOutputPath(EncoderState* state) {
    return state->outputPath;
}

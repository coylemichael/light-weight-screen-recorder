/*
 * Media Foundation Encoder Implementation
 * Using proper C-style COM vtable calls
 */

#include "encoder.h"
#include <mferror.h>
#include <stdio.h>
#include <time.h>

// Quality to bitrate mapping for screen recording
// Screen content (text, UI) needs higher bitrates than video for sharp edges
// Bitrates scale with resolution for consistent quality across sizes
static UINT32 GetBitrate(int width, int height, int fps, QualityPreset quality) {
    // Base bitrate per megapixel at 60fps
    // These are tuned for screen content which is more demanding than video
    float mbpPerMegapixel;
    
    switch (quality) {
        case QUALITY_LOW:      mbpPerMegapixel = 8.0f;  break;  // ~30 Mbps at 1440p60
        case QUALITY_MEDIUM:   mbpPerMegapixel = 15.0f; break;  // ~55 Mbps at 1440p60  
        case QUALITY_HIGH:     mbpPerMegapixel = 25.0f; break;  // ~90 Mbps at 1440p60
        case QUALITY_LOSSLESS: mbpPerMegapixel = 40.0f; break;  // ~150 Mbps at 1440p60 (visually lossless)
        default:               mbpPerMegapixel = 40.0f; break;
    }
    
    // Calculate megapixels and scale bitrate
    float megapixels = (float)(width * height) / 1000000.0f;
    float fpsScale = (float)fps / 60.0f; // Scale for framerate
    if (fpsScale < 0.5f) fpsScale = 0.5f;
    if (fpsScale > 1.0f) fpsScale = 1.0f; // Don't exceed 60fps scaling
    
    UINT32 bitrate = (UINT32)(megapixels * mbpPerMegapixel * fpsScale * 1000000.0f);
    
    // Reasonable bounds: 5 Mbps minimum, 200 Mbps maximum
    if (bitrate < 5000000) bitrate = 5000000;
    if (bitrate > 200000000) bitrate = 200000000;
    
    return bitrate;
}

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
    ZeroMemory(state, sizeof(EncoderState));
    
    state->width = width;
    state->height = height;
    state->fps = fps;
    state->format = format;
    state->quality = quality;
    state->frameDuration = 10000000ULL / fps; // 100-nanosecond units
    
    strncpy(state->outputPath, outputPath, MAX_PATH - 1);
    
    // Ensure output directory exists
    char dirPath[MAX_PATH];
    strncpy(dirPath, outputPath, MAX_PATH - 1);
    char* lastSlash = strrchr(dirPath, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
        CreateDirectoryA(dirPath, NULL);
    }
    
    // Convert path to wide string (use ACP, not UTF8)
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create sink writer attributes
    IMFAttributes* attributes = NULL;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) return FALSE;
    
    // Enable hardware encoding if available
    hr = attributes->lpVtbl->SetUINT32(attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    
    // Create sink writer
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attributes, &state->sinkWriter);
    attributes->lpVtbl->Release(attributes);
    if (FAILED(hr)) return FALSE;
    
    // Configure output media type (encoded)
    IMFMediaType* outputType = NULL;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        state->sinkWriter->lpVtbl->Release(state->sinkWriter);
        return FALSE;
    }
    
    GUID videoFormat = GetVideoFormat(format);
    UINT32 bitrate = GetBitrate(width, height, fps, quality);
    
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_SUBTYPE, &videoFormat);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_AVG_BITRATE, bitrate);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    
    // Use H.264 High Profile for better compression efficiency at high bitrates
    if (format == FORMAT_MP4 || format == FORMAT_AVI) {
        // eAVEncH264VProfile_High = 100
        outputType->lpVtbl->SetUINT32(outputType, &MF_MT_MPEG2_PROFILE, 100);
        // Level 5.1 supports up to 4K60
        outputType->lpVtbl->SetUINT32(outputType, &MF_MT_MPEG2_LEVEL, 51);
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
    outputType->lpVtbl->Release(outputType);
    if (FAILED(hr)) {
        state->sinkWriter->lpVtbl->Release(state->sinkWriter);
        return FALSE;
    }
    
    // Configure input media type (raw BGRA)
    IMFMediaType* inputType = NULL;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        state->sinkWriter->lpVtbl->Release(state->sinkWriter);
        return FALSE;
    }
    
    inputType->lpVtbl->SetGUID(inputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    inputType->lpVtbl->SetGUID(inputType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    inputType->lpVtbl->SetUINT32(inputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_FRAME_SIZE, frameSize);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_FRAME_RATE, frameRate);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_PIXEL_ASPECT_RATIO, aspectRatio);
    
    // Calculate stride (negative for top-down DIB)
    LONG stride = -((LONG)width * 4);
    inputType->lpVtbl->SetUINT32(inputType, &MF_MT_DEFAULT_STRIDE, (UINT32)stride);
    
    hr = state->sinkWriter->lpVtbl->SetInputMediaType(state->sinkWriter, state->videoStreamIndex, 
                                                       inputType, NULL);
    inputType->lpVtbl->Release(inputType);
    if (FAILED(hr)) {
        state->sinkWriter->lpVtbl->Release(state->sinkWriter);
        return FALSE;
    }
    
    // Start writing
    hr = state->sinkWriter->lpVtbl->BeginWriting(state->sinkWriter);
    if (FAILED(hr)) {
        state->sinkWriter->lpVtbl->Release(state->sinkWriter);
        return FALSE;
    }
    
    state->initialized = TRUE;
    state->recording = TRUE;
    state->frameCount = 0;
    state->startTime = 0;
    
    return TRUE;
}

BOOL Encoder_WriteFrame(EncoderState* state, const BYTE* frameData, UINT64 timestamp) {
    (void)timestamp;
    if (!state->initialized || !state->recording) return FALSE;
    
    // Create media buffer
    DWORD bufferSize = state->width * state->height * 4;
    IMFMediaBuffer* buffer = NULL;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &buffer);
    if (FAILED(hr)) return FALSE;
    
    // Lock buffer and copy frame data
    BYTE* bufferData = NULL;
    hr = buffer->lpVtbl->Lock(buffer, &bufferData, NULL, NULL);
    if (FAILED(hr)) {
        buffer->lpVtbl->Release(buffer);
        return FALSE;
    }
    
    // Copy frame data (flip vertically for Media Foundation)
    const BYTE* src = frameData + (state->height - 1) * state->width * 4;
    BYTE* dst = bufferData;
    int rowBytes = state->width * 4;
    
    for (int y = 0; y < state->height; y++) {
        memcpy(dst, src, rowBytes);
        src -= rowBytes;
        dst += rowBytes;
    }
    
    buffer->lpVtbl->Unlock(buffer);
    buffer->lpVtbl->SetCurrentLength(buffer, bufferSize);
    
    // Create sample
    IMFSample* sample = NULL;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        buffer->lpVtbl->Release(buffer);
        return FALSE;
    }
    
    hr = sample->lpVtbl->AddBuffer(sample, buffer);
    buffer->lpVtbl->Release(buffer);
    if (FAILED(hr)) {
        sample->lpVtbl->Release(sample);
        return FALSE;
    }
    
    // Set sample time
    UINT64 sampleTime = state->frameCount * state->frameDuration;
    sample->lpVtbl->SetSampleTime(sample, sampleTime);
    sample->lpVtbl->SetSampleDuration(sample, state->frameDuration);
    
    // Write sample
    hr = state->sinkWriter->lpVtbl->WriteSample(state->sinkWriter, state->videoStreamIndex, sample);
    sample->lpVtbl->Release(sample);
    
    if (SUCCEEDED(hr)) {
        state->frameCount++;
        return TRUE;
    }
    
    return FALSE;
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

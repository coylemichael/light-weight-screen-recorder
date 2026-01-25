/*
 * MP4 Muxer Implementation
 * Writes HEVC (H.265) encoded samples to MP4 file using IMFSinkWriter passthrough
 *
 * ERROR HANDLING PATTERN:
 * - Goto-cleanup for functions with multiple resource allocations
 * - Uses CHECK_HR/CHECK_HR_LOG macros from mem_utils.h for HRESULT checks
 * - Continue-on-error for individual samples in loops (best effort)
 * - All MF errors are logged with HRESULT values
 * - Returns BOOL to propagate errors; callers must check
 * - "Always check creation, release in reverse order" (see mem_utils.h)
 */

#include "mp4_muxer.h"
#include "util.h"
#include "logger.h"
#include "constants.h"
#include "mem_utils.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>

/*
 * HEVC format GUID: {43564548-0000-0010-8000-00AA00389B71}
 * Thread Access: [ReadOnly - constant initialized at compile time]
 */
static const GUID MFVideoFormat_HEVC_Local = 
    {0x43564548, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* Alias for logging */
#define MuxLog Logger_Log

/* ============================================================================
 * MUXER HELPER FUNCTIONS
 * ============================================================================
 * These extract common operations to reduce duplication and improve readability.
 */

/**
 * Create an MF video media type configured for HEVC passthrough.
 * Caller must Release() the returned type.
 */
static IMFMediaType* CreateHEVCMediaType(const MuxerConfig* config) {
    IMFMediaType* videoType = NULL;
    HRESULT hr = MFCreateMediaType(&videoType);
    if (FAILED(hr)) return NULL;
    
    UINT32 bitrate = Util_CalculateBitrate(config->width, config->height, 
                                           config->fps, config->quality);
    
    videoType->lpVtbl->SetGUID(videoType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    videoType->lpVtbl->SetGUID(videoType, &MF_MT_SUBTYPE, &MFVideoFormat_HEVC_Local);
    videoType->lpVtbl->SetUINT32(videoType, &MF_MT_AVG_BITRATE, bitrate);
    videoType->lpVtbl->SetUINT32(videoType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    
    UINT64 frameSize = ((UINT64)config->width << 32) | config->height;
    videoType->lpVtbl->SetUINT64(videoType, &MF_MT_FRAME_SIZE, frameSize);
    
    UINT64 frameRate = ((UINT64)config->fps << 32) | 1;
    videoType->lpVtbl->SetUINT64(videoType, &MF_MT_FRAME_RATE, frameRate);
    
    UINT64 pixelAspect = ((UINT64)1 << 32) | 1;
    videoType->lpVtbl->SetUINT64(videoType, &MF_MT_PIXEL_ASPECT_RATIO, pixelAspect);
    
    /* Set HEVC sequence header (VPS/SPS/PPS) if provided */
    if (config->seqHeader && config->seqHeaderSize > 0) {
        hr = videoType->lpVtbl->SetBlob(videoType, &MF_MT_MPEG_SEQUENCE_HEADER, 
                                        config->seqHeader, config->seqHeaderSize);
        if (SUCCEEDED(hr)) {
            MuxLog("MP4Muxer: Set video sequence header (%u bytes)\n", config->seqHeaderSize);
        }
    }
    
    return videoType;
}

/**
 * Create an MF audio media type configured for AAC.
 * Caller must Release() the returned type.
 */
static IMFMediaType* CreateAACMediaType(const MuxerAudioConfig* config) {
    IMFMediaType* audioType = NULL;
    HRESULT hr = MFCreateMediaType(&audioType);
    if (FAILED(hr)) return NULL;
    
    audioType->lpVtbl->SetGUID(audioType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    audioType->lpVtbl->SetGUID(audioType, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, config->sampleRate);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_NUM_CHANNELS, config->channels);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config->bitrate / 8);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AAC_PAYLOAD_TYPE, 0);  /* Raw AAC */
    
    /* Set AudioSpecificConfig if available */
    if (config->configData && config->configSize > 0) {
        audioType->lpVtbl->SetBlob(audioType, &MF_MT_USER_DATA, 
            config->configData, config->configSize);
    }
    
    return audioType;
}

/**
 * Write a single video sample to the sink writer.
 * Returns TRUE on success, FALSE on failure (non-fatal, caller continues).
 */
static BOOL WriteVideoSampleToWriter(IMFSinkWriter* writer, DWORD streamIndex,
                                      const MuxerSample* sample) {
    if (!sample->data || sample->size == 0) return FALSE;
    
    IMFMediaBuffer* mfBuffer = NULL;
    HRESULT hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
    if (FAILED(hr)) return FALSE;
    
    BYTE* bufData = NULL;
    hr = mfBuffer->lpVtbl->Lock(mfBuffer, &bufData, NULL, NULL);
    if (FAILED(hr)) {
        mfBuffer->lpVtbl->Release(mfBuffer);
        return FALSE;
    }
    
    memcpy(bufData, sample->data, sample->size);
    mfBuffer->lpVtbl->Unlock(mfBuffer);
    mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
    
    IMFSample* mfSample = NULL;
    hr = MFCreateSample(&mfSample);
    if (FAILED(hr)) {
        mfBuffer->lpVtbl->Release(mfBuffer);
        return FALSE;
    }
    
    mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
    mfSample->lpVtbl->SetSampleTime(mfSample, sample->timestamp);
    mfSample->lpVtbl->SetSampleDuration(mfSample, sample->duration);
    
    if (sample->isKeyframe) {
        mfSample->lpVtbl->SetUINT32(mfSample, &MFSampleExtension_CleanPoint, TRUE);
    }
    
    hr = writer->lpVtbl->WriteSample(writer, streamIndex, mfSample);
    mfSample->lpVtbl->Release(mfSample);
    mfBuffer->lpVtbl->Release(mfBuffer);
    
    return SUCCEEDED(hr);
}

/**
 * Write a single audio sample to the sink writer.
 * Returns TRUE on success, FALSE on failure (non-fatal, caller continues).
 */
static BOOL WriteAudioSampleToWriter(IMFSinkWriter* writer, DWORD streamIndex,
                                      const MuxerAudioSample* sample) {
    if (!sample->data || sample->size == 0) return FALSE;
    
    IMFMediaBuffer* mfBuffer = NULL;
    HRESULT hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
    if (FAILED(hr)) return FALSE;
    
    BYTE* bufData = NULL;
    hr = mfBuffer->lpVtbl->Lock(mfBuffer, &bufData, NULL, NULL);
    if (FAILED(hr)) {
        mfBuffer->lpVtbl->Release(mfBuffer);
        return FALSE;
    }
    
    memcpy(bufData, sample->data, sample->size);
    mfBuffer->lpVtbl->Unlock(mfBuffer);
    mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
    
    IMFSample* mfSample = NULL;
    hr = MFCreateSample(&mfSample);
    if (FAILED(hr)) {
        mfBuffer->lpVtbl->Release(mfBuffer);
        return FALSE;
    }
    
    mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
    mfSample->lpVtbl->SetSampleTime(mfSample, sample->timestamp);
    mfSample->lpVtbl->SetSampleDuration(mfSample, sample->duration);
    
    hr = writer->lpVtbl->WriteSample(writer, streamIndex, mfSample);
    mfSample->lpVtbl->Release(mfSample);
    mfBuffer->lpVtbl->Release(mfBuffer);
    
    return SUCCEEDED(hr);
}

BOOL MP4Muxer_WriteFile(
    const char* outputPath,
    const MuxerSample* samples,
    int sampleCount,
    const MuxerConfig* config)
{
    // Preconditions
    LWSR_ASSERT(outputPath != NULL);
    LWSR_ASSERT(samples != NULL);
    LWSR_ASSERT(sampleCount > 0);
    LWSR_ASSERT(config != NULL);
    LWSR_ASSERT(config->width > 0);
    LWSR_ASSERT(config->height > 0);
    LWSR_ASSERT(config->fps > 0);
    
    BOOL result = FALSE;
    IMFSinkWriter* writer = NULL;
    IMFAttributes* attrs = NULL;
    IMFMediaType* outputType = NULL;
    DWORD streamIndex = 0;
    BOOL beginWritingCalled = FALSE;
    
    if (!outputPath || !samples || sampleCount <= 0 || !config) {
        MuxLog("MP4Muxer: Invalid parameters\n");
        return FALSE;
    }
    
    MuxLog("MP4Muxer: Writing %d samples to %s\n", sampleCount, outputPath);
    
    // Convert path to wide string
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create SinkWriter with hardware acceleration enabled
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    CHECK_HR_LOG(hr, cleanup, "MP4Muxer: MFCreateAttributes");
    
    attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->lpVtbl->SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
    
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attrs, &writer);
    CHECK_HR_LOG(hr, cleanup, "MP4Muxer: MFCreateSinkWriterFromURL");
    
    // Create HEVC media type using helper
    outputType = CreateHEVCMediaType(config);
    if (!outputType) goto cleanup;
    
    MuxLog("MP4Muxer: %dx%d @ %d fps\n", config->width, config->height, config->fps);
    
    // Add stream
    hr = writer->lpVtbl->AddStream(writer, outputType, &streamIndex);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: AddStream failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // For HEVC passthrough, use the SAME type for input as output
    // This triggers passthrough mode - no transcoding
    hr = writer->lpVtbl->SetInputMediaType(writer, streamIndex, outputType, NULL);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: SetInputMediaType failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // Begin writing
    hr = writer->lpVtbl->BeginWriting(writer);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: BeginWriting failed 0x%08X\n", hr);
        goto cleanup;
    }
    beginWritingCalled = TRUE;
    
    // Write all samples using helper function
    int samplesWritten = 0;
    int keyframeCount = 0;
    
    // VERIFY: First sample must be a keyframe for valid HEVC decoding
    if (sampleCount > 0 && !samples[0].isKeyframe) {
        MuxLog("MP4Muxer: WARNING - First sample is NOT a keyframe! POC errors likely.\n");
    } else if (sampleCount > 0) {
        MuxLog("MP4Muxer: First sample is keyframe (good)\n");
    }
    
    for (int i = 0; i < sampleCount; i++) {
        const MuxerSample* sample = &samples[i];
        
        if (WriteVideoSampleToWriter(writer, streamIndex, sample)) {
            samplesWritten++;
            if (sample->isKeyframe) keyframeCount++;
        }
    }
    
    // Log final stats - use last sample's actual timestamp for accurate duration
    LONGLONG finalDuration = (sampleCount > 0) ? samples[sampleCount-1].timestamp + samples[sampleCount-1].duration : 0;
    MuxLog("MP4Muxer: Wrote %d/%d samples (%.3fs real-time), keyframes: %d\n", 
           samplesWritten, sampleCount, (double)finalDuration / (double)MF_UNITS_PER_SECOND, keyframeCount);
    
    // Finalize - can be slow on cloud/network drives
    if (beginWritingCalled) {
        DWORD finalizeStart = GetTickCount();
        hr = writer->lpVtbl->Finalize(writer);
        DWORD finalizeTime = GetTickCount() - finalizeStart;
        
        if (FAILED(hr)) {
            MuxLog("MP4Muxer: Finalize failed with HRESULT 0x%08X after %u ms\n", hr, finalizeTime);
        } else if (finalizeTime > 2000) {
            MuxLog("MP4Muxer: Finalize took %u ms (slow disk/network?)\n", finalizeTime);
        }
    }
    
    result = SUCCEEDED(hr) && samplesWritten > 0;
    MuxLog("MP4Muxer: Finalize %s\n", result ? "OK" : "FAILED");
    
cleanup:
    SAFE_RELEASE(outputType);
    SAFE_RELEASE(attrs);
    SAFE_RELEASE(writer);
    
    return result;
}

// Write video and audio to MP4 file
BOOL MP4Muxer_WriteFileWithAudio(
    const char* outputPath,
    const MuxerSample* videoSamples,
    int videoSampleCount,
    const MuxerConfig* videoConfig,
    const MuxerAudioSample* audioSamples,
    int audioSampleCount,
    const MuxerAudioConfig* audioConfig)
{
    // Preconditions (video is required, audio is optional)
    LWSR_ASSERT(outputPath != NULL);
    LWSR_ASSERT(videoSamples != NULL);
    LWSR_ASSERT(videoSampleCount > 0);
    LWSR_ASSERT(videoConfig != NULL);
    
    BOOL result = FALSE;
    IMFSinkWriter* writer = NULL;
    IMFAttributes* attrs = NULL;
    IMFMediaType* videoType = NULL;
    IMFMediaType* audioType = NULL;
    BOOL beginWritingCalled = FALSE;
    DWORD videoStreamIndex = 0;
    DWORD audioStreamIndex = 0;
    
    if (!outputPath || !videoSamples || videoSampleCount <= 0 || !videoConfig) {
        MuxLog("MP4Muxer: Invalid video parameters\n");
        return FALSE;
    }
    
    // If no audio, fall back to video-only
    if (!audioSamples || audioSampleCount <= 0 || !audioConfig) {
        return MP4Muxer_WriteFile(outputPath, videoSamples, videoSampleCount, videoConfig);
    }
    
    MuxLog("MP4Muxer: Writing %d video + %d audio samples to %s\n", 
           videoSampleCount, audioSampleCount, outputPath);
    
    // Convert path to wide string
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create SinkWriter
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    CHECK_HR_LOG(hr, cleanup, "MP4Muxer: MFCreateAttributes");
    
    attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->lpVtbl->SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
    
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attrs, &writer);
    CHECK_HR_LOG(hr, cleanup, "MP4Muxer: MFCreateSinkWriterFromURL");
    
    // === VIDEO STREAM (using helper) ===
    videoType = CreateHEVCMediaType(videoConfig);
    if (!videoType) goto cleanup;
    
    hr = writer->lpVtbl->AddStream(writer, videoType, &videoStreamIndex);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: AddStream (video) failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    hr = writer->lpVtbl->SetInputMediaType(writer, videoStreamIndex, videoType, NULL);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: SetInputMediaType (video) failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // === AUDIO STREAM (using helper) ===
    audioType = CreateAACMediaType(audioConfig);
    if (!audioType) goto cleanup;
    
    hr = writer->lpVtbl->AddStream(writer, audioType, &audioStreamIndex);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: AddStream (audio) failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    hr = writer->lpVtbl->SetInputMediaType(writer, audioStreamIndex, audioType, NULL);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: SetInputMediaType (audio) failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // Begin writing
    hr = writer->lpVtbl->BeginWriting(writer);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: BeginWriting failed 0x%08X\n", hr);
        goto cleanup;
    }
    beginWritingCalled = TRUE;
    
    // === INTERLEAVED WRITING ===
    // Write samples in timestamp order for proper interleaving
    int videoIdx = 0;
    int audioIdx = 0;
    int videoWritten = 0;
    int audioWritten = 0;
    int totalSamples = videoSampleCount + audioSampleCount;
    int samplesProcessed = 0;
    int lastProgressPercent = -1;
    
    while (videoIdx < videoSampleCount || audioIdx < audioSampleCount) {
        // Log progress every 10%
        samplesProcessed++;
        int progressPercent = (samplesProcessed * 100) / totalSamples;
        if (progressPercent >= lastProgressPercent + 10) {
            lastProgressPercent = progressPercent;
            MuxLog("MP4Muxer: Progress %d%% (%d/%d samples)\n", 
                   progressPercent, samplesProcessed, totalSamples);
        }
        
        BOOL writeVideo = FALSE;
        
        if (videoIdx >= videoSampleCount) {
            writeVideo = FALSE;
        } else if (audioIdx >= audioSampleCount) {
            writeVideo = TRUE;
        } else {
            // Compare timestamps - write whichever is earlier
            writeVideo = (videoSamples[videoIdx].timestamp <= audioSamples[audioIdx].timestamp);
        }
        
        if (writeVideo && videoIdx < videoSampleCount) {
            const MuxerSample* sample = &videoSamples[videoIdx++];
            if (!sample->data || sample->size == 0) continue;
            
            if (WriteVideoSampleToWriter(writer, videoStreamIndex, sample)) {
                videoWritten++;
            }
        } else if (audioIdx < audioSampleCount) {
            const MuxerAudioSample* sample = &audioSamples[audioIdx++];
            if (!sample->data || sample->size == 0) continue;
            
            if (WriteAudioSampleToWriter(writer, audioStreamIndex, sample)) {
                audioWritten++;
            }
        }
    }
    
    MuxLog("MP4Muxer: Wrote %d/%d video, %d/%d audio samples\n",
           videoWritten, videoSampleCount, audioWritten, audioSampleCount);
    
    // Finalize - this can be VERY slow on cloud/network drives
    // because it rewrites the MP4 header (moov atom) with final timing info
    if (beginWritingCalled) {
        DWORD finalizeStart = GetTickCount();
        hr = writer->lpVtbl->Finalize(writer);
        DWORD finalizeTime = GetTickCount() - finalizeStart;
        
        if (FAILED(hr)) {
            MuxLog("MP4Muxer: Finalize failed with HRESULT 0x%08X after %u ms\n", hr, finalizeTime);
        } else if (finalizeTime > 2000) {
            MuxLog("MP4Muxer: Finalize took %u ms (slow disk/network?)\n", finalizeTime);
        }
    }
    
    result = SUCCEEDED(hr) && videoWritten > 0;
    MuxLog("MP4Muxer: Finalize %s\n", result ? "OK" : "FAILED");
    
cleanup:
    SAFE_RELEASE(audioType);
    SAFE_RELEASE(videoType);
    SAFE_RELEASE(attrs);
    SAFE_RELEASE(writer);
    
    return result;
}

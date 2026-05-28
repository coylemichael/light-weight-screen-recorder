/*
 * mp4_muxer.c - Media Foundation IMFSinkWriter - passthrough mux to MP4
 * 
 * SHARED BY: replay_buffer.c (batch API), recording.c (streaming API)
 * 
 * Writes HEVC-encoded samples to MP4 using IMFSinkWriter passthrough.
 * Two modes:
 *   - Batch: MP4Muxer_WriteFile() - write all samples at once (replay saves)
 *   - Streaming: StreamingMuxer_*() - write frames as they arrive (recording)
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
#include <limits.h>

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
    
    hr = videoType->lpVtbl->SetGUID(videoType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (FAILED(hr)) goto fail;
    hr = videoType->lpVtbl->SetGUID(videoType, &MF_MT_SUBTYPE, &MFVideoFormat_HEVC_Local);
    if (FAILED(hr)) goto fail;
    hr = videoType->lpVtbl->SetUINT32(videoType, &MF_MT_AVG_BITRATE, bitrate);
    if (FAILED(hr)) goto fail;
    hr = videoType->lpVtbl->SetUINT32(videoType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) goto fail;
    
    UINT64 frameSize = ((UINT64)config->width << 32) | config->height;
    hr = videoType->lpVtbl->SetUINT64(videoType, &MF_MT_FRAME_SIZE, frameSize);
    if (FAILED(hr)) goto fail;
    
    UINT64 frameRate = ((UINT64)config->fps << 32) | 1;
    hr = videoType->lpVtbl->SetUINT64(videoType, &MF_MT_FRAME_RATE, frameRate);
    if (FAILED(hr)) goto fail;
    
    UINT64 pixelAspect = ((UINT64)1 << 32) | 1;
    hr = videoType->lpVtbl->SetUINT64(videoType, &MF_MT_PIXEL_ASPECT_RATIO, pixelAspect);
    if (FAILED(hr)) goto fail;
    
    /* Set HEVC sequence header (VPS/SPS/PPS) if provided */
    if (config->seqHeader && config->seqHeaderSize > 0) {
        hr = videoType->lpVtbl->SetBlob(videoType, &MF_MT_MPEG_SEQUENCE_HEADER, 
                                        config->seqHeader, config->seqHeaderSize);
        if (FAILED(hr)) {
            MuxLog("MP4Muxer: SetBlob(sequence header) failed 0x%08X\n", hr);
            goto fail;
        }
        MuxLog("MP4Muxer: Set video sequence header (%u bytes)\n", config->seqHeaderSize);
    }
    
    return videoType;

fail:
    MuxLog("MP4Muxer: CreateHEVCMediaType failed 0x%08X\n", hr);
    SAFE_RELEASE(videoType);
    return NULL;
}

/**
 * Create an MF audio media type configured for AAC.
 * Caller must Release() the returned type.
 */
static IMFMediaType* CreateAACMediaType(const MuxerAudioConfig* config) {
    IMFMediaType* audioType = NULL;
    HRESULT hr = MFCreateMediaType(&audioType);
    if (FAILED(hr)) return NULL;
    
    hr = audioType->lpVtbl->SetGUID(audioType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (FAILED(hr)) goto fail;
    hr = audioType->lpVtbl->SetGUID(audioType, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    if (FAILED(hr)) goto fail;
    hr = audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, config->sampleRate);
    if (FAILED(hr)) goto fail;
    hr = audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_NUM_CHANNELS, config->channels);
    if (FAILED(hr)) goto fail;
    hr = audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(hr)) goto fail;
    hr = audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config->bitrate / 8);
    if (FAILED(hr)) goto fail;
    hr = audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AAC_PAYLOAD_TYPE, 0);  /* Raw AAC */
    if (FAILED(hr)) goto fail;
    
    /* Set AudioSpecificConfig if available */
    if (config->configData && config->configSize > 0) {
        hr = audioType->lpVtbl->SetBlob(audioType, &MF_MT_USER_DATA, 
            config->configData, config->configSize);
        if (FAILED(hr)) {
            MuxLog("MP4Muxer: SetBlob(AudioSpecificConfig) failed 0x%08X\n", hr);
            goto fail;
        }
    }
    
    return audioType;

fail:
    MuxLog("MP4Muxer: CreateAACMediaType failed 0x%08X\n", hr);
    SAFE_RELEASE(audioType);
    return NULL;
}

/**
 * Write a single video sample to the sink writer.
 * Returns TRUE on success, FALSE on failure (non-fatal, caller continues).
 */
static BOOL WriteVideoSampleToWriter(IMFSinkWriter* writer, DWORD streamIndex,
                                      const MuxerSample* sample) {
    if (!sample->data || sample->size == 0) return FALSE;
    
    BOOL result = FALSE;
    IMFMediaBuffer* mfBuffer = NULL;
    IMFSample* mfSample = NULL;
    BOOL locked = FALSE;
    BYTE* bufData = NULL;
    HRESULT hr;
    
    hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
    if (FAILED(hr)) goto cleanup;
    
    MF_LOCK_BUFFER(mfBuffer, &bufData, NULL, NULL, hr, cleanup, locked);
    memcpy(bufData, sample->data, sample->size);
    MF_UNLOCK_BUFFER(mfBuffer, locked);
    
    hr = mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
    if (FAILED(hr)) goto cleanup;
    
    hr = MFCreateSample(&mfSample);
    if (FAILED(hr)) goto cleanup;
    
    hr = mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
    if (FAILED(hr)) goto cleanup;
    hr = mfSample->lpVtbl->SetSampleTime(mfSample, sample->timestamp);
    if (FAILED(hr)) goto cleanup;
    hr = mfSample->lpVtbl->SetSampleDuration(mfSample, sample->duration);
    if (FAILED(hr)) goto cleanup;
    
    if (sample->isKeyframe) {
        hr = mfSample->lpVtbl->SetUINT32(mfSample, &MFSampleExtension_CleanPoint, TRUE);
        if (FAILED(hr)) goto cleanup;
    }
    
    hr = writer->lpVtbl->WriteSample(writer, streamIndex, mfSample);
    result = SUCCEEDED(hr);
    
cleanup:
    MF_UNLOCK_BUFFER(mfBuffer, locked);
    SAFE_RELEASE(mfSample);
    SAFE_RELEASE(mfBuffer);
    return result;
}

/**
 * Write a single audio sample to the sink writer.
 * Returns TRUE on success, FALSE on failure (non-fatal, caller continues).
 */
static BOOL WriteAudioSampleToWriter(IMFSinkWriter* writer, DWORD streamIndex,
                                      const MuxerAudioSample* sample) {
    if (!sample->data || sample->size == 0) return FALSE;
    
    BOOL result = FALSE;
    IMFMediaBuffer* mfBuffer = NULL;
    IMFSample* mfSample = NULL;
    BOOL locked = FALSE;
    BYTE* bufData = NULL;
    HRESULT hr;
    
    hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
    if (FAILED(hr)) goto cleanup;
    
    MF_LOCK_BUFFER(mfBuffer, &bufData, NULL, NULL, hr, cleanup, locked);
    memcpy(bufData, sample->data, sample->size);
    MF_UNLOCK_BUFFER(mfBuffer, locked);
    
    hr = mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
    if (FAILED(hr)) goto cleanup;
    
    hr = MFCreateSample(&mfSample);
    if (FAILED(hr)) goto cleanup;
    
    hr = mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
    if (FAILED(hr)) goto cleanup;
    hr = mfSample->lpVtbl->SetSampleTime(mfSample, sample->timestamp);
    if (FAILED(hr)) goto cleanup;
    hr = mfSample->lpVtbl->SetSampleDuration(mfSample, sample->duration);
    if (FAILED(hr)) goto cleanup;
    
    hr = writer->lpVtbl->WriteSample(writer, streamIndex, mfSample);
    result = SUCCEEDED(hr);
    
cleanup:
    MF_UNLOCK_BUFFER(mfBuffer, locked);
    SAFE_RELEASE(mfSample);
    SAFE_RELEASE(mfBuffer);
    return result;
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
    
    // Convert path to wide string (UTF-8 -> UTF-16) so non-ANSI paths survive
    WCHAR wPath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, wPath, MAX_PATH) == 0) {
        MuxLog("MP4Muxer: MultiByteToWideChar failed (path too long or invalid UTF-8)\n");
        return FALSE;
    }
    
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
    {
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
    
    // Convert path to wide string (UTF-8 -> UTF-16) so non-ANSI paths survive
    WCHAR wPath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, wPath, MAX_PATH) == 0) {
        MuxLog("MP4Muxer: MultiByteToWideChar failed (path too long or invalid UTF-8)\n");
        return FALSE;
    }
    
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
    {
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

/* ============================================================================
 * MULTI-TRACK AUDIO BATCH WRITE
 * ============================================================================
 * Write video + N audio tracks to MP4. Track 0 = mixed, 1..N = individual.
 */

BOOL MP4Muxer_WriteFileWithMultiAudio(
    const char* outputPath,
    const MuxerSample* videoSamples,
    int videoSampleCount,
    const MuxerConfig* videoConfig,
    const MuxerAudioTrack* audioTracks,
    int audioTrackCount)
{
    LWSR_ASSERT(outputPath != NULL);
    LWSR_ASSERT(videoSamples != NULL);
    LWSR_ASSERT(videoSampleCount > 0);
    LWSR_ASSERT(videoConfig != NULL);

    if (!outputPath || !videoSamples || videoSampleCount <= 0 || !videoConfig)
        return FALSE;

    /* Fall back if no audio tracks */
    if (!audioTracks || audioTrackCount <= 0)
        return MP4Muxer_WriteFile(outputPath, videoSamples, videoSampleCount, videoConfig);

    /* Fall back to single-track path if only 1 track */
    if (audioTrackCount == 1 && audioTracks[0].samples && audioTracks[0].sampleCount > 0)
        return MP4Muxer_WriteFileWithAudio(outputPath, videoSamples, videoSampleCount, videoConfig,
                                           audioTracks[0].samples, audioTracks[0].sampleCount,
                                           &audioTracks[0].config);

    BOOL result = FALSE;
    IMFSinkWriter* writer = NULL;
    IMFAttributes* attrs = NULL;
    IMFMediaType* videoType = NULL;
    IMFMediaType* audioTypes[MAX_AUDIO_TRACKS] = {0};
    DWORD videoStreamIndex = 0;
    DWORD audioStreamIndices[MAX_AUDIO_TRACKS] = {0};
    int actualTrackCount = 0;
    BOOL beginWritingCalled = FALSE;

    if (audioTrackCount > MAX_AUDIO_TRACKS) audioTrackCount = MAX_AUDIO_TRACKS;

    MuxLog("MP4Muxer: Writing %d video samples + %d audio tracks to %s\n",
           videoSampleCount, audioTrackCount, outputPath);

    WCHAR wPath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, wPath, MAX_PATH) == 0) {
        MuxLog("MP4Muxer: MultiByteToWideChar failed (path too long or invalid UTF-8)\n");
        return FALSE;
    }

    HRESULT hr = MFCreateAttributes(&attrs, 2);
    CHECK_HR_LOG(hr, cleanup, "MP4Muxer: MFCreateAttributes");

    attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->lpVtbl->SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);

    hr = MFCreateSinkWriterFromURL(wPath, NULL, attrs, &writer);
    CHECK_HR_LOG(hr, cleanup, "MP4Muxer: MFCreateSinkWriterFromURL");

    /* === VIDEO STREAM === */
    videoType = CreateHEVCMediaType(videoConfig);
    if (!videoType) goto cleanup;

    hr = writer->lpVtbl->AddStream(writer, videoType, &videoStreamIndex);
    if (FAILED(hr)) { MuxLog("MP4Muxer: AddStream (video) failed 0x%08X\n", hr); goto cleanup; }

    hr = writer->lpVtbl->SetInputMediaType(writer, videoStreamIndex, videoType, NULL);
    if (FAILED(hr)) { MuxLog("MP4Muxer: SetInputMediaType (video) failed 0x%08X\n", hr); goto cleanup; }

    /* === AUDIO STREAMS === */
    for (int t = 0; t < audioTrackCount; t++) {
        if (!audioTracks[t].samples || audioTracks[t].sampleCount <= 0) continue;

        audioTypes[actualTrackCount] = CreateAACMediaType(&audioTracks[t].config);
        if (!audioTypes[actualTrackCount]) continue;

        hr = writer->lpVtbl->AddStream(writer, audioTypes[actualTrackCount], &audioStreamIndices[actualTrackCount]);
        if (FAILED(hr)) { SAFE_RELEASE(audioTypes[actualTrackCount]); continue; }

        hr = writer->lpVtbl->SetInputMediaType(writer, audioStreamIndices[actualTrackCount],
                                                audioTypes[actualTrackCount], NULL);
        if (FAILED(hr)) continue;

        MuxLog("MP4Muxer: Audio track %d: %d samples, stream index %u\n",
               t, audioTracks[t].sampleCount, audioStreamIndices[actualTrackCount]);
        actualTrackCount++;
    }

    if (actualTrackCount == 0) {
        MuxLog("MP4Muxer: No valid audio tracks, falling back to video-only\n");
        SAFE_RELEASE(videoType);
        SAFE_RELEASE(attrs);
        SAFE_RELEASE(writer);
        return MP4Muxer_WriteFile(outputPath, videoSamples, videoSampleCount, videoConfig);
    }

    /* Begin writing */
    hr = writer->lpVtbl->BeginWriting(writer);
    if (FAILED(hr)) { MuxLog("MP4Muxer: BeginWriting failed 0x%08X\n", hr); goto cleanup; }
    beginWritingCalled = TRUE;

    /* === INTERLEAVED WRITING ===
     * Write all streams interleaved by timestamp for proper MP4 structure.
     * Use cursors for video + each audio track. */
    {
        int videoIdx = 0;
        int audioIdx[MAX_AUDIO_TRACKS] = {0};
        int videoWritten = 0;
        int audioWritten[MAX_AUDIO_TRACKS] = {0};
        int totalSamples = videoSampleCount;
        for (int t = 0; t < audioTrackCount; t++) totalSamples += audioTracks[t].sampleCount;
        int samplesProcessed = 0;
        int lastProgressPercent = -1;
        int trackForIdx[MAX_AUDIO_TRACKS];  /* maps actualTrack -> original track index */
        {
            int at = 0;
            for (int t = 0; t < audioTrackCount; t++) {
                if (audioTracks[t].samples && audioTracks[t].sampleCount > 0 && at < actualTrackCount) {
                    trackForIdx[at] = t;
                    at++;
                }
            }
        }

        for (;;) {
            /* Find stream with earliest next timestamp */
            LONGLONG earliest = LLONG_MAX;
            int earliestStream = -1; /* -1=video, 0..N-1=audio track */

            if (videoIdx < videoSampleCount) {
                earliest = videoSamples[videoIdx].timestamp;
                earliestStream = -1;
            }

            for (int at = 0; at < actualTrackCount; at++) {
                int origTrack = trackForIdx[at];
                if (audioIdx[at] < audioTracks[origTrack].sampleCount) {
                    LONGLONG ts = audioTracks[origTrack].samples[audioIdx[at]].timestamp;
                    if (ts < earliest) {
                        earliest = ts;
                        earliestStream = at;
                    }
                }
            }

            if (earliest == LLONG_MAX) break; /* All streams exhausted */

            samplesProcessed++;
            int progressPercent = (samplesProcessed * 100) / totalSamples;
            if (progressPercent >= lastProgressPercent + 10) {
                lastProgressPercent = progressPercent;
                MuxLog("MP4Muxer: Progress %d%% (%d/%d samples)\n",
                       progressPercent, samplesProcessed, totalSamples);
            }

            if (earliestStream == -1) {
                /* Write video */
                const MuxerSample* sample = &videoSamples[videoIdx++];
                if (sample->data && sample->size > 0) {
                    if (WriteVideoSampleToWriter(writer, videoStreamIndex, sample))
                        videoWritten++;
                }
            } else {
                /* Write audio for track */
                int at = earliestStream;
                int origTrack = trackForIdx[at];
                const MuxerAudioSample* sample = &audioTracks[origTrack].samples[audioIdx[at]++];
                if (sample->data && sample->size > 0) {
                    if (WriteAudioSampleToWriter(writer, audioStreamIndices[at], sample))
                        audioWritten[at]++;
                }
            }
        }

        MuxLog("MP4Muxer: Wrote %d/%d video", videoWritten, videoSampleCount);
        for (int at = 0; at < actualTrackCount; at++) {
            int origTrack = trackForIdx[at];
            MuxLog(", track%d=%d/%d", origTrack, audioWritten[at], audioTracks[origTrack].sampleCount);
        }
        MuxLog("\n");
    }

    /* Finalize */
    {
        DWORD finalizeStart = GetTickCount();
        hr = writer->lpVtbl->Finalize(writer);
        DWORD finalizeTime = GetTickCount() - finalizeStart;
        if (FAILED(hr))
            MuxLog("MP4Muxer: Finalize failed 0x%08X after %u ms\n", hr, finalizeTime);
        else if (finalizeTime > 2000)
            MuxLog("MP4Muxer: Finalize took %u ms (slow disk?)\n", finalizeTime);
    }

    result = SUCCEEDED(hr);
    MuxLog("MP4Muxer: Finalize %s\n", result ? "OK" : "FAILED");

cleanup:
    for (int t = 0; t < MAX_AUDIO_TRACKS; t++) SAFE_RELEASE(audioTypes[t]);
    SAFE_RELEASE(videoType);
    SAFE_RELEASE(attrs);
    SAFE_RELEASE(writer);

    return result;
}

/* ============================================================================
 * STREAMING MUXER IMPLEMENTATION
 * ============================================================================
 * Real-time muxing for direct recording (write frames as they arrive).
 */

struct StreamingMuxer {
    IMFSinkWriter* writer;
    DWORD videoStreamIndex;
    DWORD audioStreamIndex;
    BOOL hasAudio;
    BOOL beginWritingCalled;
    CRITICAL_SECTION lock;          // Thread-safety for concurrent video/audio writes
    
    // Stats
    int videoSamplesWritten;
    int audioSamplesWritten;
    int keyframeCount;
    LONGLONG lastVideoTimestamp;
    LONGLONG lastAudioTimestamp;
};

StreamingMuxer* StreamingMuxer_Create(
    const char* outputPath,
    const MuxerConfig* videoConfig)
{
    return StreamingMuxer_CreateWithAudio(outputPath, videoConfig, NULL);
}

StreamingMuxer* StreamingMuxer_CreateWithAudio(
    const char* outputPath,
    const MuxerConfig* videoConfig,
    const MuxerAudioConfig* audioConfig)
{
    LWSR_ASSERT(outputPath != NULL);
    LWSR_ASSERT(videoConfig != NULL);
    
    if (!outputPath || !videoConfig) return NULL;
    
    StreamingMuxer* muxer = NULL;
    IMFAttributes* attrs = NULL;
    IMFMediaType* videoType = NULL;
    IMFMediaType* audioType = NULL;
    BOOL success = FALSE;
    
    // Allocate muxer struct
    muxer = (StreamingMuxer*)calloc(1, sizeof(StreamingMuxer));
    if (!muxer) {
        MuxLog("StreamingMuxer: Failed to allocate muxer struct\n");
        return NULL;
    }
    
    InitializeCriticalSection(&muxer->lock);
    
    MuxLog("StreamingMuxer: Creating %s (%dx%d @ %d fps)\n", 
           outputPath, videoConfig->width, videoConfig->height, videoConfig->fps);
    
    // Convert path to wide string (UTF-8 -> UTF-16) so non-ANSI paths survive
    WCHAR wPath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, outputPath, -1, wPath, MAX_PATH) == 0) {
        MuxLog("StreamingMuxer: MultiByteToWideChar failed (path too long or invalid UTF-8)\n");
        goto cleanup;
    }
    
    // Create SinkWriter with hardware acceleration
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) goto cleanup;
    
    attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->lpVtbl->SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
    
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attrs, &muxer->writer);
    if (FAILED(hr)) {
        MuxLog("StreamingMuxer: MFCreateSinkWriterFromURL failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // Create video media type (HEVC passthrough)
    videoType = CreateHEVCMediaType(videoConfig);
    if (!videoType) goto cleanup;
    
    // Add video stream
    hr = muxer->writer->lpVtbl->AddStream(muxer->writer, videoType, &muxer->videoStreamIndex);
    if (FAILED(hr)) {
        MuxLog("StreamingMuxer: AddStream (video) failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // HEVC passthrough: same type for input and output
    hr = muxer->writer->lpVtbl->SetInputMediaType(muxer->writer, muxer->videoStreamIndex, videoType, NULL);
    if (FAILED(hr)) {
        MuxLog("StreamingMuxer: SetInputMediaType (video) failed 0x%08X\n", hr);
        goto cleanup;
    }
    
    // Add audio stream if config provided
    if (audioConfig) {
        audioType = CreateAACMediaType(audioConfig);
        if (audioType) {
            hr = muxer->writer->lpVtbl->AddStream(muxer->writer, audioType, &muxer->audioStreamIndex);
            if (SUCCEEDED(hr)) {
                hr = muxer->writer->lpVtbl->SetInputMediaType(muxer->writer, muxer->audioStreamIndex, audioType, NULL);
                if (SUCCEEDED(hr)) {
                    muxer->hasAudio = TRUE;
                    MuxLog("StreamingMuxer: Audio stream added (sr=%d, ch=%d)\n", 
                           audioConfig->sampleRate, audioConfig->channels);
                }
            }
            if (!muxer->hasAudio) {
                MuxLog("StreamingMuxer: WARNING - Audio stream setup failed, continuing without audio\n");
            }
        }
    }
    
    // Begin writing
    hr = muxer->writer->lpVtbl->BeginWriting(muxer->writer);
    if (FAILED(hr)) {
        MuxLog("StreamingMuxer: BeginWriting failed 0x%08X\n", hr);
        goto cleanup;
    }
    muxer->beginWritingCalled = TRUE;
    
    MuxLog("StreamingMuxer: Ready for streaming writes\n");
    success = TRUE;
    
cleanup:
    SAFE_RELEASE(audioType);
    SAFE_RELEASE(videoType);
    SAFE_RELEASE(attrs);
    
    if (!success) {
        if (muxer) {
            SAFE_RELEASE(muxer->writer);
            DeleteCriticalSection(&muxer->lock);
            free(muxer);
        }
        return NULL;
    }
    
    return muxer;
}

BOOL StreamingMuxer_WriteVideo(StreamingMuxer* muxer, const MuxerSample* sample) {
    if (!muxer || !muxer->writer || !sample) return FALSE;
    if (!sample->data || sample->size == 0) return FALSE;
    
    BOOL result = FALSE;
    
    EnterCriticalSection(&muxer->lock);
    
    if (WriteVideoSampleToWriter(muxer->writer, muxer->videoStreamIndex, sample)) {
        muxer->videoSamplesWritten++;
        muxer->lastVideoTimestamp = sample->timestamp;
        if (sample->isKeyframe) muxer->keyframeCount++;
        result = TRUE;
    }
    
    LeaveCriticalSection(&muxer->lock);
    
    return result;
}

BOOL StreamingMuxer_WriteAudio(StreamingMuxer* muxer, const MuxerAudioSample* sample) {
    if (!muxer || !muxer->writer || !muxer->hasAudio || !sample) return FALSE;
    if (!sample->data || sample->size == 0) return FALSE;
    
    BOOL result = FALSE;
    
    EnterCriticalSection(&muxer->lock);
    
    if (WriteAudioSampleToWriter(muxer->writer, muxer->audioStreamIndex, sample)) {
        muxer->audioSamplesWritten++;
        muxer->lastAudioTimestamp = sample->timestamp;
        result = TRUE;
    }
    
    LeaveCriticalSection(&muxer->lock);
    
    return result;
}

BOOL StreamingMuxer_Close(StreamingMuxer* muxer) {
    if (!muxer) return FALSE;
    
    BOOL result = FALSE;
    HRESULT hr = S_OK;
    
    EnterCriticalSection(&muxer->lock);
    
    MuxLog("StreamingMuxer: Closing (video=%d, audio=%d, keyframes=%d)\n",
           muxer->videoSamplesWritten, muxer->audioSamplesWritten, muxer->keyframeCount);
    
    if (muxer->writer && muxer->beginWritingCalled) {
        DWORD finalizeStart = GetTickCount();
        hr = muxer->writer->lpVtbl->Finalize(muxer->writer);
        DWORD finalizeTime = GetTickCount() - finalizeStart;
        
        if (FAILED(hr)) {
            MuxLog("StreamingMuxer: Finalize failed 0x%08X after %u ms\n", hr, finalizeTime);
        } else {
            if (finalizeTime > 2000) {
                MuxLog("StreamingMuxer: Finalize took %u ms (slow disk?)\n", finalizeTime);
            }
            result = (muxer->videoSamplesWritten > 0);
        }
    }
    
    SAFE_RELEASE(muxer->writer);
    
    LeaveCriticalSection(&muxer->lock);
    DeleteCriticalSection(&muxer->lock);
    
    MuxLog("StreamingMuxer: Close %s\n", result ? "OK" : "FAILED");
    free(muxer);
    
    return result;
}

void StreamingMuxer_Abort(StreamingMuxer* muxer) {
    if (!muxer) return;
    
    MuxLog("StreamingMuxer: Aborting (video=%d written before abort)\n", 
           muxer->videoSamplesWritten);
    
    EnterCriticalSection(&muxer->lock);
    
    // Release without finalize - file will be corrupted but we exit fast
    SAFE_RELEASE(muxer->writer);
    
    LeaveCriticalSection(&muxer->lock);
    DeleteCriticalSection(&muxer->lock);
    
    free(muxer);
}

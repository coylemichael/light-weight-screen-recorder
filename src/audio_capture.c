/*
 * Audio Capture Implementation
 * WASAPI-based audio capture with loopback support
 *
 * ERROR HANDLING PATTERN:
 * - Goto-cleanup for CreateSource() with multiple COM allocations
 * - HRESULT checks use FAILED()/SUCCEEDED() macros exclusively
 * - Device invalidation errors (AUDCLNT_E_*) trigger graceful shutdown
 * - All WASAPI errors are logged with HRESULT values
 * - Returns BOOL/NULL to propagate errors; callers must check
 */

#define COBJMACROS
#include <windows.h>

#include "audio_capture.h"
#include "audio_guids.h"
#include "util.h"
#include "logger.h"
#include "constants.h"
#include "mem_utils.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>

/* Individual audio source capture */
struct AudioCaptureSource {
    char deviceId[256];
    AudioDeviceType type;
    BOOL isLoopback;
    
    IMMDevice* device;
    IAudioClient* audioClient;
    IAudioCaptureClient* captureClient;
    
    WAVEFORMATEX* deviceFormat;
    WAVEFORMATEX targetFormat;
    
    /* Per-source buffer */
    BYTE* buffer;
    int bufferSize;
    int bufferWritePos;
    int bufferAvailable;
    CRITICAL_SECTION lock;
    
    HANDLE captureThread;  /* Thread handle for proper cleanup */
    volatile LONG active;  /* Thread-safe: use InterlockedExchange */
    
    /* Timing for event-driven sources */
    LARGE_INTEGER lastPacketTime;
    LARGE_INTEGER perfFreq;
    BOOL hasReceivedPacket;
    
    /* Device invalidation tracking */
    volatile LONG deviceInvalidated;
    DWORD consecutiveErrors;
    
    /* Recovery tracking */
    LARGE_INTEGER lastRecoveryAttempt;
};

/*
 * Global MMDevice enumerator for audio capture.
 * Thread Access: [Main thread creates, audio threads use via COM]
 * Lifetime: Init to Shutdown
 */
static IMMDeviceEnumerator* g_audioEnumerator = NULL;

/* Buffer size constants */
#define MIX_BUFFER_SIZE (AUDIO_BYTES_PER_SEC * 5)     /* 5 seconds */
#define SOURCE_BUFFER_SIZE (AUDIO_BYTES_PER_SEC * 2)  /* 2 seconds per source */

BOOL AudioCapture_Init(void) {
    if (g_audioEnumerator) return TRUE;
    
    HRESULT hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator_Shared,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator_Shared,
        (void**)&g_audioEnumerator
    );
    
    if (FAILED(hr)) {
        Logger_Log("AudioCapture_Init: CoCreateInstance failed (0x%08X)\n", hr);
        return FALSE;
    }
    return TRUE;
}

void AudioCapture_Shutdown(void) {
    if (g_audioEnumerator) {
        g_audioEnumerator->lpVtbl->Release(g_audioEnumerator);
        g_audioEnumerator = NULL;
    }
}

// Create a single capture source
// Uses goto-cleanup pattern for consistent resource cleanup on all error paths
static AudioCaptureSource* CreateSource(const char* deviceId) {
    if (!deviceId || deviceId[0] == '\0' || !g_audioEnumerator) {
        return NULL;
    }
    
    // Initialize all resources to NULL for safe cleanup
    AudioCaptureSource* src = NULL;
    BOOL csInitialized = FALSE;
    HRESULT hr;
    
    src = (AudioCaptureSource*)calloc(1, sizeof(AudioCaptureSource));
    if (!src) goto cleanup;
    
    strncpy(src->deviceId, deviceId, sizeof(src->deviceId) - 1);
    InitializeCriticalSection(&src->lock);
    csInitialized = TRUE;
    
    // Get device info to determine if loopback
    AudioDeviceInfo info;
    if (AudioDevice_GetById(deviceId, &info)) {
        src->type = info.type;
        src->isLoopback = (info.type == AUDIO_DEVICE_OUTPUT);
        Logger_Log("CreateSource: device='%s', type=%d, isLoopback=%d\n", 
            deviceId, info.type, src->isLoopback);
    } else {
        Logger_Log("CreateSource: AudioDevice_GetById FAILED for '%s'\n", deviceId);
    }
    
    // Get device by ID
    WCHAR wideId[256];
    Util_Utf8ToWide(deviceId, wideId, 256);
    
    hr = g_audioEnumerator->lpVtbl->GetDevice(g_audioEnumerator, wideId, &src->device);
    if (FAILED(hr)) goto cleanup;
    
    // Activate audio client
    hr = src->device->lpVtbl->Activate(
        src->device,
        &IID_IAudioClient_Shared,
        CLSCTX_ALL,
        NULL,
        (void**)&src->audioClient
    );
    if (FAILED(hr)) goto cleanup;
    
    // Get device format
    hr = src->audioClient->lpVtbl->GetMixFormat(src->audioClient, &src->deviceFormat);
    if (FAILED(hr)) goto cleanup;
    
    // Log device format for debugging
    Logger_Log("Audio device format: %d Hz, %d ch, %d bit, tag=%d (target: %d Hz)\n",
        src->deviceFormat->nSamplesPerSec,
        src->deviceFormat->nChannels,
        src->deviceFormat->wBitsPerSample,
        src->deviceFormat->wFormatTag,
        AUDIO_SAMPLE_RATE);
    
    // Set up target format (what we want)
    src->targetFormat.wFormatTag = WAVE_FORMAT_PCM;
    src->targetFormat.nChannels = AUDIO_CHANNELS;
    src->targetFormat.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    src->targetFormat.wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    src->targetFormat.nBlockAlign = AUDIO_BLOCK_ALIGN;
    src->targetFormat.nAvgBytesPerSec = AUDIO_BYTES_PER_SEC;
    src->targetFormat.cbSize = 0;
    
    // Allocate buffer
    src->bufferSize = SOURCE_BUFFER_SIZE;
    src->buffer = (BYTE*)malloc(src->bufferSize);
    if (!src->buffer) goto cleanup;
    
    // Success - return the source
    return src;
    
cleanup:
    /* Clean up in reverse order of acquisition using SAFE_* macros */
    /* All pointers were initialized to NULL by calloc, so SAFE_* macros are safe */
    if (src) {
        SAFE_FREE(src->buffer);
        SAFE_COTASKMEM_FREE(src->deviceFormat);
        SAFE_RELEASE(src->audioClient);
        SAFE_RELEASE(src->device);
        if (csInitialized) DeleteCriticalSection(&src->lock);
        free(src);
    }
    return NULL;
}

/* Destroy a capture source using SAFE_* macros for consistent cleanup */
static void DestroySource(AudioCaptureSource* src) {
    if (!src) return;
    
    SAFE_RELEASE(src->captureClient);
    SAFE_COTASKMEM_FREE(src->deviceFormat);
    SAFE_RELEASE(src->audioClient);
    SAFE_RELEASE(src->device);
    SAFE_FREE(src->buffer);
    
    DeleteCriticalSection(&src->lock);
    free(src);
}

// Forward declaration for SourceCaptureThread
static DWORD WINAPI SourceCaptureThread(LPVOID param);

// Forward declaration for InitSourceCapture
static BOOL InitSourceCapture(AudioCaptureSource* src);

/*
 * MULTI-RESOURCE FUNCTION: TryRecoverSource
 * Resources: 4 - device, audioClient, deviceFormat, captureClient (via InitSourceCapture)
 * Pattern: goto-cleanup with SAFE_*
 * 
 * Try to recover an invalidated audio source.
 * This releases old COM objects and re-acquires them.
 * Returns TRUE if recovery succeeded.
 * 
 * IMPORTANT: Caller must ensure source thread is stopped before calling.
 */
static BOOL TryRecoverSource(AudioCaptureSource* src) {
    if (!src || !g_audioEnumerator) return FALSE;
    
    HRESULT hr;
    BOOL audioStarted = FALSE;
    
    Logger_Log("Audio source '%s' attempting recovery...\n", src->deviceId);
    
    // Release old COM objects (they're invalid anyway)
    // SAFE_* macros handle NULL and set to NULL after release
    SAFE_RELEASE(src->captureClient);
    SAFE_COTASKMEM_FREE(src->deviceFormat);
    SAFE_RELEASE(src->audioClient);
    SAFE_RELEASE(src->device);
    
    // Try to reacquire device
    WCHAR wideId[256];
    Util_Utf8ToWide(src->deviceId, wideId, 256);
    
    hr = g_audioEnumerator->lpVtbl->GetDevice(g_audioEnumerator, wideId, &src->device);
    if (FAILED(hr)) {
        Logger_Log("Audio source '%s' recovery failed: device not found (0x%08X)\n", src->deviceId, hr);
        goto cleanup;
    }
    
    // Activate audio client
    hr = src->device->lpVtbl->Activate(
        src->device,
        &IID_IAudioClient_Shared,
        CLSCTX_ALL,
        NULL,
        (void**)&src->audioClient
    );
    if (FAILED(hr)) {
        Logger_Log("Audio source '%s' recovery failed: Activate (0x%08X)\n", src->deviceId, hr);
        goto cleanup;
    }
    
    // Get device format
    hr = src->audioClient->lpVtbl->GetMixFormat(src->audioClient, &src->deviceFormat);
    if (FAILED(hr)) {
        Logger_Log("Audio source '%s' recovery failed: GetMixFormat (0x%08X)\n", src->deviceId, hr);
        goto cleanup;
    }
    
    // Initialize capture (creates captureClient)
    if (!InitSourceCapture(src)) {
        Logger_Log("Audio source '%s' recovery failed: InitSourceCapture\n", src->deviceId);
        goto cleanup;
    }
    
    // Start the audio client
    hr = src->audioClient->lpVtbl->Start(src->audioClient);
    if (FAILED(hr)) {
        Logger_Log("Audio source '%s' recovery failed: Start (0x%08X)\n", src->deviceId, hr);
        goto cleanup;
    }
    audioStarted = TRUE;
    
    // Clear invalidation flag and error count (use Interlocked for thread safety)
    InterlockedExchange(&src->deviceInvalidated, FALSE);
    src->consecutiveErrors = 0;
    src->hasReceivedPacket = FALSE;
    
    // Clear buffer (protected by critical section)
    EnterCriticalSection(&src->lock);
    src->bufferAvailable = 0;
    src->bufferWritePos = 0;
    LeaveCriticalSection(&src->lock);
    
    // Restart capture thread (use Interlocked for thread safety)
    InterlockedExchange(&src->active, TRUE);
    src->captureThread = CreateThread(NULL, 0, SourceCaptureThread, src, 0, NULL);
    if (!src->captureThread) {
        Logger_Log("Audio source '%s' recovery failed: CreateThread\n", src->deviceId);
        InterlockedExchange(&src->active, FALSE);
        goto cleanup;
    }
    
    Logger_Log("Audio source '%s' recovered successfully!\n", src->deviceId);
    return TRUE;
    
cleanup:
    // Release in reverse order of acquisition
    if (audioStarted && src->audioClient) {
        src->audioClient->lpVtbl->Stop(src->audioClient);
    }
    SAFE_RELEASE(src->captureClient);
    SAFE_COTASKMEM_FREE(src->deviceFormat);
    SAFE_RELEASE(src->audioClient);
    SAFE_RELEASE(src->device);
    return FALSE;
}

// Recovery interval: try every 5 seconds
#define AUDIO_RECOVERY_INTERVAL_MS 5000

// Initialize a source for capture
static BOOL InitSourceCapture(AudioCaptureSource* src) {
    if (!src || !src->audioClient) return FALSE;
    
    // Buffer duration in 100ns units (100ms)
    REFERENCE_TIME bufferDuration = WASAPI_BUFFER_DURATION_100NS;
    
    // NOTE: Don't use AUDCLNT_STREAMFLAGS_EVENTCALLBACK - we poll instead
    // Using event callback requires SetEventHandle which adds complexity
    DWORD streamFlags = 0;
    if (src->isLoopback) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    
    Logger_Log("InitSourceCapture: device='%s', isLoopback=%d, flags=0x%X\n", 
        src->deviceId, src->isLoopback, streamFlags);
    
    // Initialize audio client
    // Use device's native format - we'll convert later
    HRESULT hr = src->audioClient->lpVtbl->Initialize(
        src->audioClient,
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration,
        0,
        src->deviceFormat,
        NULL
    );
    
    if (FAILED(hr)) {
        Logger_Log("InitSourceCapture: Initialize failed (0x%08X) for '%s'\n", hr, src->deviceId);
        return FALSE;
    }
    
    Logger_Log("InitSourceCapture: Initialize succeeded for '%s'\n", src->deviceId);
    
    // Get capture client
    hr = src->audioClient->lpVtbl->GetService(
        src->audioClient,
        &IID_IAudioCaptureClient_Shared,
        (void**)&src->captureClient
    );
    
    if (FAILED(hr)) {
        Logger_Log("InitSourceCapture: GetService failed (0x%08X) for '%s'\n", hr, src->deviceId);
        return FALSE;
    }
    
    Logger_Log("InitSourceCapture: GetService succeeded for '%s'\n", src->deviceId);
    return TRUE;
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT for WAVEFORMATEXTENSIBLE
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_Local = 
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// Convert audio samples to target format WITH RESAMPLING
// Uses linear interpolation for sample rate conversion
static int ConvertSamples(
    const BYTE* srcData, int srcSamples, const WAVEFORMATEX* srcFmt,
    BYTE* dstData, int dstMaxBytes, const WAVEFORMATEX* dstFmt
) {
    if (!srcData || !dstData || srcSamples == 0) return 0;
    
    // Get source format details
    int srcChannels = srcFmt->nChannels;
    int srcBits = srcFmt->wBitsPerSample;
    int srcRate = srcFmt->nSamplesPerSec;
    
    // Detect float format - check both plain tag and extensible SubFormat
    BOOL srcFloat = (srcFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (srcFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && srcFmt->cbSize >= 22) {
        // WAVEFORMATEXTENSIBLE - check SubFormat GUID
        typedef struct {
            WAVEFORMATEX Format;
            union {
                WORD wValidBitsPerSample;
                WORD wSamplesPerBlock;
                WORD wReserved;
            } Samples;
            DWORD dwChannelMask;
            GUID SubFormat;
        } WAVEFORMATEXTENSIBLE_LOCAL;
        
        const WAVEFORMATEXTENSIBLE_LOCAL* extFmt = (const WAVEFORMATEXTENSIBLE_LOCAL*)srcFmt;
        if (memcmp(&extFmt->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_Local, sizeof(GUID)) == 0) {
            srcFloat = TRUE;
        }
    }
    
    // Get dest format details
    int dstChannels = dstFmt->nChannels;
    int dstRate = dstFmt->nSamplesPerSec;
    
    // Calculate output sample count based on sample rate ratio
    int dstSamples = (int)((double)srcSamples * dstRate / srcRate);
    if (dstSamples * dstFmt->nBlockAlign > dstMaxBytes) {
        dstSamples = dstMaxBytes / dstFmt->nBlockAlign;
    }
    if (dstSamples <= 0) return 0;
    
    // Helper function to read a source sample as float stereo
    #define READ_SRC_SAMPLE(idx, pLeft, pRight) do { \
        int _i = (idx); \
        if (_i >= srcSamples) _i = srcSamples - 1; \
        if (_i < 0) _i = 0; \
        float _l = 0, _r = 0; \
        if (srcFloat && srcBits == 32) { \
            const float* sf = (const float*)(srcData + _i * srcFmt->nBlockAlign); \
            _l = sf[0]; \
            _r = (srcChannels >= 2) ? sf[1] : _l; \
        } else if (srcBits == 16) { \
            const short* ss = (const short*)(srcData + _i * srcFmt->nBlockAlign); \
            _l = ss[0] / AUDIO_16BIT_MAX; \
            _r = (srcChannels >= 2) ? ss[1] / AUDIO_16BIT_MAX : _l; \
        } else if (srcBits == 24) { \
            const BYTE* p = srcData + _i * srcFmt->nBlockAlign; \
            int s1 = (p[0] | (p[1] << 8) | (p[2] << 16)); \
            if (s1 & AUDIO_24BIT_SIGN_MASK) s1 |= AUDIO_24BIT_SIGN_EXTEND; \
            _l = s1 / AUDIO_24BIT_MAX; \
            if (srcChannels >= 2) { \
                int s2 = (p[3] | (p[4] << 8) | (p[5] << 16)); \
                if (s2 & AUDIO_24BIT_SIGN_MASK) s2 |= AUDIO_24BIT_SIGN_EXTEND; \
                _r = s2 / AUDIO_24BIT_MAX; \
            } else { _r = _l; } \
        } \
        *(pLeft) = _l; *(pRight) = _r; \
    } while(0)
    
    // Resample using linear interpolation
    double srcPos = 0.0;
    double srcStep = (double)srcRate / dstRate;
    
    for (int i = 0; i < dstSamples; i++) {
        int srcIdx = (int)srcPos;
        double frac = srcPos - srcIdx;
        
        // Read two adjacent source samples
        float left0, right0, left1, right1;
        READ_SRC_SAMPLE(srcIdx, &left0, &right0);
        READ_SRC_SAMPLE(srcIdx + 1, &left1, &right1);
        
        // Linear interpolation
        float left = left0 + (float)(frac * (left1 - left0));
        float right = right0 + (float)(frac * (right1 - right0));
        
        // Clamp
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        
        // Write to destination as 16-bit
        short* dstShorts = (short*)(dstData + i * dstFmt->nBlockAlign);
        dstShorts[0] = (short)(left * AUDIO_16BIT_MAX_SIGNED);
        if (dstChannels >= 2) {
            dstShorts[1] = (short)(right * AUDIO_16BIT_MAX_SIGNED);
        }
        
        srcPos += srcStep;
    }
    
    #undef READ_SRC_SAMPLE
    
    return dstSamples * dstFmt->nBlockAlign;
}

// Capture thread for a single source
static DWORD WINAPI SourceCaptureThread(LPVOID param) {
    AudioCaptureSource* src = (AudioCaptureSource*)param;
    if (!src) return 0;
    
    // Temporary buffer for format conversion
    BYTE* convBuffer = (BYTE*)malloc(SOURCE_BUFFER_SIZE);
    if (!convBuffer) {
        Logger_Log("SourceCaptureThread: malloc failed for convBuffer\n");
        InterlockedExchange(&src->active, FALSE);
        return 0;
    }
    
    while (InterlockedCompareExchange(&src->active, 0, 0)) {
        // Heartbeat (use AUDIO_SRC1 - we could differentiate but keep it simple)
        Logger_Heartbeat(THREAD_AUDIO_SRC1);
        
        UINT32 packetLength = 0;
        HRESULT hr = src->captureClient->lpVtbl->GetNextPacketSize(
            src->captureClient, &packetLength
        );
        
        if (FAILED(hr)) {
            // Check for device invalidation (device yanked, audio service restarted, etc.)
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                InterlockedExchange(&src->deviceInvalidated, TRUE);
                Logger_Log("Audio source '%s' device invalidated (hr=0x%08X) - stopping capture",
                      src->deviceId, hr);
                break;  // Exit the loop - device is gone
            }
            src->consecutiveErrors++;
            if (src->consecutiveErrors > MAX_CONSECUTIVE_ERRORS) {
                Logger_Log("Audio source '%s' too many consecutive errors - stopping", src->deviceId);
                break;
            }
            Sleep(AUDIO_POLL_INTERVAL_MS);
            continue;
        }
        src->consecutiveErrors = 0;  // Reset on success
        
        while (packetLength > 0 && InterlockedCompareExchange(&src->active, 0, 0)) {
            BYTE* data = NULL;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            
            hr = src->captureClient->lpVtbl->GetBuffer(
                src->captureClient,
                &data,
                &numFrames,
                &flags,
                NULL,
                NULL
            );
            
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                    InterlockedExchange(&src->deviceInvalidated, TRUE);
                }
                break;
            }
            
            if (numFrames > 0 && data) {
                int convertedBytes = 0;
                
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Silent - write zeros with proper resampling
                    // numFrames is in source sample rate, convert to target rate
                    int srcRate = src->deviceFormat->nSamplesPerSec;
                    int dstRate = src->targetFormat.nSamplesPerSec;
                    int dstFrames = (int)((double)numFrames * dstRate / srcRate);
                    convertedBytes = dstFrames * src->targetFormat.nBlockAlign;
                    if (convertedBytes > SOURCE_BUFFER_SIZE) {
                        convertedBytes = SOURCE_BUFFER_SIZE;
                    }
                    memset(convBuffer, 0, convertedBytes);
                } else {
                    // Convert to target format
                    convertedBytes = ConvertSamples(
                        data, numFrames, src->deviceFormat,
                        convBuffer, SOURCE_BUFFER_SIZE, &src->targetFormat
                    );
                }
                
                // Write to source ring buffer
                if (convertedBytes > 0) {
                    EnterCriticalSection(&src->lock);
                    
                    int spaceAvailable = src->bufferSize - src->bufferAvailable;
                    if (convertedBytes > spaceAvailable) {
                        // Buffer full - drop oldest data
                        int toDrop = convertedBytes - spaceAvailable;
                        src->bufferAvailable -= toDrop;
                    }
                    
                    // Write in possibly two parts (ring buffer wrap)
                    int writePos = src->bufferWritePos;
                    int toEnd = src->bufferSize - writePos;
                    
                    if (convertedBytes <= toEnd) {
                        memcpy(src->buffer + writePos, convBuffer, convertedBytes);
                    } else {
                        memcpy(src->buffer + writePos, convBuffer, toEnd);
                        memcpy(src->buffer, convBuffer + toEnd, convertedBytes - toEnd);
                    }
                    
                    src->bufferWritePos = (writePos + convertedBytes) % src->bufferSize;
                    src->bufferAvailable += convertedBytes;
                    
                    // Record that we received a packet (for event-driven source detection)
                    QueryPerformanceCounter(&src->lastPacketTime);
                    src->hasReceivedPacket = TRUE;
                    
                    LeaveCriticalSection(&src->lock);
                }
            }
            
            src->captureClient->lpVtbl->ReleaseBuffer(src->captureClient, numFrames);
            
            hr = src->captureClient->lpVtbl->GetNextPacketSize(
                src->captureClient, &packetLength
            );
            if (FAILED(hr)) break;
        }
        
        Sleep(AUDIO_POLL_INTERVAL_MS);  // Poll interval between checks
    }
    
    free(convBuffer);
    return 0;
}

/*
 * MULTI-RESOURCE FUNCTION: AudioCapture_Create
 * Resources: 4 - ctx (calloc), mixLock (CS), mixBuffer (malloc), sources[3]
 * Pattern: goto-cleanup with SAFE_FREE
 * Init: calloc ensures NULL initialization
 */
AudioCaptureContext* AudioCapture_Create(
    const char* deviceId1, int volume1,
    const char* deviceId2, int volume2,
    const char* deviceId3, int volume3
) {
    if (!AudioCapture_Init()) {
        return NULL;
    }
    
    AudioCaptureContext* ctx = (AudioCaptureContext*)calloc(1, sizeof(AudioCaptureContext));
    if (!ctx) return NULL;
    
    BOOL csInitialized = FALSE;
    
    InitializeCriticalSection(&ctx->mixLock);
    csInitialized = TRUE;
    QueryPerformanceFrequency(&ctx->perfFreq);
    
    // Allocate mix buffer
    ctx->mixBufferSize = MIX_BUFFER_SIZE;
    ctx->mixBuffer = (BYTE*)malloc(ctx->mixBufferSize);
    if (!ctx->mixBuffer) goto cleanup;
    
    // Create sources and assign volumes to match source index
    // This ensures volumes[i] corresponds to sources[i]
    const char* deviceIds[] = {deviceId1, deviceId2, deviceId3};
    int volumes[] = {volume1, volume2, volume3};
    
    for (int i = 0; i < 3; i++) {
        if (deviceIds[i] && deviceIds[i][0] != '\0') {
            AudioCaptureSource* src = CreateSource(deviceIds[i]);
            if (src) {
                // Assign volume to match source index, not device slot index
                int srcIdx = ctx->sourceCount;
                ctx->volumes[srcIdx] = (volumes[i] < 0) ? 0 : (volumes[i] > 100) ? 100 : volumes[i];
                ctx->sources[srcIdx] = src;
                ctx->sourceCount++;
                Logger_Log("Audio source %d: device slot %d, volume=%d%%\n", srcIdx, i, ctx->volumes[srcIdx]);
            }
        }
    }
    
    return ctx;
    
cleanup:
    SAFE_FREE(ctx->mixBuffer);
    if (csInitialized) DeleteCriticalSection(&ctx->mixLock);
    free(ctx);
    return NULL;
}

void AudioCapture_Destroy(AudioCaptureContext* ctx) {
    if (!ctx) return;
    
    AudioCapture_Stop(ctx);
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        DestroySource(ctx->sources[i]);
    }
    
    SAFE_FREE(ctx->mixBuffer);
    
    DeleteCriticalSection(&ctx->mixLock);
    free(ctx);
}

// ============================================================================
// Mix Thread Helper Functions
// ============================================================================

/*
 * Check if an audio source is dormant (event-driven virtual device with no recent packets).
 * A source is dormant if it has received packets before but hasn't recently.
 * Dormant sources contribute silence rather than blocking the mixer.
 */
static BOOL IsSourceDormant(AudioCaptureSource* src, LARGE_INTEGER now, double dormantThresholdMs) {
    if (!src->hasReceivedPacket) return FALSE;
    
    double msSincePacket = (double)(now.QuadPart - src->lastPacketTime.QuadPart) * 1000.0 / src->perfFreq.QuadPart;
    return (msSincePacket > dormantThresholdMs);
}

/*
 * Read audio data from a source's ring buffer into the provided buffer.
 * Returns the number of bytes read.
 */
static int ReadFromSourceBuffer(AudioCaptureSource* src, BYTE* destBuffer, int maxBytes) {
    int toRead = src->bufferAvailable;
    if (toRead > maxBytes) toRead = maxBytes;
    
    if (toRead > 0) {
        int readPos = (src->bufferWritePos - src->bufferAvailable + src->bufferSize) % src->bufferSize;
        int toEnd = src->bufferSize - readPos;
        
        if (toRead <= toEnd) {
            memcpy(destBuffer, src->buffer + readPos, toRead);
        } else {
            memcpy(destBuffer, src->buffer + readPos, toEnd);
            memcpy(destBuffer + toEnd, src->buffer, toRead - toEnd);
        }
        
        src->bufferAvailable -= toRead;
    }
    
    return toRead;
}

/*
 * Mix multiple audio source buffers into a single output buffer.
 * Applies per-source volume, tracks peak levels, and clamps to prevent clipping.
 */
static void MixAudioSamples(
    BYTE** srcBuffers, int* srcBytes, int* volumes,
    int sourceCount, int numSamples,
    BYTE* outBuffer, int* peakLeft, int* peakRight)
{
    for (int s = 0; s < numSamples; s++) {
        int leftSum = 0, rightSum = 0;
        
        for (int i = 0; i < sourceCount; i++) {
            // Check if this source has data for this sample
            if (srcBytes[i] > s * AUDIO_BLOCK_ALIGN) {
                short* samples = (short*)(srcBuffers[i] + s * AUDIO_BLOCK_ALIGN);
                // Apply per-source volume (0-100)
                int vol = volumes[i];
                leftSum += (samples[0] * vol) / 100;
                rightSum += (samples[1] * vol) / 100;
            }
            // Sources without data for this sample contribute silence (0) implicitly
        }
        
        // Track peaks
        int absL = leftSum < 0 ? -leftSum : leftSum;
        int absR = rightSum < 0 ? -rightSum : rightSum;
        if (absL > *peakLeft) *peakLeft = absL;
        if (absR > *peakRight) *peakRight = absR;
        
        // Clamp to prevent clipping
        if (leftSum > 32767) leftSum = 32767;
        if (leftSum < -32768) leftSum = -32768;
        if (rightSum > 32767) rightSum = 32767;
        if (rightSum < -32768) rightSum = -32768;
        
        short* outSamples = (short*)(outBuffer + s * AUDIO_BLOCK_ALIGN);
        outSamples[0] = (short)leftSum;
        outSamples[1] = (short)rightSum;
    }
}

/*
 * Write mixed audio chunk to the context's ring buffer.
 * Drops oldest data if buffer is full.
 */
static void WriteMixedToBuffer(AudioCaptureContext* ctx, BYTE* mixChunk, int bytesToMix) {
    EnterCriticalSection(&ctx->mixLock);
    
    int spaceAvailable = ctx->mixBufferSize - ctx->mixBufferAvailable;
    if (bytesToMix > spaceAvailable) {
        // Drop oldest
        int toDrop = bytesToMix - spaceAvailable;
        ctx->mixBufferAvailable -= toDrop;
        ctx->mixBufferReadPos = (ctx->mixBufferReadPos + toDrop) % ctx->mixBufferSize;
    }
    
    int writePos = ctx->mixBufferWritePos;
    int toEnd = ctx->mixBufferSize - writePos;
    
    if (bytesToMix <= toEnd) {
        memcpy(ctx->mixBuffer + writePos, mixChunk, bytesToMix);
    } else {
        memcpy(ctx->mixBuffer + writePos, mixChunk, toEnd);
        memcpy(ctx->mixBuffer, mixChunk + toEnd, bytesToMix - toEnd);
    }
    
    ctx->mixBufferWritePos = (writePos + bytesToMix) % ctx->mixBufferSize;
    ctx->mixBufferAvailable += bytesToMix;
    
    LeaveCriticalSection(&ctx->mixLock);
}

// Mix capture thread - reads from all sources and mixes
static DWORD WINAPI MixCaptureThread(LPVOID param) {
    AudioCaptureContext* ctx = (AudioCaptureContext*)param;
    if (!ctx) return 0;
    
    // Peak tracking for logging - reset each session
    int peakLeft = 0, peakRight = 0;
    int logCounter = 0;
    
    // Temp buffers for reading from sources
    BYTE* srcBuffers[MAX_AUDIO_SOURCES] = {0};
    int srcBytes[MAX_AUDIO_SOURCES] = {0};
    BOOL srcDormant[MAX_AUDIO_SOURCES] = {0};  // TRUE if source is event-driven and currently silent
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        srcBuffers[i] = (BYTE*)malloc(AUDIO_MIX_CHUNK_SIZE);
        if (!srcBuffers[i]) {
            // Allocation failed - clean up previously allocated buffers and exit
            Logger_Log("MixCaptureThread: malloc failed for srcBuffer[%d]\n", i);
            for (int j = 0; j < i; j++) {
                if (srcBuffers[j]) free(srcBuffers[j]);
            }
            InterlockedExchange(&ctx->running, FALSE);
            return 0;
        }
    }
    
    const int chunkSize = AUDIO_MIX_CHUNK_SIZE;  // Process in chunks
    const double dormantThresholdMs = DORMANT_THRESHOLD_MS;  // Consider source dormant after no packets
    
    // Rate limiting: track how much audio we should output based on elapsed time
    LARGE_INTEGER rateStartTime;
    QueryPerformanceCounter(&rateStartTime);
    LONGLONG totalBytesOutput = 0;  // Total bytes we've written to mix buffer
    
    // Recovery timing
    LARGE_INTEGER lastRecoveryCheck;
    QueryPerformanceCounter(&lastRecoveryCheck);
    
    while (InterlockedCompareExchange(&ctx->running, 0, 0)) {
        // Heartbeat for monitoring
        Logger_Heartbeat(THREAD_AUDIO_MIX);
        
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        
        // ====================================================================
        // Check for invalidated sources and attempt recovery (every 5 seconds)
        // This is lightweight: just checks a flag and timer, no COM calls
        // unless we actually need to recover
        // ====================================================================
        double msSinceRecoveryCheck = (double)(now.QuadPart - lastRecoveryCheck.QuadPart) * 1000.0 / ctx->perfFreq.QuadPart;
        if (msSinceRecoveryCheck >= AUDIO_RECOVERY_INTERVAL_MS) {
            lastRecoveryCheck = now;
            
            for (int i = 0; i < ctx->sourceCount; i++) {
                AudioCaptureSource* src = ctx->sources[i];
                if (!src) continue;
                
                // Check if source is invalidated AND thread has stopped
                if (InterlockedCompareExchange(&src->deviceInvalidated, 0, 0) &&
                    !InterlockedCompareExchange(&src->active, 0, 0)) {
                    
                    // Check if enough time passed since last recovery attempt for this source
                    double msSinceLastAttempt = (double)(now.QuadPart - src->lastRecoveryAttempt.QuadPart) * 1000.0 / ctx->perfFreq.QuadPart;
                    if (msSinceLastAttempt >= AUDIO_RECOVERY_INTERVAL_MS || src->lastRecoveryAttempt.QuadPart == 0) {
                        src->lastRecoveryAttempt = now;
                        
                        // Wait for thread to fully exit if handle exists
                        if (src->captureThread) {
                            WaitForSingleObject(src->captureThread, 1000);
                            CloseHandle(src->captureThread);
                            src->captureThread = NULL;
                        }
                        
                        TryRecoverSource(src);
                    }
                }
            }
        }
        
        // Check how much data is available from each source
        int maxBytes = 0;
        int activeSources = 0;
        int nonDormantSources = 0;
        int availableBytes[MAX_AUDIO_SOURCES] = {0};
        
        for (int i = 0; i < ctx->sourceCount; i++) {
            AudioCaptureSource* src = ctx->sources[i];
            if (!src || !InterlockedCompareExchange(&src->active, 0, 0)) continue;
            
            activeSources++;
            EnterCriticalSection(&src->lock);
            availableBytes[i] = src->bufferAvailable;
            
            // Check if source is dormant (uses helper function)
            srcDormant[i] = FALSE;
            if (availableBytes[i] == 0) {
                srcDormant[i] = IsSourceDormant(src, now, dormantThresholdMs);
            }
            
            LeaveCriticalSection(&src->lock);
            
            if (!srcDormant[i]) {
                nonDormantSources++;
                if (availableBytes[i] > maxBytes) {
                    maxBytes = availableBytes[i];
                }
            }
        }
        
        // Need at least one non-dormant source with data to proceed
        if (nonDormantSources == 0 || maxBytes == 0) {
            Sleep(2);  // Wait for data
            continue;
        }
        
        // Rate limiting: calculate how many bytes we SHOULD have output by now
        // This prevents sources from "running ahead" of wall-clock time
        double elapsedSec = (double)(now.QuadPart - rateStartTime.QuadPart) / ctx->perfFreq.QuadPart;
        LONGLONG expectedBytes = (LONGLONG)(elapsedSec * AUDIO_BYTES_PER_SEC);
        LONGLONG bytesAllowed = expectedBytes - totalBytesOutput;
        
        // If we're ahead of schedule, wait
        if (bytesAllowed < chunkSize / 2) {
            Sleep(2);
            continue;
        }
        
        // Process up to chunkSize bytes from non-dormant sources
        int processBytes = maxBytes;
        if (processBytes > chunkSize) processBytes = chunkSize;
        // Also limit by rate
        if (processBytes > bytesAllowed) processBytes = (int)bytesAllowed;
        // Align to sample boundaries (4 bytes per sample for stereo 16-bit)
        processBytes = (processBytes / AUDIO_BLOCK_ALIGN) * AUDIO_BLOCK_ALIGN;
        if (processBytes <= 0) {
            Sleep(1);
            continue;
        }
        
        // Read from each source - dormant sources contribute silence
        for (int i = 0; i < ctx->sourceCount; i++) {
            AudioCaptureSource* src = ctx->sources[i];
            // Thread-safe check
            if (!src || !InterlockedCompareExchange(&src->active, 0, 0) || srcDormant[i]) {
                // Dormant sources contribute silence (srcBytes[i] = 0 means silence in the mixer)
                srcBytes[i] = 0;
                continue;
            }
            
            EnterCriticalSection(&src->lock);
            srcBytes[i] = ReadFromSourceBuffer(src, srcBuffers[i], processBytes);
            LeaveCriticalSection(&src->lock);
        }
        
        // Find how many bytes to actually process (max of what any source provided)
        int bytesToMix = 0;
        for (int i = 0; i < ctx->sourceCount; i++) {
            if (srcBytes[i] > bytesToMix) bytesToMix = srcBytes[i];
        }
        
        // Mix sources using helper function
        if (bytesToMix > 0) {
            BYTE* mixChunk = (BYTE*)malloc(bytesToMix);
            if (!mixChunk) {
                Logger_Log("MixCaptureThread: malloc failed for mixChunk (%d bytes)\n", bytesToMix);
                Sleep(10);  // Back off on allocation failure
                continue;
            }
            memset(mixChunk, 0, bytesToMix);
            int numSamples = bytesToMix / AUDIO_BLOCK_ALIGN;
            
            MixAudioSamples(srcBuffers, srcBytes, ctx->volumes, 
                           ctx->sourceCount, numSamples, 
                           mixChunk, &peakLeft, &peakRight);
            
            // Log peak levels periodically
            logCounter++;
            if (logCounter % 500 == 0) {
                // Convert to dB-ish scale (peak/32767 as percentage)
                float peakPctL = (float)peakLeft / 32767.0f * 100.0f;
                float peakPctR = (float)peakRight / 32767.0f * 100.0f;
                double rateElapsed = (double)(now.QuadPart - rateStartTime.QuadPart) / ctx->perfFreq.QuadPart;
                double actualRate = (rateElapsed > 0) ? (totalBytesOutput / rateElapsed) : 0;
                Logger_Log("Audio: L=%.1f%% R=%.1f%% bytes=[%d,%d,%d] dormant=[%d,%d,%d] rate=%.0f/s (target=%d)\n", 
                           peakPctL, peakPctR,
                           srcBytes[0], srcBytes[1], srcBytes[2],
                           srcDormant[0], srcDormant[1], srcDormant[2],
                           actualRate, AUDIO_BYTES_PER_SEC);
                peakLeft = 0;
                peakRight = 0;
            }
            
            // Write mixed audio to output buffer (using helper)
            WriteMixedToBuffer(ctx, mixChunk, bytesToMix);
            
            // Track total output for rate limiting
            totalBytesOutput += bytesToMix;
            
            free(mixChunk);
        }
    }
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        if (srcBuffers[i]) free(srcBuffers[i]);
    }
    
    return 0;
}

BOOL AudioCapture_Start(AudioCaptureContext* ctx) {
    // Precondition
    LWSR_ASSERT(ctx != NULL);
    
    // Thread-safe check
    if (!ctx || InterlockedCompareExchange(&ctx->running, 0, 0)) return FALSE;
    
    // Initialize and start each source
    for (int i = 0; i < ctx->sourceCount; i++) {
        AudioCaptureSource* src = ctx->sources[i];
        if (!src) continue;
        
        if (!InitSourceCapture(src)) {
            Logger_Log("AudioCapture_Start: InitSourceCapture failed for source %d\n", i);
            continue;
        }
        
        InterlockedExchange(&src->active, TRUE);
        
        // Initialize timing for event-driven source detection
        QueryPerformanceFrequency(&src->perfFreq);
        QueryPerformanceCounter(&src->lastPacketTime);
        src->hasReceivedPacket = FALSE;
        
        // Start audio client
        HRESULT hr = src->audioClient->lpVtbl->Start(src->audioClient);
        if (FAILED(hr)) {
            Logger_Log("AudioCapture: IAudioClient::Start failed (0x%08X) for device %s\n", hr, src->deviceId);
            InterlockedExchange(&src->active, FALSE);
            continue;
        }
        
        // Start capture thread for this source
        src->captureThread = CreateThread(NULL, 0, SourceCaptureThread, src, 0, NULL);
        if (!src->captureThread) {
            Logger_Log("AudioCapture: CreateThread failed for source %s\n", src->deviceId);
            src->audioClient->lpVtbl->Stop(src->audioClient);
            InterlockedExchange(&src->active, FALSE);
        }
    }
    
    // Record start time
    QueryPerformanceCounter(&ctx->startTime);
    
    // Start mix thread - use atomic write
    InterlockedExchange(&ctx->running, TRUE);
    ctx->captureThread = CreateThread(NULL, 0, MixCaptureThread, ctx, 0, NULL);
    if (!ctx->captureThread) {
        Logger_Log("AudioCapture: CreateThread failed for mix thread\n");
        InterlockedExchange(&ctx->running, FALSE);
        
        // Clean up source threads that were started
        for (int i = 0; i < ctx->sourceCount; i++) {
            AudioCaptureSource* src = ctx->sources[i];
            if (!src) continue;
            
            InterlockedExchange(&src->active, FALSE);
            if (src->audioClient) {
                src->audioClient->lpVtbl->Stop(src->audioClient);
            }
            if (src->captureThread) {
                WaitForSingleObject(src->captureThread, 1000);
                CloseHandle(src->captureThread);
                src->captureThread = NULL;
            }
        }
        return FALSE;
    }
    
    return TRUE;
}

void AudioCapture_Stop(AudioCaptureContext* ctx) {
    if (!ctx) return;
    
    // Thread-safe: use atomic write
    InterlockedExchange(&ctx->running, FALSE);
    
    // Stop sources and wait for their threads
    for (int i = 0; i < ctx->sourceCount; i++) {
        AudioCaptureSource* src = ctx->sources[i];
        if (!src) continue;
        
        // Thread-safe: use atomic write
        InterlockedExchange(&src->active, FALSE);
        
        if (src->audioClient) {
            src->audioClient->lpVtbl->Stop(src->audioClient);
        }
        
        // Wait for source capture thread to finish
        if (src->captureThread) {
            DWORD waitResult = WaitForSingleObject(src->captureThread, 3000);
            if (waitResult == WAIT_TIMEOUT) {
                Logger_Log("AudioCapture_Stop: Warning - source thread %d did not exit in time\n", i);
            }
            CloseHandle(src->captureThread);
            src->captureThread = NULL;
        }
    }
    
    // Wait for mix thread
    if (ctx->captureThread) {
        DWORD waitResult = WaitForSingleObject(ctx->captureThread, 3000);
        if (waitResult == WAIT_TIMEOUT) {
            Logger_Log("AudioCapture_Stop: Warning - mix thread did not exit in time\n");
        }
        CloseHandle(ctx->captureThread);
        ctx->captureThread = NULL;
    }
}

int AudioCapture_Read(AudioCaptureContext* ctx, BYTE* buffer, int maxBytes, LONGLONG* timestamp) {
    // Preconditions
    LWSR_ASSERT(ctx != NULL);
    LWSR_ASSERT(buffer != NULL);
    LWSR_ASSERT(maxBytes > 0);
    
    if (!ctx || !buffer) return 0;
    
    EnterCriticalSection(&ctx->mixLock);
    
    int available = ctx->mixBufferAvailable;
    if (available > maxBytes) available = maxBytes;
    
    if (available > 0) {
        int readPos = ctx->mixBufferReadPos;
        int toEnd = ctx->mixBufferSize - readPos;
        
        if (available <= toEnd) {
            memcpy(buffer, ctx->mixBuffer + readPos, available);
        } else {
            memcpy(buffer, ctx->mixBuffer + readPos, toEnd);
            memcpy(buffer + toEnd, ctx->mixBuffer, available - toEnd);
        }
        
        ctx->mixBufferReadPos = (readPos + available) % ctx->mixBufferSize;
        ctx->mixBufferAvailable -= available;
    }
    
    LeaveCriticalSection(&ctx->mixLock);
    
    // Calculate timestamp
    if (timestamp) {
        *timestamp = AudioCapture_GetTimestamp(ctx);
    }
    
    return available;
}

LONGLONG AudioCapture_GetTimestamp(AudioCaptureContext* ctx) {
    // Precondition
    LWSR_ASSERT(ctx != NULL);
    
    if (!ctx) return 0;
    
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    
    // Return time since start in 100ns units
    LONGLONG elapsed = now.QuadPart - ctx->startTime.QuadPart;
    return (elapsed * 10000000) / ctx->perfFreq.QuadPart;
}

BOOL AudioCapture_HasData(AudioCaptureContext* ctx) {
    // Precondition
    LWSR_ASSERT(ctx != NULL);
    
    if (!ctx) return FALSE;
    
    EnterCriticalSection(&ctx->mixLock);
    BOOL hasData = ctx->mixBufferAvailable > 0;
    LeaveCriticalSection(&ctx->mixLock);
    
    return hasData;
}

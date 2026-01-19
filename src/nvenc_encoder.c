/*
 * NVENC Hardware Encoder - Clean Implementation
 * Based on NVIDIA Video Codec SDK 13.0 nvEncodeAPI.h documentation
 * 
 * Architecture (per API docs lines 3380-3391):
 * - Main thread: Fast frame submission (non-blocking)
 * - Output thread: Waits on completion events, retrieves bitstream
 * 
 * Key API requirements implemented:
 * - Multiple input buffers for async pipelining (lines 3617-3627)
 * - Unmap AFTER LockBitstream returns (lines 4165-4167)
 * - Lock outputs in submission order (lines 3402-3404)
 * - Each output buffer has distinct completion event (line 2582)
 * 
 * CRITICAL FIX (per NVIDIA docs Section 6.3):
 * - Desktop Duplication and NVENC use SEPARATE D3D11 devices
 * - Shared textures with keyed mutex for cross-device synchronization
 * - This eliminates thread contention that caused hangs
 */

#include "nvenc_encoder.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <dxgi.h>
#include <dxgi1_2.h>

// Include official NVIDIA header
#include "nvEncodeAPI.h"

#define NvLog Logger_Log

// ============================================================================
// Constants
// ============================================================================

// Per API docs (line 3444-3445): "at least 4 more than number of B frames"
// With no B-frames, minimum is 4. We use 8 for better pipelining.
#define NUM_BUFFERS 8

// ============================================================================
// Encoder State
// ============================================================================

typedef struct {
    EncodedFrame frame;
    BOOL valid;
} OutputSlot;

struct NVENCEncoder {
    // NVENC core
    HMODULE nvencLib;
    NV_ENCODE_API_FUNCTION_LIST fn;
    void* encoder;
    
    // Own D3D11 device for NVENC (separate from capture device)
    // This is CRITICAL per NVIDIA docs: Desktop Duplication and NVENC
    // must use separate devices to avoid thread contention
    ID3D11Device* encDevice;
    ID3D11DeviceContext* encContext;
    
    // Reference to source device (for texture copies on source side)
    ID3D11Device* srcDevice;
    ID3D11DeviceContext* srcContext;
    
    // Dimensions and settings
    int width;
    int height;
    int fps;
    int qp;
    uint64_t frameDuration;  // 100-ns units
    
    // Per API: Each in-flight frame needs its own input buffer
    // Staging textures on SOURCE device (shared with encoder device)
    ID3D11Texture2D* stagingTextures[NUM_BUFFERS];  // On srcDevice, with SHARED flag
    HANDLE sharedHandles[NUM_BUFFERS];               // For cross-device sharing
    
    // Input textures on ENCODER device (opened from shared handles)
    ID3D11Texture2D* inputTextures[NUM_BUFFERS];    // On encDevice
    NV_ENC_REGISTERED_PTR registeredResources[NUM_BUFFERS];
    NV_ENC_INPUT_PTR mappedResources[NUM_BUFFERS];  // Track for unmap
    
    // Keyed mutexes for synchronization between devices
    IDXGIKeyedMutex* srcMutex[NUM_BUFFERS];   // On source device textures
    IDXGIKeyedMutex* encMutex[NUM_BUFFERS];   // On encoder device textures
    
    // Output bitstream buffers (one per in-flight frame)
    NV_ENC_OUTPUT_PTR outputBuffers[NUM_BUFFERS];
    
    // Per API (line 2582): Each output buffer needs distinct completion event
    HANDLE completionEvents[NUM_BUFFERS];
    
    // Timestamps for pending frames
    LONGLONG pendingTimestamps[NUM_BUFFERS];
    
    // Ring buffer indices
    int submitIndex;    // Next buffer to use for submission
    int retrieveIndex;  // Next buffer to retrieve from
    volatile LONG pendingCount;  // Frames in flight
    
    // Frame counter
    uint64_t frameNumber;
    
    // Output thread (per API lines 3384-3388)
    HANDLE outputThread;
    volatile BOOL stopThread;
    
    // Callback for completed frames
    EncodedFrameCallback frameCallback;
    void* callbackUserData;
    
    // Synchronization
    CRITICAL_SECTION submitLock;
    
    // Per-instance diagnostic counters (avoid static pollution)
    int pipelineFullCount;
    int mutexTimeoutCount;
    
    // Device lost detection
    volatile BOOL deviceLost;
    
    BOOL initialized;
};

// ============================================================================
// Forward Declarations
// ============================================================================

static unsigned __stdcall OutputThreadProc(void* param);
static BOOL CreateInputTextures(NVENCEncoder* enc);
static void DestroyInputTextures(NVENCEncoder* enc);

// ============================================================================
// Public API
// ============================================================================

BOOL NVENCEncoder_IsAvailable(void) {
    HMODULE lib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!lib) lib = LoadLibraryA("nvEncodeAPI.dll");
    if (lib) {
        FreeLibrary(lib);
        return TRUE;
    }
    return FALSE;
}

NVENCEncoder* NVENCEncoder_Create(ID3D11Device* d3dDevice, int width, int height, int fps, QualityPreset quality) {
    if (!d3dDevice || width <= 0 || height <= 0 || fps <= 0) {
        NvLog("NVENCEncoder: Invalid parameters\n");
        return NULL;
    }
    
    NvLog("Creating NVENCEncoder (%dx%d @ %d fps, quality=%d)...\n", width, height, fps, quality);
    
    NVENCEncoder* enc = (NVENCEncoder*)calloc(1, sizeof(NVENCEncoder));
    if (!enc) return NULL;
    
    enc->srcDevice = d3dDevice;  // Keep reference to source device
    d3dDevice->lpVtbl->GetImmediateContext(d3dDevice, &enc->srcContext);  // Get source context
    enc->width = width;
    enc->height = height;
    enc->fps = fps;
    enc->frameDuration = 10000000ULL / fps;
    
    InitializeCriticalSection(&enc->submitLock);
    
    // ========================================================================
    // Step 1: Create SEPARATE D3D11 device for NVENC
    // Per NVIDIA docs: Desktop Duplication + NVENC on same device causes
    // thread contention. Using separate device eliminates this issue.
    // ========================================================================
    
    {
        // Get the adapter from source device to create encoder device on same GPU
        IDXGIDevice* dxgiDevice = NULL;
        IDXGIAdapter* adapter = NULL;
        HRESULT hr = d3dDevice->lpVtbl->QueryInterface(d3dDevice, &IID_IDXGIDevice, (void**)&dxgiDevice);
        if (SUCCEEDED(hr)) {
            hr = dxgiDevice->lpVtbl->GetAdapter(dxgiDevice, &adapter);
            dxgiDevice->lpVtbl->Release(dxgiDevice);
        }
        
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL featureLevel;
        
        // Create encoder's own D3D11 device on the same adapter
        hr = D3D11CreateDevice(
            adapter,  // Use same GPU as source
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            NULL,
            0,  // No special flags needed
            featureLevels,
            1,
            D3D11_SDK_VERSION,
            &enc->encDevice,
            &featureLevel,
            &enc->encContext
        );
        
        if (adapter) adapter->lpVtbl->Release(adapter);
        
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: Failed to create encoder D3D11 device (0x%08X)\n", hr);
            goto fail;
        }
        NvLog("NVENCEncoder: Created separate D3D11 device for encoding\n");
    }
    
    // ========================================================================
    // Step 2: Load nvEncodeAPI64.dll and get function list
    // ========================================================================
    
    enc->nvencLib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!enc->nvencLib) {
        enc->nvencLib = LoadLibraryA("nvEncodeAPI.dll");
    }
    if (!enc->nvencLib) {
        NvLog("NVENCEncoder: Failed to load nvEncodeAPI64.dll\n");
        goto fail;
    }
    
    typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
    PFN_NvEncodeAPICreateInstance createInstance = 
        (PFN_NvEncodeAPICreateInstance)GetProcAddress(enc->nvencLib, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        NvLog("NVENCEncoder: NvEncodeAPICreateInstance not found\n");
        goto fail;
    }
    
    enc->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = createInstance(&enc->fn);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENCEncoder: CreateInstance failed (%d)\n", st);
        goto fail;
    }
    
    // ========================================================================
    // Step 3: Open encode session with ENCODER's D3D11 device (not source)
    // ========================================================================
    
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {0};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device = enc->encDevice;  // Use encoder's own device!
    sessionParams.apiVersion = NVENCAPI_VERSION;
    
    st = enc->fn.nvEncOpenEncodeSessionEx(&sessionParams, &enc->encoder);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENCEncoder: OpenEncodeSessionEx failed (%d)\n", st);
        goto fail;
    }
    
    // Log SDK version
    NvLog("NVENCEncoder: SDK API version %d.%d\n",
          NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    
    // Check async support - required for this implementation
    NV_ENC_CAPS_PARAM capsParam = {0};
    capsParam.version = NV_ENC_CAPS_PARAM_VER;
    capsParam.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
    int capsVal = 0;
    enc->fn.nvEncGetEncodeCaps(enc->encoder, NV_ENC_CODEC_HEVC_GUID, &capsParam, &capsVal);
    if (!capsVal) {
        NvLog("NVENCEncoder: Async mode not supported - this GPU/driver does not support async encoding\n");
        goto fail;
    }
    
    // ========================================================================
    // Step 3: Get preset config and configure encoder
    // Per API: Use NvEncGetEncodePresetConfigEx to get base config
    // ========================================================================
    
    NV_ENC_PRESET_CONFIG presetConfig = {0};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    
    st = enc->fn.nvEncGetEncodePresetConfigEx(enc->encoder, 
        NV_ENC_CODEC_HEVC_GUID, 
        NV_ENC_PRESET_P1_GUID,  // Fastest preset
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
        &presetConfig);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENCEncoder: GetEncodePresetConfigEx failed (%d)\n", st);
        goto fail;
    }
    
    // Customize config for screen recording
    NV_ENC_CONFIG config = presetConfig.presetCfg;
    config.gopLength = fps * 2;  // 2-second GOP for seeking
    config.frameIntervalP = 1;   // No B-frames (confirmed by user)
    
    // Disable expensive features for maximum speed
    config.rcParams.enableAQ = 0;
    config.rcParams.enableTemporalAQ = 0;
    config.rcParams.enableLookahead = 0;
    config.rcParams.lookaheadDepth = 0;
    config.rcParams.disableBadapt = 1;
    config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    
    // HEVC: Disable temporal filter, minimal references
    config.encodeCodecConfig.hevcConfig.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
    config.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 2;
    
    // Constant QP mode (fastest, no rate control overhead)
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    switch (quality) {
        case QUALITY_LOW:      enc->qp = 28; break;
        case QUALITY_MEDIUM:   enc->qp = 24; break;
        case QUALITY_HIGH:     enc->qp = 20; break;
        case QUALITY_LOSSLESS: enc->qp = 16; break;
        default:               enc->qp = 24; break;
    }
    config.rcParams.constQP.qpInterP = enc->qp;
    config.rcParams.constQP.qpInterB = enc->qp;
    config.rcParams.constQP.qpIntra = enc->qp > 4 ? enc->qp - 4 : 1;
    
    // ========================================================================
    // Step 4: Initialize encoder
    // Per API (line 2240): enableEncodeAsync=1 for async mode
    // ========================================================================
    
    NV_ENC_INITIALIZE_PARAMS initParams = {0};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams.encodeWidth = width;
    initParams.encodeHeight = height;
    initParams.darWidth = width;
    initParams.darHeight = height;
    initParams.frameRateNum = fps;
    initParams.frameRateDen = 1;
    initParams.enableEncodeAsync = 1;  // Async mode required
    initParams.enablePTD = 1;  // Let NVENC decide picture types
    initParams.encodeConfig = &config;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    
    st = enc->fn.nvEncInitializeEncoder(enc->encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENCEncoder: Initialize failed (%d)\n", st);
        goto fail;
    }
    
    NvLog("NVENCEncoder: HEVC CQP (QP=%d), async mode\n", enc->qp);
    
    // ========================================================================
    // Step 5: Create input textures (one per buffer slot)
    // Per API: Each in-flight frame needs its own input buffer
    // ========================================================================
    
    if (!CreateInputTextures(enc)) {
        NvLog("NVENCEncoder: Failed to create input textures\n");
        goto fail;
    }
    
    // ========================================================================
    // Step 6: Create output bitstream buffers and completion events
    // Per API (line 2582): Each output buffer needs distinct completion event
    // ========================================================================
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        // Create bitstream buffer
        NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamParams = {0};
        createBitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        
        st = enc->fn.nvEncCreateBitstreamBuffer(enc->encoder, &createBitstreamParams);
        if (st != NV_ENC_SUCCESS) {
            NvLog("NVENCEncoder: CreateBitstreamBuffer[%d] failed (%d)\n", i, st);
            goto fail;
        }
        enc->outputBuffers[i] = createBitstreamParams.bitstreamBuffer;
        
        // Create and register completion event
        enc->completionEvents[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!enc->completionEvents[i]) {
            NvLog("NVENCEncoder: CreateEvent[%d] failed\n", i);
            goto fail;
        }
        
        NV_ENC_EVENT_PARAMS eventParams = {0};
        eventParams.version = NV_ENC_EVENT_PARAMS_VER;
        eventParams.completionEvent = enc->completionEvents[i];
        
        st = enc->fn.nvEncRegisterAsyncEvent(enc->encoder, &eventParams);
        if (st != NV_ENC_SUCCESS) {
            NvLog("NVENCEncoder: RegisterAsyncEvent[%d] failed (%d)\n", i, st);
            CloseHandle(enc->completionEvents[i]);
            enc->completionEvents[i] = NULL;
            goto fail;
        }
    }
    
    // ========================================================================
    // Step 7: Start output thread
    // Per API (lines 3384-3388): "The client can create another thread and 
    // wait on the event object to be signaled"
    // ========================================================================
    
    enc->stopThread = FALSE;
    enc->outputThread = (HANDLE)_beginthreadex(NULL, 0, OutputThreadProc, enc, 0, NULL);
    if (!enc->outputThread) {
        NvLog("NVENCEncoder: Failed to create output thread\n");
        goto fail;
    }
    
    enc->initialized = TRUE;
    NvLog("NVENCEncoder: Ready (%d buffers, async)\n", NUM_BUFFERS);
    return enc;
    
fail:
    NVENCEncoder_Destroy(enc);
    return NULL;
}

void NVENCEncoder_SetCallback(NVENCEncoder* enc, EncodedFrameCallback callback, void* userData) {
    if (!enc) return;
    enc->frameCallback = callback;
    enc->callbackUserData = userData;
}

// Returns: 1 = success, 0 = transient failure (retry), -1 = device lost (must recreate)
int NVENCEncoder_SubmitTexture(NVENCEncoder* enc, ID3D11Texture2D* nv12Source, LONGLONG timestamp) {
    if (!enc || !enc->initialized || !nv12Source) return 0;
    
    // Check if device was previously marked as lost
    if (enc->deviceLost) return -1;
    
    // Debug: Log entry every 1000 frames to track if we're still entering
    static LONG submitAttempts = 0;
    LONG attempt = InterlockedIncrement(&submitAttempts);
    if (attempt % 1000 == 1) {
        NvLog("NVENCEncoder: [DEBUG] SubmitTexture entry #%d, pending=%d\n", attempt, enc->pendingCount);
    }
    
    EnterCriticalSection(&enc->submitLock);
    
    // Check if pipeline is full
    if (enc->pendingCount >= NUM_BUFFERS) {
        LeaveCriticalSection(&enc->submitLock);
        // Rate-limit this log to avoid spam (log once per 100 occurrences)
        enc->pipelineFullCount++;
        if ((enc->pipelineFullCount % 100) == 1) {
            NvLog("NVENCEncoder: Pipeline full (%d pending) - frame dropped\n", enc->pendingCount);
        }
        return 0;  // Transient - output thread will drain
    }
    
    int idx = enc->submitIndex;
    NVENCSTATUS st;
    HRESULT hr;
    
    // ========================================================================
    // Step 1: Acquire keyed mutex on SOURCE device for writing
    // Key 0 = available, Key 1 = encoder owns it
    // ========================================================================
    hr = enc->srcMutex[idx]->lpVtbl->AcquireSync(enc->srcMutex[idx], 0, 100);
    
    // Check for device removed/reset errors
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        enc->deviceLost = TRUE;
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: DEVICE LOST detected on srcMutex acquire (0x%08X)\n", hr);
        return -1;
    }
    
    if (hr == WAIT_TIMEOUT || FAILED(hr)) {
        LeaveCriticalSection(&enc->submitLock);
        enc->mutexTimeoutCount++;
        if ((enc->mutexTimeoutCount % 100) == 1) {
            NvLog("NVENCEncoder: Mutex acquire timeout[%d] (0x%08X)\n", idx, hr);
        }
        return 0;  // Transient
    }
    
    // ========================================================================
    // Step 2: Copy source texture to staging texture (on SOURCE device)
    // Using SOURCE device context - this is safe, runs on capture thread
    // ========================================================================
    enc->srcContext->lpVtbl->CopyResource(enc->srcContext, 
                                           (ID3D11Resource*)enc->stagingTextures[idx], 
                                           (ID3D11Resource*)nv12Source);
    
    // Flush after copy to ensure GPU work completes before mutex release
    // This prevents race conditions between source and encoder devices
    enc->srcContext->lpVtbl->Flush(enc->srcContext);
    
    // Check for device removed after GPU operation
    hr = enc->srcDevice->lpVtbl->GetDeviceRemovedReason(enc->srcDevice);
    if (hr != S_OK) {
        enc->deviceLost = TRUE;
        enc->srcMutex[idx]->lpVtbl->ReleaseSync(enc->srcMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: DEVICE LOST after CopyResource (0x%08X)\n", hr);
        return -1;
    }
    
    // ========================================================================
    // Step 3: Release mutex with key 1 (signaling encoder can use it)
    // ========================================================================
    enc->srcMutex[idx]->lpVtbl->ReleaseSync(enc->srcMutex[idx], 1);
    
    // ========================================================================
    // Step 4: Acquire mutex on ENCODER device for encoding
    // ========================================================================
    hr = enc->encMutex[idx]->lpVtbl->AcquireSync(enc->encMutex[idx], 1, 100);
    
    // Check for device removed/reset errors
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        enc->deviceLost = TRUE;
        enc->srcMutex[idx]->lpVtbl->ReleaseSync(enc->srcMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: DEVICE LOST detected on encMutex acquire (0x%08X)\n", hr);
        return -1;
    }
    
    if (hr == WAIT_TIMEOUT || FAILED(hr)) {
        // Try to recover by releasing back to state 0
        enc->srcMutex[idx]->lpVtbl->ReleaseSync(enc->srcMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: Encoder mutex acquire failed[%d]\n", idx);
        return 0;  // Transient
    }
    
    // ========================================================================
    // Step 5: Map the registered resource (on ENCODER device)
    // ========================================================================
    NV_ENC_MAP_INPUT_RESOURCE mapParams = {0};
    mapParams.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapParams.registeredResource = enc->registeredResources[idx];
    
    st = enc->fn.nvEncMapInputResource(enc->encoder, &mapParams);
    
    // Check for NVENC device errors
    if (st == NV_ENC_ERR_DEVICE_NOT_EXIST || st == NV_ENC_ERR_INVALID_DEVICE || 
        st == NV_ENC_ERR_INVALID_ENCODERDEVICE) {
        enc->deviceLost = TRUE;
        enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: DEVICE LOST on MapInputResource (%d)\n", st);
        return -1;
    }
    
    if (st != NV_ENC_SUCCESS) {
        enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: MapInputResource[%d] failed (%d)\n", idx, st);
        return 0;  // Transient
    }
    
    // Store for later unmap (per API lines 4165-4167: unmap after LockBitstream)
    enc->mappedResources[idx] = mapParams.mappedResource;
    
    // ========================================================================
    // Step 6: Submit frame for encoding
    // Per API: In async mode, this returns immediately
    // ========================================================================
    
    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = mapParams.mappedResource;
    picParams.outputBitstream = enc->outputBuffers[idx];
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picParams.inputWidth = enc->width;
    picParams.inputHeight = enc->height;
    picParams.inputPitch = 0;  // Let NVENC determine from texture
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputTimeStamp = timestamp;
    picParams.inputDuration = enc->frameDuration;
    picParams.completionEvent = enc->completionEvents[idx];
    
    // Force IDR every 2 seconds for seeking
    if (enc->frameNumber % (enc->fps * 2) == 0) {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    }
    
    st = enc->fn.nvEncEncodePicture(enc->encoder, &picParams);
    
    // Check for NVENC device errors
    if (st == NV_ENC_ERR_DEVICE_NOT_EXIST || st == NV_ENC_ERR_INVALID_DEVICE || 
        st == NV_ENC_ERR_INVALID_ENCODERDEVICE) {
        enc->deviceLost = TRUE;
        enc->fn.nvEncUnmapInputResource(enc->encoder, mapParams.mappedResource);
        enc->mappedResources[idx] = NULL;
        enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: DEVICE LOST on EncodePicture (%d)\n", st);
        return -1;
    }
    
    // Per API: NV_ENC_ERR_NEED_MORE_INPUT is not an error (B-frame buffering)
    // But we don't use B-frames, so this shouldn't happen
    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        enc->fn.nvEncUnmapInputResource(enc->encoder, mapParams.mappedResource);
        enc->mappedResources[idx] = NULL;
        // Release mutex back to state 0 on error
        enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
        LeaveCriticalSection(&enc->submitLock);
        NvLog("NVENCEncoder: EncodePicture[%d] failed (%d)\n", idx, st);
        return 0;  // Transient
    }
    
    // NOTE: encMutex[idx] is still held! The output thread will release it
    // after LockBitstream and UnmapInputResource complete.
    
    // Track pending frame
    enc->pendingTimestamps[idx] = timestamp;
    enc->submitIndex = (enc->submitIndex + 1) % NUM_BUFFERS;
    InterlockedIncrement(&enc->pendingCount);
    enc->frameNumber++;
    
    LeaveCriticalSection(&enc->submitLock);
    
    return 1;  // Success
}

BOOL NVENCEncoder_IsDeviceLost(NVENCEncoder* enc) {
    return enc ? enc->deviceLost : FALSE;
}

int NVENCEncoder_DrainCompleted(NVENCEncoder* enc, EncodedFrameCallback callback, void* userData) {
    // In async mode, the output thread handles frame retrieval
    // This function is a no-op as draining happens automatically via callback
    (void)enc;
    (void)callback;
    (void)userData;
    return 0;
}

BOOL NVENCEncoder_EncodeTexture(NVENCEncoder* enc, ID3D11Texture2D* nv12Tex, LONGLONG ts, EncodedFrame* out) {
    // Legacy API - wraps new API for compatibility
    if (!enc || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    
    // Just submit - async mode will deliver via callback
    return NVENCEncoder_SubmitTexture(enc, nv12Tex, ts);
}

BOOL NVENCEncoder_Flush(NVENCEncoder* enc, EncodedFrame* out) {
    if (!enc || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    
    // Send EOS
    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    enc->fn.nvEncEncodePicture(enc->encoder, &picParams);
    
    return FALSE;
}

BOOL NVENCEncoder_GetSequenceHeader(NVENCEncoder* enc, BYTE* buffer, DWORD bufferSize, DWORD* outSize) {
    if (!enc || !enc->initialized || !buffer || !outSize) return FALSE;
    
    uint32_t payloadSize = 0;
    
    NV_ENC_SEQUENCE_PARAM_PAYLOAD seqParams = {0};
    seqParams.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    seqParams.inBufferSize = bufferSize;
    seqParams.spsppsBuffer = buffer;
    seqParams.outSPSPPSPayloadSize = &payloadSize;
    
    NVENCSTATUS st = enc->fn.nvEncGetSequenceParams(enc->encoder, &seqParams);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENCEncoder: GetSequenceParams failed (%d)\n", st);
        return FALSE;
    }
    
    *outSize = payloadSize;
    NvLog("NVENCEncoder: Sequence header size: %u bytes\n", payloadSize);
    return TRUE;
}

void NVENCEncoder_GetStats(NVENCEncoder* enc, int* framesEncoded, double* avgEncodeTimeMs) {
    if (!enc) return;
    if (framesEncoded) *framesEncoded = (int)enc->frameNumber;
    if (avgEncodeTimeMs) *avgEncodeTimeMs = 0.0;
}

void NVENCEncoder_Destroy(NVENCEncoder* enc) {
    if (!enc) return;
    
    NvLog("NVENCEncoder: Destroy (%llu frames)\n", enc->frameNumber);
    
    // Stop output thread
    if (enc->outputThread) {
        enc->stopThread = TRUE;
        
        // Signal all events to wake up thread
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (enc->completionEvents[i]) {
                SetEvent(enc->completionEvents[i]);
            }
        }
        
        WaitForSingleObject(enc->outputThread, 5000);
        CloseHandle(enc->outputThread);
    }
    
    // Cleanup NVENC resources
    if (enc->encoder) {
        // Flush D3D11 contexts to ensure all pending GPU work is complete
        // This is the D3D11 equivalent of cuCtxSynchronize() recommended by NVIDIA
        // to prevent NVENC API calls from hanging
        if (enc->encContext) {
            NvLog("NVENCEncoder: Flushing encoder D3D11 context before cleanup\n");
            enc->encContext->lpVtbl->Flush(enc->encContext);
        }
        if (enc->srcContext) {
            NvLog("NVENCEncoder: Flushing source D3D11 context before cleanup\n");
            enc->srcContext->lpVtbl->Flush(enc->srcContext);
        }
        
        // Send EOS
        NV_ENC_PIC_PARAMS picParams = {0};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        enc->fn.nvEncEncodePicture(enc->encoder, &picParams);
        
        // Unregister events and destroy buffers
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (enc->completionEvents[i]) {
                NV_ENC_EVENT_PARAMS eventParams = {0};
                eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                eventParams.completionEvent = enc->completionEvents[i];
                enc->fn.nvEncUnregisterAsyncEvent(enc->encoder, &eventParams);
                CloseHandle(enc->completionEvents[i]);
            }
            
            if (enc->outputBuffers[i]) {
                enc->fn.nvEncDestroyBitstreamBuffer(enc->encoder, enc->outputBuffers[i]);
            }
        }
        
        // Destroy input textures (includes shared textures and mutexes)
        DestroyInputTextures(enc);
        
        // Destroy encoder
        enc->fn.nvEncDestroyEncoder(enc->encoder);
    }
    
    // Release encoder's D3D11 device and context
    if (enc->encContext) {
        enc->encContext->lpVtbl->Release(enc->encContext);
    }
    if (enc->encDevice) {
        enc->encDevice->lpVtbl->Release(enc->encDevice);
    }
    
    // Release source context (we didn't AddRef the source device)
    if (enc->srcContext) {
        enc->srcContext->lpVtbl->Release(enc->srcContext);
    }
    
    if (enc->nvencLib) {
        FreeLibrary(enc->nvencLib);
    }
    
    DeleteCriticalSection(&enc->submitLock);
    free(enc);
}

// ============================================================================
// Output Thread (Per API lines 3384-3388)
// "The client can create another thread and wait on the event object to be 
// signaled by NvEncodeAPI interface on completion of the encoding process"
// ============================================================================

static unsigned __stdcall OutputThreadProc(void* param) {
    NVENCEncoder* enc = (NVENCEncoder*)param;
    
    NvLog("NVENCEncoder: Output thread started\n");
    
    int framesRetrieved = 0;
    
    // Per NVIDIA docs (lines 3701-3704):
    // "Waits on E1, copies encoded bitstream from O1
    //  Waits on E2, copies encoded bitstream from O2
    //  Waits on E3, copies encoded bitstream from O3"
    // We wait on events IN ORDER (retrieveIndex), one at a time.
    
    while (!enc->stopThread) {
        // Heartbeat for monitoring
        Logger_Heartbeat(THREAD_NVENC_OUTPUT);
        
        int idx = enc->retrieveIndex;
        
        // ====================================================================
        // Step 1: Wait for completion event for this specific slot
        // Per docs: "wait on the event object to be signaled"
        // ====================================================================
        
        DWORD waitResult = WaitForSingleObject(enc->completionEvents[idx], 100);
        
        if (waitResult == WAIT_TIMEOUT) {
            // No frame ready yet - check if we should stop, then wait again
            continue;
        }
        
        if (waitResult != WAIT_OBJECT_0) {
            if (!enc->stopThread) {
                NvLog("NVENCEncoder: Wait[%d] failed (0x%X)\n", idx, waitResult);
            }
            continue;
        }
        
        if (enc->stopThread) break;
        
        // ====================================================================
        // CHECK DEVICE HEALTH before any GPU/NVENC operations
        // This prevents hanging on dead device calls
        // ====================================================================
        HRESULT deviceCheck = enc->encDevice->lpVtbl->GetDeviceRemovedReason(enc->encDevice);
        if (deviceCheck != S_OK) {
            enc->deviceLost = TRUE;
            NvLog("NVENCEncoder: DEVICE REMOVED detected in output thread (0x%08X)\n", deviceCheck);
            break;  // Exit loop immediately
        }
        
        // ====================================================================
        // Step 2: Lock bitstream to get encoded data
        // Per docs (lines 3623-3626): event signaled means data is ready
        // ====================================================================
        
        NvLog("NVENCEncoder: [DEBUG] Calling LockBitstream[%d]...\n", idx);
        
        NV_ENC_LOCK_BITSTREAM lockParams = {0};
        lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockParams.outputBitstream = enc->outputBuffers[idx];
        lockParams.doNotWait = 1;  // Don't block - event already signaled
        
        NVENCSTATUS st = enc->fn.nvEncLockBitstream(enc->encoder, &lockParams);
        
        NvLog("NVENCEncoder: [DEBUG] LockBitstream[%d] returned %d\n", idx, st);
        
        // Check for device lost errors
        if (st == NV_ENC_ERR_DEVICE_NOT_EXIST || st == NV_ENC_ERR_INVALID_DEVICE || 
            st == NV_ENC_ERR_INVALID_ENCODERDEVICE) {
            enc->deviceLost = TRUE;
            NvLog("NVENCEncoder: DEVICE LOST in OutputThread LockBitstream (%d)\n", st);
            // Release mutex and exit thread - consumer will detect deviceLost
            if (enc->encMutex[idx]) {
                enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
            }
            break;  // Exit the while loop
        }
        
        if (st != NV_ENC_SUCCESS) {
            NvLog("NVENCEncoder: LockBitstream[%d] failed (%d)\n", idx, st);
            // Still advance to avoid getting stuck
            if (enc->encMutex[idx]) {
                enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
            }
            enc->retrieveIndex = (enc->retrieveIndex + 1) % NUM_BUFFERS;
            InterlockedDecrement(&enc->pendingCount);
            continue;
        }
        
        // ====================================================================
        // Step 3: Copy encoded data
        // ====================================================================
        
        EncodedFrame frame = {0};
        frame.data = (BYTE*)malloc(lockParams.bitstreamSizeInBytes);
        if (frame.data) {
            memcpy(frame.data, lockParams.bitstreamBufferPtr, lockParams.bitstreamSizeInBytes);
            frame.size = lockParams.bitstreamSizeInBytes;
            frame.timestamp = enc->pendingTimestamps[idx];
            frame.duration = enc->frameDuration;
            frame.isKeyframe = (lockParams.pictureType == NV_ENC_PIC_TYPE_IDR);
        }
        
        // Unlock bitstream
        NvLog("NVENCEncoder: [DEBUG] Calling UnlockBitstream[%d]...\n", idx);
        enc->fn.nvEncUnlockBitstream(enc->encoder, enc->outputBuffers[idx]);
        NvLog("NVENCEncoder: [DEBUG] UnlockBitstream[%d] done\n", idx);
        
        // ====================================================================
        // Step 4: Unmap input resource
        // Per docs (lines 4165-4167): unmap AFTER LockBitstream
        // Flush D3D11 context before unmap to ensure GPU work is complete
        // (NVIDIA forum recommendation to prevent nvEncUnmapInputResource hangs)
        // ====================================================================
        
        if (enc->mappedResources[idx]) {
            // Flush encoder context before unmap to sync GPU state
            if (enc->encContext) {
                enc->encContext->lpVtbl->Flush(enc->encContext);
            }
            NvLog("NVENCEncoder: [DEBUG] Calling UnmapInputResource[%d]...\n", idx);
            enc->fn.nvEncUnmapInputResource(enc->encoder, enc->mappedResources[idx]);
            NvLog("NVENCEncoder: [DEBUG] UnmapInputResource[%d] done\n", idx);
            enc->mappedResources[idx] = NULL;
        }
        
        // ====================================================================
        // Step 5: Release keyed mutex - allows source device to reuse buffer
        // Release with key 0 so source can acquire it again
        // ====================================================================
        
        if (enc->encMutex[idx]) {
            NvLog("NVENCEncoder: [DEBUG] ReleaseSync[%d]...\n", idx);
            enc->encMutex[idx]->lpVtbl->ReleaseSync(enc->encMutex[idx], 0);
            NvLog("NVENCEncoder: [DEBUG] ReleaseSync[%d] done\n", idx);
        }
        
        // ====================================================================
        // Step 6: Deliver frame and advance
        // ====================================================================
        
        if (frame.data && enc->frameCallback) {
            enc->frameCallback(&frame, enc->callbackUserData);
            framesRetrieved++;
        } else if (frame.data) {
            free(frame.data);
        }
        
        enc->retrieveIndex = (enc->retrieveIndex + 1) % NUM_BUFFERS;
        InterlockedDecrement(&enc->pendingCount);
    }
    
    NvLog("NVENCEncoder: Output thread exiting (retrieved %d frames)\n", framesRetrieved);
    return 0;
}

// ============================================================================
// Input Texture Management - Cross-Device Shared Textures
// Per API: Each in-flight frame needs its own input buffer
// 
// We use shared textures with keyed mutex for synchronization:
// 1. Create staging texture on SOURCE device with SHARED_KEYEDMUTEX flag
// 2. Open that texture on ENCODER device via shared handle
// 3. Use keyed mutex to synchronize copy (source) and encode (encoder)
// ============================================================================

static BOOL CreateInputTextures(NVENCEncoder* enc) {
    HRESULT hr;
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        // ====================================================================
        // Step 1: Create NV12 shared texture on SOURCE device
        // ====================================================================
        D3D11_TEXTURE2D_DESC stagingDesc = {0};
        stagingDesc.Width = enc->width;
        stagingDesc.Height = enc->height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_NV12;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_DEFAULT;
        stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // Needed for shared
        stagingDesc.CPUAccessFlags = 0;
        stagingDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        
        hr = enc->srcDevice->lpVtbl->CreateTexture2D(enc->srcDevice, &stagingDesc, 
                                                      NULL, &enc->stagingTextures[i]);
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: CreateTexture2D staging[%d] failed (0x%08X)\n", i, hr);
            return FALSE;
        }
        
        // Get keyed mutex for source-side texture
        hr = enc->stagingTextures[i]->lpVtbl->QueryInterface(
            enc->stagingTextures[i], &IID_IDXGIKeyedMutex, (void**)&enc->srcMutex[i]);
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: QueryInterface IDXGIKeyedMutex src[%d] failed\n", i);
            return FALSE;
        }
        
        // Get shared handle
        IDXGIResource* dxgiRes = NULL;
        hr = enc->stagingTextures[i]->lpVtbl->QueryInterface(
            enc->stagingTextures[i], &IID_IDXGIResource, (void**)&dxgiRes);
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: QueryInterface IDXGIResource[%d] failed\n", i);
            return FALSE;
        }
        hr = dxgiRes->lpVtbl->GetSharedHandle(dxgiRes, &enc->sharedHandles[i]);
        dxgiRes->lpVtbl->Release(dxgiRes);
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: GetSharedHandle[%d] failed\n", i);
            return FALSE;
        }
        
        // ====================================================================
        // Step 2: Open shared texture on ENCODER device
        // ====================================================================
        hr = enc->encDevice->lpVtbl->OpenSharedResource(
            enc->encDevice, enc->sharedHandles[i], &IID_ID3D11Texture2D, 
            (void**)&enc->inputTextures[i]);
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: OpenSharedResource[%d] failed (0x%08X)\n", i, hr);
            return FALSE;
        }
        
        // Get keyed mutex for encoder-side texture
        hr = enc->inputTextures[i]->lpVtbl->QueryInterface(
            enc->inputTextures[i], &IID_IDXGIKeyedMutex, (void**)&enc->encMutex[i]);
        if (FAILED(hr)) {
            NvLog("NVENCEncoder: QueryInterface IDXGIKeyedMutex enc[%d] failed\n", i);
            return FALSE;
        }
        
        // ====================================================================
        // Step 3: Register encoder-side texture with NVENC
        // ====================================================================
        NV_ENC_REGISTER_RESOURCE regParams = {0};
        regParams.version = NV_ENC_REGISTER_RESOURCE_VER;
        regParams.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        regParams.width = enc->width;
        regParams.height = enc->height;
        regParams.pitch = 0;
        regParams.subResourceIndex = 0;
        regParams.resourceToRegister = enc->inputTextures[i];
        regParams.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        regParams.bufferUsage = NV_ENC_INPUT_IMAGE;
        
        NVENCSTATUS st = enc->fn.nvEncRegisterResource(enc->encoder, &regParams);
        if (st != NV_ENC_SUCCESS) {
            NvLog("NVENCEncoder: RegisterResource[%d] failed (%d)\n", i, st);
            return FALSE;
        }
        enc->registeredResources[i] = regParams.registeredResource;
    }
    
    NvLog("NVENCEncoder: Created %d shared textures (%dx%d NV12, cross-device)\n", 
          NUM_BUFFERS, enc->width, enc->height);
    return TRUE;
}

static void DestroyInputTextures(NVENCEncoder* enc) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (enc->mappedResources[i]) {
            enc->fn.nvEncUnmapInputResource(enc->encoder, enc->mappedResources[i]);
            enc->mappedResources[i] = NULL;
        }
        
        if (enc->registeredResources[i]) {
            enc->fn.nvEncUnregisterResource(enc->encoder, enc->registeredResources[i]);
            enc->registeredResources[i] = NULL;
        }
        
        // Release encoder-side mutex and texture
        if (enc->encMutex[i]) {
            enc->encMutex[i]->lpVtbl->Release(enc->encMutex[i]);
            enc->encMutex[i] = NULL;
        }
        if (enc->inputTextures[i]) {
            enc->inputTextures[i]->lpVtbl->Release(enc->inputTextures[i]);
            enc->inputTextures[i] = NULL;
        }
        
        // Release source-side mutex and texture
        if (enc->srcMutex[i]) {
            enc->srcMutex[i]->lpVtbl->Release(enc->srcMutex[i]);
            enc->srcMutex[i] = NULL;
        }
        if (enc->stagingTextures[i]) {
            enc->stagingTextures[i]->lpVtbl->Release(enc->stagingTextures[i]);
            enc->stagingTextures[i] = NULL;
        }
        
        enc->sharedHandles[i] = NULL;
    }
}

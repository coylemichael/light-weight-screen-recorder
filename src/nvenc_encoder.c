/*
 * NVENC Hardware Encoder - CUDA Path
 * Based directly on OBS nvenc-cuda.c and cuda-helpers.c
 * 
 * Flow:
 * 1. Load nvcuda.dll, get function pointers
 * 2. Create CUDA context (cuCtxCreate)
 * 3. Create CUDA arrays for input surfaces (cuArray3DCreate)
 * 4. Open NVENC session with CUDA device type
 * 5. For each frame: CPU buffer → CUDA array (cuMemcpy2D) → NVENC encode
 */

#include "nvenc_encoder.h"
#include "logger.h"
#include "constants.h"
#include "leak_tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include official NVIDIA header
#include "nvEncodeAPI.h"

#define NvLog Logger_Log

// ============================================================================
// CUDA Types and Function Pointers (from OBS cuda-helpers.c)
// ============================================================================

typedef int CUresult;
typedef void* CUcontext;
typedef void* CUarray;
typedef int CUdevice;

#define CUDA_SUCCESS 0

// CUDA array descriptor (from cuda.h)
typedef struct {
    size_t Width;
    size_t Height;
    size_t Depth;
    unsigned int Format;
    unsigned int NumChannels;
    unsigned int Flags;
} CUDA_ARRAY3D_DESCRIPTOR;

// CUDA memcpy descriptor - MUST match CUDA driver API exactly
typedef unsigned long long CUdeviceptr;
typedef enum {
    CU_MEMORYTYPE_HOST = 1,
    CU_MEMORYTYPE_DEVICE = 2,
    CU_MEMORYTYPE_ARRAY = 3
} CUmemorytype;

typedef struct {
    size_t srcXInBytes;
    size_t srcY;
    CUmemorytype srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    
    size_t dstXInBytes;
    size_t dstY;
    CUmemorytype dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    
    size_t WidthInBytes;
    size_t Height;
} CUDA_MEMCPY2D;

#define CU_AD_FORMAT_UNSIGNED_INT8 1
#define CUDA_ARRAY3D_SURFACE_LDST 0x02

// CUDA function pointer types
typedef CUresult (*CU_INIT)(unsigned int);
typedef CUresult (*CU_DEVICE_GET_COUNT)(int*);
typedef CUresult (*CU_DEVICE_GET)(CUdevice*, int);
typedef CUresult (*CU_CTX_CREATE)(CUcontext*, unsigned int, CUdevice);
typedef CUresult (*CU_CTX_DESTROY)(CUcontext);
typedef CUresult (*CU_CTX_PUSH_CURRENT)(CUcontext);
typedef CUresult (*CU_CTX_POP_CURRENT)(CUcontext*);
typedef CUresult (*CU_ARRAY3D_CREATE)(CUarray*, const CUDA_ARRAY3D_DESCRIPTOR*);
typedef CUresult (*CU_ARRAY_DESTROY)(CUarray);
typedef CUresult (*CU_MEMCPY2D)(const CUDA_MEMCPY2D*);
typedef CUresult (*CU_MEM_HOST_REGISTER)(void*, size_t, unsigned int);
typedef CUresult (*CU_MEM_HOST_UNREGISTER)(void*);
typedef CUresult (*CU_GET_ERROR_NAME)(CUresult, const char**);
typedef CUresult (*CU_GET_ERROR_STRING)(CUresult, const char**);

// CUDA functions struct (like OBS CudaFunctions)
typedef struct {
    CU_INIT cuInit;
    CU_DEVICE_GET_COUNT cuDeviceGetCount;
    CU_DEVICE_GET cuDeviceGet;
    CU_CTX_CREATE cuCtxCreate;
    CU_CTX_DESTROY cuCtxDestroy;
    CU_CTX_PUSH_CURRENT cuCtxPushCurrent;
    CU_CTX_POP_CURRENT cuCtxPopCurrent;
    CU_ARRAY3D_CREATE cuArray3DCreate;
    CU_ARRAY_DESTROY cuArrayDestroy;
    CU_MEMCPY2D cuMemcpy2D;
    CU_MEM_HOST_REGISTER cuMemHostRegister;
    CU_MEM_HOST_UNREGISTER cuMemHostUnregister;
    CU_GET_ERROR_NAME cuGetErrorName;
    CU_GET_ERROR_STRING cuGetErrorString;
} CudaFunctions;

static HMODULE g_cudaLib = NULL;
static CudaFunctions* cu = NULL;

// ============================================================================
// CUDA Surface (from OBS)
// ============================================================================

typedef struct {
    CUarray tex;
    NV_ENC_REGISTERED_PTR res;
    NV_ENC_INPUT_PTR mapped_res;
} CudaSurface;

// ============================================================================
// Constants
// ============================================================================

#define NUM_BUFFERS 4
#define GOP_LENGTH_SECONDS 2
#define MF_UNITS_PER_SECOND 10000000LL

// ============================================================================
// Encoder State
// ============================================================================

struct NVENCEncoder {
    // NVENC
    HMODULE nvencLib;
    NV_ENCODE_API_FUNCTION_LIST fn;
    void* encoder;
    
    // CUDA
    CUcontext cu_ctx;
    
    // Surfaces (like OBS enc->surfaces)
    CudaSurface surfaces[NUM_BUFFERS];
    int buf_count;
    int next_bitstream;
    
    // Output bitstream buffers
    NV_ENC_OUTPUT_PTR outputBuffers[NUM_BUFFERS];
    
    // Dimensions
    int width;
    int height;
    int fps;
    int qp;
    uint64_t frameDuration;
    
    // Frame counter
    uint64_t frameNumber;
    
    // Callback
    EncodedFrameCallback frameCallback;
    void* callbackUserData;
    
    // Frame stats
    UINT64 totalBytesEncoded;
    UINT32 lastFrameSize;
    UINT32 minFrameSize;
    UINT32 maxFrameSize;
    
    BOOL initialized;
};

// ============================================================================
// CUDA Loading (from OBS cuda-helpers.c)
// ============================================================================

static BOOL load_cuda_lib(void) {
    if (g_cudaLib) return TRUE;
    
    g_cudaLib = LoadLibraryA("nvcuda.dll");
    if (!g_cudaLib) {
        NvLog("NVENC: Failed to load nvcuda.dll\n");
        return FALSE;
    }
    return TRUE;
}

static void* load_cuda_func(const char* name) {
    void* ptr = (void*)GetProcAddress(g_cudaLib, name);
    if (!ptr) {
        NvLog("NVENC: Failed to load CUDA function: %s\n", name);
    }
    return ptr;
}

static BOOL init_cuda(void) {
    if (cu) return TRUE;
    
    if (!load_cuda_lib()) return FALSE;
    
    cu = (CudaFunctions*)calloc(1, sizeof(CudaFunctions));
    if (!cu) return FALSE;
    
    // Load all functions (like OBS cuda_functions array)
    cu->cuInit = (CU_INIT)load_cuda_func("cuInit");
    cu->cuDeviceGetCount = (CU_DEVICE_GET_COUNT)load_cuda_func("cuDeviceGetCount");
    cu->cuDeviceGet = (CU_DEVICE_GET)load_cuda_func("cuDeviceGet");
    cu->cuCtxCreate = (CU_CTX_CREATE)load_cuda_func("cuCtxCreate_v2");
    cu->cuCtxDestroy = (CU_CTX_DESTROY)load_cuda_func("cuCtxDestroy_v2");
    cu->cuCtxPushCurrent = (CU_CTX_PUSH_CURRENT)load_cuda_func("cuCtxPushCurrent_v2");
    cu->cuCtxPopCurrent = (CU_CTX_POP_CURRENT)load_cuda_func("cuCtxPopCurrent_v2");
    cu->cuArray3DCreate = (CU_ARRAY3D_CREATE)load_cuda_func("cuArray3DCreate_v2");
    cu->cuArrayDestroy = (CU_ARRAY_DESTROY)load_cuda_func("cuArrayDestroy");
    cu->cuMemcpy2D = (CU_MEMCPY2D)load_cuda_func("cuMemcpy2D_v2");
    cu->cuMemHostRegister = (CU_MEM_HOST_REGISTER)load_cuda_func("cuMemHostRegister_v2");
    cu->cuMemHostUnregister = (CU_MEM_HOST_UNREGISTER)load_cuda_func("cuMemHostUnregister");
    cu->cuGetErrorName = (CU_GET_ERROR_NAME)load_cuda_func("cuGetErrorName");
    cu->cuGetErrorString = (CU_GET_ERROR_STRING)load_cuda_func("cuGetErrorString");
    
    // Check all loaded
    if (!cu->cuInit || !cu->cuDeviceGetCount || !cu->cuDeviceGet ||
        !cu->cuCtxCreate || !cu->cuCtxDestroy || !cu->cuCtxPushCurrent ||
        !cu->cuCtxPopCurrent || !cu->cuArray3DCreate || !cu->cuArrayDestroy ||
        !cu->cuMemcpy2D || !cu->cuMemHostRegister || !cu->cuMemHostUnregister) {
        free(cu);
        cu = NULL;
        return FALSE;
    }
    
    return TRUE;
}

// ============================================================================
// CUDA Error Checking (from OBS)
// ============================================================================

#define CU_FAILED(call) \
    do { \
        CUresult _res = (call); \
        if (_res != CUDA_SUCCESS) { \
            const char* _name = "Unknown"; \
            const char* _desc = "Unknown"; \
            if (cu->cuGetErrorName) cu->cuGetErrorName(_res, &_name); \
            if (cu->cuGetErrorString) cu->cuGetErrorString(_res, &_desc); \
            NvLog("NVENC CUDA: %s failed: %s (%d) - %s\n", #call, _name, _res, _desc); \
            return FALSE; \
        } \
    } while(0)

#define CU_CHECK(call) \
    do { \
        CUresult _res = (call); \
        if (_res != CUDA_SUCCESS) { \
            success = FALSE; \
            goto unmap; \
        } \
    } while(0)

// ============================================================================
// CUDA Context Management (from OBS nvenc-cuda.c cuda_ctx_init)
// ============================================================================

static BOOL cuda_ctx_init(NVENCEncoder* enc) {
    int count;
    CUdevice device;
    
    CU_FAILED(cu->cuInit(0));
    CU_FAILED(cu->cuDeviceGetCount(&count));
    
    if (count == 0) {
        NvLog("NVENC: No CUDA devices found\n");
        return FALSE;
    }
    
    CU_FAILED(cu->cuDeviceGet(&device, 0));  // Use first GPU
    CU_FAILED(cu->cuCtxCreate(&enc->cu_ctx, 0, device));
    CU_FAILED(cu->cuCtxPopCurrent(NULL));
    
    NvLog("NVENC: CUDA context created\n");
    return TRUE;
}

static void cuda_ctx_free(NVENCEncoder* enc) {
    if (enc->cu_ctx) {
        cu->cuCtxPopCurrent(NULL);
        cu->cuCtxDestroy(enc->cu_ctx);
        enc->cu_ctx = NULL;
    }
}

// ============================================================================
// CUDA Surface Management (from OBS nvenc-cuda.c)
// ============================================================================

static BOOL cuda_surface_init(NVENCEncoder* enc, CudaSurface* surf) {
    // NV12: Y plane (full height) + UV plane (half height)
    CUDA_ARRAY3D_DESCRIPTOR desc = {0};
    desc.Width = enc->width;
    desc.Height = enc->height + enc->height / 2;  // NV12
    desc.Depth = 0;
    desc.Flags = CUDA_ARRAY3D_SURFACE_LDST;
    desc.NumChannels = 1;
    desc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    
    CU_FAILED(cu->cuArray3DCreate(&surf->tex, &desc));
    
    // Register with NVENC
    NV_ENC_REGISTER_RESOURCE reg = {0};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
    reg.resourceToRegister = (void*)surf->tex;
    reg.width = enc->width;
    reg.height = enc->height;
    reg.pitch = (uint32_t)(desc.Width * desc.NumChannels);
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    
    NVENCSTATUS st = enc->fn.nvEncRegisterResource(enc->encoder, &reg);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: nvEncRegisterResource failed (%d)\n", st);
        cu->cuArrayDestroy(surf->tex);
        return FALSE;
    }
    
    surf->res = reg.registeredResource;
    surf->mapped_res = NULL;
    return TRUE;
}

static BOOL cuda_init_surfaces(NVENCEncoder* enc) {
    CU_FAILED(cu->cuCtxPushCurrent(enc->cu_ctx));
    
    for (int i = 0; i < enc->buf_count; i++) {
        if (!cuda_surface_init(enc, &enc->surfaces[i])) {
            cu->cuCtxPopCurrent(NULL);
            return FALSE;
        }
    }
    
    CU_FAILED(cu->cuCtxPopCurrent(NULL));
    
    NvLog("NVENC: Created %d CUDA surfaces (%dx%d NV12)\n", 
          enc->buf_count, enc->width, enc->height);
    return TRUE;
}

static void cuda_surface_free(NVENCEncoder* enc, CudaSurface* surf) {
    if (surf->res) {
        if (surf->mapped_res) {
            enc->fn.nvEncUnmapInputResource(enc->encoder, surf->mapped_res);
        }
        enc->fn.nvEncUnregisterResource(enc->encoder, surf->res);
        cu->cuArrayDestroy(surf->tex);
        surf->res = NULL;
        surf->mapped_res = NULL;
        surf->tex = NULL;
    }
}

static void cuda_free_surfaces(NVENCEncoder* enc) {
    if (!enc->cu_ctx) return;
    
    cu->cuCtxPushCurrent(enc->cu_ctx);
    for (int i = 0; i < enc->buf_count; i++) {
        cuda_surface_free(enc, &enc->surfaces[i]);
    }
    cu->cuCtxPopCurrent(NULL);
}

// ============================================================================
// Public API
// ============================================================================

BOOL NVENCEncoder_IsAvailable(void) {
    // Check both CUDA and NVENC
    if (!init_cuda()) return FALSE;
    
    HMODULE lib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!lib) lib = LoadLibraryA("nvEncodeAPI.dll");
    if (lib) {
        FreeLibrary(lib);
        return TRUE;
    }
    return FALSE;
}

NVENCEncoder* NVENCEncoder_Create(ID3D11Device* d3dDevice, int width, int height, int fps, QualityPreset quality) {
    (void)d3dDevice;  // Not used in CUDA path
    if (width <= 0 || height <= 0 || fps <= 0) {
        NvLog("NVENC: Invalid parameters\n");
        return NULL;
    }
    
    NvLog("NVENC: Creating encoder (%dx%d @ %d fps, quality=%d)...\n", width, height, fps, quality);
    
    // Init CUDA
    if (!init_cuda()) {
        NvLog("NVENC: CUDA init failed\n");
        return NULL;
    }
    
    NVENCEncoder* enc = (NVENCEncoder*)calloc(1, sizeof(NVENCEncoder));
    if (!enc) return NULL;
    
    enc->width = width;
    enc->height = height;
    enc->fps = fps;
    enc->frameDuration = MF_UNITS_PER_SECOND / fps;
    enc->buf_count = NUM_BUFFERS;
    
    // Create CUDA context
    if (!cuda_ctx_init(enc)) {
        free(enc);
        return NULL;
    }
    
    // Load NVENC
    enc->nvencLib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!enc->nvencLib) enc->nvencLib = LoadLibraryA("nvEncodeAPI.dll");
    if (!enc->nvencLib) {
        NvLog("NVENC: Failed to load nvEncodeAPI64.dll\n");
        cuda_ctx_free(enc);
        free(enc);
        return NULL;
    }
    
    typedef NVENCSTATUS (NVENCAPI *PFN_CREATE)(NV_ENCODE_API_FUNCTION_LIST*);
    PFN_CREATE createInstance = (PFN_CREATE)GetProcAddress(enc->nvencLib, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        NvLog("NVENC: NvEncodeAPICreateInstance not found\n");
        goto fail;
    }
    
    enc->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(&enc->fn) != NV_ENC_SUCCESS) {
        NvLog("NVENC: CreateInstance failed\n");
        goto fail;
    }
    
    // Open session with CUDA device type (like OBS)
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {0};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessionParams.device = enc->cu_ctx;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    
    NVENCSTATUS st = enc->fn.nvEncOpenEncodeSessionEx(&sessionParams, &enc->encoder);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: OpenEncodeSessionEx failed (%d)\n", st);
        goto fail;
    }
    
    NvLog("NVENC: Session opened (CUDA device type)\n");
    
    // Get preset config
    NV_ENC_PRESET_CONFIG presetConfig = {0};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    
    st = enc->fn.nvEncGetEncodePresetConfigEx(enc->encoder,
        NV_ENC_CODEC_HEVC_GUID,
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
        &presetConfig);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: GetEncodePresetConfigEx failed (%d)\n", st);
        goto fail;
    }
    
    // Configure encoder
    NV_ENC_CONFIG config = presetConfig.presetCfg;
    config.gopLength = fps * GOP_LENGTH_SECONDS;
    config.frameIntervalP = 1;  // No B-frames
    
    // Disable expensive features
    config.rcParams.enableAQ = 0;
    config.rcParams.enableTemporalAQ = 0;
    config.rcParams.enableLookahead = 0;
    
    // Constant QP
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    switch (quality) {
        case QUALITY_LOW:      enc->qp = QP_LOW;      break;
        case QUALITY_MEDIUM:   enc->qp = QP_MEDIUM;   break;
        case QUALITY_HIGH:     enc->qp = QP_HIGH;     break;
        case QUALITY_LOSSLESS: enc->qp = QP_LOSSLESS; break;
        default:               enc->qp = QP_MEDIUM;   break;
    }
    config.rcParams.constQP.qpInterP = enc->qp;
    config.rcParams.constQP.qpInterB = enc->qp;
    config.rcParams.constQP.qpIntra = enc->qp > 4 ? enc->qp - 4 : 0;
    
    // Initialize encoder
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
    initParams.enableEncodeAsync = 0;  // Sync mode (like OBS soft encoder)
    initParams.enablePTD = 1;
    initParams.encodeConfig = &config;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    
    st = enc->fn.nvEncInitializeEncoder(enc->encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: InitializeEncoder failed (%d)\n", st);
        goto fail;
    }
    
    NvLog("NVENC: Encoder initialized (HEVC CQP QP=%d)\n", enc->qp);
    
    // Create CUDA surfaces
    if (!cuda_init_surfaces(enc)) {
        NvLog("NVENC: Failed to create CUDA surfaces\n");
        goto fail;
    }
    
    // Create bitstream buffers
    for (int i = 0; i < enc->buf_count; i++) {
        NV_ENC_CREATE_BITSTREAM_BUFFER createBuf = {0};
        createBuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        
        st = enc->fn.nvEncCreateBitstreamBuffer(enc->encoder, &createBuf);
        if (st != NV_ENC_SUCCESS) {
            NvLog("NVENC: CreateBitstreamBuffer[%d] failed (%d)\n", i, st);
            goto fail;
        }
        enc->outputBuffers[i] = createBuf.bitstreamBuffer;
    }
    
    enc->initialized = TRUE;
    NvLog("NVENC: Ready (%d buffers, sync mode)\n", enc->buf_count);
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

// Copy frame to CUDA surface (from OBS copy_frame)
static BOOL copy_frame(NVENCEncoder* enc, BYTE* data[2], int linesize[2], CudaSurface* surf) {
    size_t height = enc->height;
    size_t width = enc->width;
    
    CUDA_MEMCPY2D m = {0};
    m.srcMemoryType = CU_MEMORYTYPE_HOST;
    m.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    m.dstArray = surf->tex;
    m.WidthInBytes = width;
    m.Height = height;
    
    CUresult res;
    
    res = cu->cuCtxPushCurrent(enc->cu_ctx);
    if (res != CUDA_SUCCESS) {
        NvLog("NVENC: cuCtxPushCurrent failed (%d)\n", res);
        return FALSE;
    }
    
    // Copy Y plane
    m.srcPitch = linesize[0];
    m.srcHost = data[0];
    res = cu->cuMemcpy2D(&m);
    if (res != CUDA_SUCCESS) {
        NvLog("NVENC: cuMemcpy2D Y plane failed (%d)\n", res);
        cu->cuCtxPopCurrent(NULL);
        return FALSE;
    }
    
    // Copy UV plane
    m.srcPitch = linesize[1];
    m.srcHost = data[1];
    m.dstY = height;
    m.Height = height / 2;
    res = cu->cuMemcpy2D(&m);
    if (res != CUDA_SUCCESS) {
        NvLog("NVENC: cuMemcpy2D UV plane failed (%d)\n", res);
        cu->cuCtxPopCurrent(NULL);
        return FALSE;
    }
    
    cu->cuCtxPopCurrent(NULL);
    return TRUE;
}

int NVENCEncoder_SubmitFrame(NVENCEncoder* enc, BYTE* data[2], int linesize[2], LONGLONG timestamp) {
    if (!enc || !enc->initialized || !data[0] || !data[1]) return 0;
    
    int idx = enc->next_bitstream;
    CudaSurface* surf = &enc->surfaces[idx];
    NV_ENC_OUTPUT_PTR bs = enc->outputBuffers[idx];
    
    // Copy frame to CUDA surface
    if (!copy_frame(enc, data, linesize, surf)) {
        NvLog("NVENC: copy_frame failed\n");
        return 0;
    }
    
    // Map input resource
    NV_ENC_MAP_INPUT_RESOURCE map = {0};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = surf->res;
    map.mappedBufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    
    NVENCSTATUS st = enc->fn.nvEncMapInputResource(enc->encoder, &map);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: MapInputResource failed (%d)\n", st);
        return 0;
    }
    surf->mapped_res = map.mappedResource;
    
    // Encode
    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = map.mappedResource;
    picParams.outputBitstream = bs;
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picParams.inputWidth = enc->width;
    picParams.inputHeight = enc->height;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputTimeStamp = timestamp;
    picParams.inputDuration = enc->frameDuration;
    
    // Force IDR every GOP
    if (enc->frameNumber % (enc->fps * GOP_LENGTH_SECONDS) == 0) {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    }
    
    st = enc->fn.nvEncEncodePicture(enc->encoder, &picParams);
    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        NvLog("NVENC: EncodePicture failed (%d)\n", st);
        enc->fn.nvEncUnmapInputResource(enc->encoder, surf->mapped_res);
        surf->mapped_res = NULL;
        return 0;
    }
    
    // Lock bitstream (sync mode - blocks until encode done)
    NV_ENC_LOCK_BITSTREAM lock = {0};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bs;
    
    st = enc->fn.nvEncLockBitstream(enc->encoder, &lock);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: LockBitstream failed (%d)\n", st);
        enc->fn.nvEncUnmapInputResource(enc->encoder, surf->mapped_res);
        surf->mapped_res = NULL;
        return 0;
    }
    
    // Copy encoded data and deliver via callback
    if (enc->frameCallback && lock.bitstreamSizeInBytes > 0) {
        EncodedFrame frame = {0};
        frame.data = (BYTE*)malloc(lock.bitstreamSizeInBytes);
        if (frame.data) {
            LEAK_TRACK_NVENC_FRAME_ALLOC();
            memcpy(frame.data, lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
            frame.size = lock.bitstreamSizeInBytes;
            frame.timestamp = timestamp;
            frame.duration = enc->frameDuration;
            frame.isKeyframe = (lock.pictureType == NV_ENC_PIC_TYPE_IDR);
            
            // Stats
            enc->lastFrameSize = lock.bitstreamSizeInBytes;
            enc->totalBytesEncoded += lock.bitstreamSizeInBytes;
            if (enc->minFrameSize == 0 || lock.bitstreamSizeInBytes < enc->minFrameSize)
                enc->minFrameSize = lock.bitstreamSizeInBytes;
            if (lock.bitstreamSizeInBytes > enc->maxFrameSize)
                enc->maxFrameSize = lock.bitstreamSizeInBytes;
            
            enc->frameCallback(&frame, enc->callbackUserData);
        }
    }
    
    // Unlock and unmap
    enc->fn.nvEncUnlockBitstream(enc->encoder, bs);
    enc->fn.nvEncUnmapInputResource(enc->encoder, surf->mapped_res);
    surf->mapped_res = NULL;
    
    enc->next_bitstream = (enc->next_bitstream + 1) % enc->buf_count;
    enc->frameNumber++;
    
    return 1;
}

BOOL NVENCEncoder_GetSequenceHeader(NVENCEncoder* enc, BYTE* buffer, DWORD bufferSize, DWORD* outSize) {
    if (!enc || !enc->initialized || !buffer || !outSize) return FALSE;
    
    *outSize = 0;
    
    uint32_t payloadSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD seqParams = {0};
    seqParams.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    seqParams.inBufferSize = bufferSize;
    seqParams.spsppsBuffer = buffer;
    seqParams.outSPSPPSPayloadSize = &payloadSize;
    
    NVENCSTATUS st = enc->fn.nvEncGetSequenceParams(enc->encoder, &seqParams);
    if (st != NV_ENC_SUCCESS) {
        NvLog("NVENC: GetSequenceParams failed (%d)\n", st);
        return FALSE;
    }
    
    *outSize = payloadSize;
    return TRUE;
}

void NVENCEncoder_GetStats(NVENCEncoder* enc, int* framesEncoded, double* avgEncodeTimeMs) {
    if (!enc) return;
    if (framesEncoded) *framesEncoded = (int)enc->frameNumber;
    if (avgEncodeTimeMs) *avgEncodeTimeMs = 0.0;
}

int NVENCEncoder_GetQP(NVENCEncoder* enc) {
    return enc ? enc->qp : -1;
}

void NVENCEncoder_GetFrameSizeStats(NVENCEncoder* enc, UINT32* lastSize, UINT32* minSize, UINT32* maxSize, UINT32* avgSize) {
    if (!enc) return;
    if (lastSize) *lastSize = enc->lastFrameSize;
    if (minSize) *minSize = enc->minFrameSize;
    if (maxSize) *maxSize = enc->maxFrameSize;
    if (avgSize && enc->frameNumber > 0) {
        *avgSize = (UINT32)(enc->totalBytesEncoded / enc->frameNumber);
    } else if (avgSize) {
        *avgSize = 0;
    }
}

void NVENCEncoder_Destroy(NVENCEncoder* enc) {
    if (!enc) return;
    
    NvLog("NVENC: Destroying encoder (%llu frames)\n", enc->frameNumber);
    
    if (enc->encoder) {
        // Send EOS
        NV_ENC_PIC_PARAMS picParams = {0};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        enc->fn.nvEncEncodePicture(enc->encoder, &picParams);
        
        // Destroy bitstream buffers
        for (int i = 0; i < enc->buf_count; i++) {
            if (enc->outputBuffers[i]) {
                enc->fn.nvEncDestroyBitstreamBuffer(enc->encoder, enc->outputBuffers[i]);
            }
        }
        
        // Destroy surfaces
        cuda_free_surfaces(enc);
        
        // Destroy encoder
        enc->fn.nvEncDestroyEncoder(enc->encoder);
    }
    
    // Free CUDA context
    cuda_ctx_free(enc);
    
    if (enc->nvencLib) {
        FreeLibrary(enc->nvencLib);
    }
    
    free(enc);
}

// ============================================================================
// D3D11 Texture Interface (reads back to CPU, then encodes via CUDA)
// ============================================================================

int NVENCEncoder_SubmitTexture(NVENCEncoder* enc, ID3D11Texture2D* nv12Texture, LONGLONG timestamp) {
    if (!enc || !nv12Texture) return 0;
    
    // Get device context
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* ctx = NULL;
    nv12Texture->lpVtbl->GetDevice(nv12Texture, &device);
    if (!device) return 0;
    device->lpVtbl->GetImmediateContext(device, &ctx);
    device->lpVtbl->Release(device);
    if (!ctx) return 0;
    
    // Create staging texture for CPU readback
    D3D11_TEXTURE2D_DESC texDesc;
    nv12Texture->lpVtbl->GetDesc(nv12Texture, &texDesc);
    
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.MiscFlags = 0;
    
    ID3D11Texture2D* staging = NULL;
    nv12Texture->lpVtbl->GetDevice(nv12Texture, &device);
    HRESULT hr = device->lpVtbl->CreateTexture2D(device, &texDesc, NULL, &staging);
    device->lpVtbl->Release(device);
    
    if (FAILED(hr) || !staging) {
        ctx->lpVtbl->Release(ctx);
        return 0;
    }
    
    // Copy GPU texture to staging
    ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)nv12Texture);
    
    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->lpVtbl->Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        staging->lpVtbl->Release(staging);
        ctx->lpVtbl->Release(ctx);
        return 0;
    }
    
    // NV12: Y plane at offset 0, UV plane at offset height * pitch
    BYTE* data[2];
    int linesize[2];
    
    data[0] = (BYTE*)mapped.pData;
    data[1] = (BYTE*)mapped.pData + enc->height * mapped.RowPitch;
    linesize[0] = mapped.RowPitch;
    linesize[1] = mapped.RowPitch;
    
    // Call new interface
    int result = NVENCEncoder_SubmitFrame(enc, data, linesize, timestamp);
    
    // Cleanup
    ctx->lpVtbl->Unmap(ctx, (ID3D11Resource*)staging, 0);
    staging->lpVtbl->Release(staging);
    ctx->lpVtbl->Release(ctx);
    
    return result;
}

// Stubs for removed functionality
BOOL NVENCEncoder_IsDeviceLost(NVENCEncoder* enc) { (void)enc; return FALSE; }
int NVENCEncoder_DrainCompleted(NVENCEncoder* enc, EncodedFrameCallback cb, void* ud) { (void)enc; (void)cb; (void)ud; return 0; }
BOOL NVENCEncoder_EncodeTexture(NVENCEncoder* enc, ID3D11Texture2D* tex, LONGLONG ts, EncodedFrame* out) { (void)out; return NVENCEncoder_SubmitTexture(enc, tex, ts); }
BOOL NVENCEncoder_Flush(NVENCEncoder* enc, EncodedFrame* out) { (void)enc; (void)out; return FALSE; }
void NVENCEncoder_ForceCleanupLeaked(void) { }
void NVENCEncoder_MarkLeaked(NVENCEncoder* enc) { (void)enc; }

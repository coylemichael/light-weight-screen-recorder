/*
 * capture.c - DXGI Desktop Duplication - acquire frames from desktop
 */

#include "capture.h"
#include "constants.h"
#include "mem_utils.h"
#include "logger.h"

// Monitor enumeration data
typedef struct {
    int targetIndex;
    int currentIndex;
    RECT bounds;
    BOOL found;
} MonitorSearchData;

static BOOL CALLBACK MonitorEnumProcInternal(HMONITOR hMonitor, HDC hdcMonitor, 
                                              LPRECT lprcMonitor, LPARAM dwData) {
    (void)hMonitor;
    (void)hdcMonitor;
    MonitorSearchData* data = (MonitorSearchData*)dwData;
    
    if (data->currentIndex == data->targetIndex) {
        data->bounds = *lprcMonitor;
        data->found = TRUE;
        return FALSE; // Stop enumeration
    }
    
    data->currentIndex++;
    return TRUE;
}

static BOOL CALLBACK MonitorEnumProcAll(HMONITOR hMonitor, HDC hdcMonitor,
                                         LPRECT lprcMonitor, LPARAM dwData) {
    (void)hMonitor;
    (void)hdcMonitor;
    RECT* bounds = (RECT*)dwData;
    
    if (IsRectEmpty(bounds)) {
        *bounds = *lprcMonitor;
    } else {
        UnionRect(bounds, bounds, lprcMonitor);
    }
    
    return TRUE;
}

BOOL Capture_GetMonitorBoundsByIndex(int monitorIndex, RECT* bounds) {
    // Preconditions
    LWSR_ASSERT(monitorIndex >= 0);
    LWSR_ASSERT(bounds != NULL);
    
    if (!bounds) return FALSE;
    
    MonitorSearchData data;
    data.targetIndex = monitorIndex;
    data.currentIndex = 0;
    data.found = FALSE;
    SetRectEmpty(&data.bounds);
    
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProcInternal, (LPARAM)&data);
    
    if (data.found) {
        *bounds = data.bounds;
        return TRUE;
    }
    return FALSE;
}

BOOL Capture_GetMonitorFromPoint(POINT pt, RECT* monitorRect, int* monitorIndex) {
    // Preconditions
    LWSR_ASSERT(monitorRect != NULL);
    LWSR_ASSERT(monitorIndex != NULL);
    
    if (!monitorRect || !monitorIndex) return FALSE;
    
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return FALSE;
    
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi)) return FALSE;
    
    *monitorRect = mi.rcMonitor;
    
    // Always returns 0 — actual DXGI output matching is done by Capture_SetRegion().
    // This parameter exists for API symmetry; callers should not rely on the value.
    *monitorIndex = 0;
    
    return TRUE;
}

BOOL Capture_GetAllMonitorsBounds(RECT* bounds) {
    // Precondition
    LWSR_ASSERT(bounds != NULL);
    
    if (!bounds) return FALSE;
    
    SetRectEmpty(bounds);
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProcAll, (LPARAM)bounds);
    return !IsRectEmpty(bounds);
}

// DWM library loading - file-scope statics for thread-safe initialization
typedef HRESULT (WINAPI *DwmGetWindowAttributeFunc)(HWND, DWORD, PVOID, DWORD);
static DwmGetWindowAttributeFunc s_pDwmGetWindowAttribute = NULL;
static INIT_ONCE s_dwmInitOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitDwmOnce(PINIT_ONCE initOnce, PVOID param, PVOID* context) {
    (void)initOnce; (void)param; (void)context;
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (hDwm) {
        s_pDwmGetWindowAttribute = (DwmGetWindowAttributeFunc)
            GetProcAddress(hDwm, "DwmGetWindowAttribute");
        // Intentionally not calling FreeLibrary - keep loaded for app lifetime
    }
    return TRUE;
}

BOOL Capture_GetWindowRect(HWND hwnd, RECT* rect) {
    // Preconditions
    LWSR_ASSERT(rect != NULL);
    
    if (!rect) return FALSE;
    if (!IsWindow(hwnd)) return FALSE;
    
    // Thread-safe one-time DWM initialization
    InitOnceExecuteOnce(&s_dwmInitOnce, InitDwmOnce, NULL, NULL);
    
    if (s_pDwmGetWindowAttribute) {
        // DWMWA_EXTENDED_FRAME_BOUNDS = 9
        if (SUCCEEDED(s_pDwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS_CONST, rect, sizeof(RECT)))) {
            return TRUE;
        }
    }
    
    return GetWindowRect(hwnd, rect);
}

// Helper to release duplication resources without full cleanup
static void ReleaseDuplication(CaptureState* state) {
    SAFE_RELEASE(state->stagingTexture);
    SAFE_RELEASE(state->duplication);
}

// Initialize desktop duplication for a specific DXGI output index.
// All writes to `state` are deferred until every step has succeeded, so a
// partial failure leaves the caller's prior state intact.
static BOOL InitDuplicationForOutput(CaptureState* state, IDXGIAdapter* adapter, int outputIndex) {
    BOOL result = FALSE;
    IDXGIOutput* dxgiOutput = NULL;
    IDXGIOutput1* dxgiOutput1 = NULL;
    IDXGIOutputDuplication* newDuplication = NULL;
    DXGI_OUTPUT_DESC localDesc = {0};
    int localRefreshRate = DEFAULT_REFRESH_RATE;
    
    HRESULT hr = adapter->lpVtbl->EnumOutputs(adapter, outputIndex, &dxgiOutput);
    if (FAILED(hr)) {
        Logger_Log("InitDuplicationForOutput: EnumOutputs[%d] failed (0x%08X)\n", outputIndex, hr);
        goto cleanup;
    }
    
    hr = dxgiOutput->lpVtbl->GetDesc(dxgiOutput, &localDesc);
    if (FAILED(hr)) {
        Logger_Log("InitDuplicationForOutput: GetDesc failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    DXGI_MODE_DESC desiredMode = {0};
    desiredMode.Width = localDesc.DesktopCoordinates.right - localDesc.DesktopCoordinates.left;
    desiredMode.Height = localDesc.DesktopCoordinates.bottom - localDesc.DesktopCoordinates.top;
    desiredMode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    
    DXGI_MODE_DESC closestMode = {0};
    hr = dxgiOutput->lpVtbl->FindClosestMatchingMode(dxgiOutput, &desiredMode, &closestMode, (IUnknown*)state->device);
    if (SUCCEEDED(hr) && closestMode.RefreshRate.Denominator > 0) {
        localRefreshRate = closestMode.RefreshRate.Numerator / closestMode.RefreshRate.Denominator;
    }
    
    hr = dxgiOutput->lpVtbl->QueryInterface(dxgiOutput, &IID_IDXGIOutput1, (void**)&dxgiOutput1);
    if (FAILED(hr)) {
        Logger_Log("InitDuplicationForOutput: QueryInterface IDXGIOutput1 failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    hr = dxgiOutput1->lpVtbl->DuplicateOutput(dxgiOutput1, (IUnknown*)state->device, &newDuplication);
    if (FAILED(hr)) {
        Logger_Log("InitDuplicationForOutput: DuplicateOutput failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    // All steps succeeded — commit to state.
    state->outputDesc = localDesc;
    state->monitorRefreshRate = localRefreshRate;
    state->duplication = newDuplication;
    newDuplication = NULL;  // ownership transferred
    state->monitorIndex = outputIndex;
    state->monitorWidth = localDesc.DesktopCoordinates.right -
                          localDesc.DesktopCoordinates.left;
    state->monitorHeight = localDesc.DesktopCoordinates.bottom -
                           localDesc.DesktopCoordinates.top;
    
    // Default capture region is full monitor
    state->captureRect = localDesc.DesktopCoordinates;
    state->captureWidth = state->monitorWidth;
    state->captureHeight = state->monitorHeight;
    
    result = TRUE;
    
cleanup:
    SAFE_RELEASE(newDuplication);
    SAFE_RELEASE(dxgiOutput1);
    SAFE_RELEASE(dxgiOutput);
    
    return result;
}

// Find which DXGI output contains most of the given region
static int FindOutputForRegion(IDXGIAdapter* adapter, RECT region) {
    int bestOutput = 0;
    int bestOverlap = 0;
    
    for (int i = 0; i < LWSR_MAX_MONITORS; i++) {
        IDXGIOutput* output = NULL;
        if (FAILED(adapter->lpVtbl->EnumOutputs(adapter, i, &output))) break;
        
        DXGI_OUTPUT_DESC desc = {0};
        HRESULT hr = output->lpVtbl->GetDesc(output, &desc);
        output->lpVtbl->Release(output);
        if (FAILED(hr)) continue;
        
        RECT overlap = {0};
        if (IntersectRect(&overlap, &region, &desc.DesktopCoordinates)) {
            int overlapArea = (overlap.right - overlap.left) * (overlap.bottom - overlap.top);
            if (overlapArea > bestOverlap) {
                bestOverlap = overlapArea;
                bestOutput = i;
            }
        }
    }
    
    return bestOutput;
}

BOOL Capture_Init(CaptureState* state) {
    // Precondition
    LWSR_ASSERT(state != NULL);
    
    if (!state) return FALSE;
    
    BOOL result = FALSE;
    IDXGIDevice* dxgiDevice = NULL;
    
    ZeroMemory(state, sizeof(CaptureState));
    
    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    HRESULT hr = D3D11CreateDevice(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &state->device,
        &featureLevel,
        &state->context
    );
    
    if (FAILED(hr)) {
        Logger_Log("Capture_Init: D3D11CreateDevice failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    // Get DXGI device
    hr = state->device->lpVtbl->QueryInterface(state->device, &IID_IDXGIDevice, (void**)&dxgiDevice);
    if (FAILED(hr)) {
        Logger_Log("Capture_Init: QueryInterface IDXGIDevice failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    // Get DXGI adapter and store it
    hr = dxgiDevice->lpVtbl->GetAdapter(dxgiDevice, &state->adapter);
    if (FAILED(hr)) {
        Logger_Log("Capture_Init: GetAdapter failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    // Initialize with primary output (output 0)
    if (!InitDuplicationForOutput(state, state->adapter, 0)) goto cleanup;
    
    state->initialized = TRUE;
    result = TRUE;
    
cleanup:
    SAFE_RELEASE(dxgiDevice);
    
    if (!result) {
        SAFE_RELEASE(state->adapter);
        SAFE_RELEASE(state->context);
        SAFE_RELEASE(state->device);
    }
    
    return result;
}

BOOL Capture_SetRegion(CaptureState* state, RECT region) {
    // Preconditions
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(region.right > region.left);
    LWSR_ASSERT(region.bottom > region.top);
    
    if (!state) return FALSE;
    if (!state->initialized) return FALSE;
    
    // Check if region is on a different monitor than current
    int targetOutput = FindOutputForRegion(state->adapter, region);
    if (targetOutput != state->monitorIndex) {
        // Need to switch to a different output
        ReleaseDuplication(state);
        if (!InitDuplicationForOutput(state, state->adapter, targetOutput)) {
            // Failed to switch, try to restore original
            if (!InitDuplicationForOutput(state, state->adapter, state->monitorIndex)) {
                // Both failed - mark state as unusable
                state->initialized = FALSE;
                Logger_Log("Capture_SetRegion: Failed to restore duplication\n");
            }
            return FALSE;
        }
    }
    
    // Clamp to monitor bounds
    IntersectRect(&state->captureRect, &region, &state->outputDesc.DesktopCoordinates);
    
    // Check if we got a valid intersection
    if (IsRectEmpty(&state->captureRect)) {
        // Region doesn't overlap with any monitor we can capture
        return FALSE;
    }
    
    int newWidth = (state->captureRect.right - state->captureRect.left) & ~1;
    int newHeight = (state->captureRect.bottom - state->captureRect.top) & ~1;
    
    // Release GPU texture if dimensions changed (will be recreated on next frame)
    if (state->gpuTexture && (newWidth != state->captureWidth || newHeight != state->captureHeight)) {
        SAFE_RELEASE(state->gpuTexture);
    }
    
    state->captureWidth = newWidth;
    state->captureHeight = newHeight;
    state->captureRect.right = state->captureRect.left + state->captureWidth;
    state->captureRect.bottom = state->captureRect.top + state->captureHeight;
    
    // Reallocate frame buffer if needed
    size_t newSize = (size_t)state->captureWidth * state->captureHeight * BYTES_PER_PIXEL_BGRA;
    if (newSize > state->frameBufferSize) {
        BYTE* newBuffer = (BYTE*)malloc(newSize);
        if (newBuffer) {
            ZeroMemory(newBuffer, newSize);  // prevent uninitialized read on first-frame timeout
            free(state->frameBuffer);
            state->frameBuffer = newBuffer;
            state->frameBufferSize = newSize;
        } else {
            Logger_Log("Capture_SetRegion: malloc failed for frame buffer (%zu bytes)\n", newSize);
            return FALSE;  // Keep existing buffer
        }
    }
    
    return TRUE;
}

BOOL Capture_SetMonitor(CaptureState* state, int monitorIndex) {
    // Preconditions
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(monitorIndex >= 0);
    
    if (!state) return FALSE;
    
    // NOTE: `monitorIndex` here is in GDI EnumDisplayMonitors order. Capture_SetRegion
    // (called below) then resolves the matching DXGI output via FindOutputForRegion,
    // and overwrites state->monitorIndex with the DXGI order. The two enumeration
    // orders are not guaranteed to match. Callers should treat `state->monitorIndex`
    // as an opaque DXGI output index, not the value originally passed here.
    MonitorSearchData search = {0};
    search.targetIndex = monitorIndex;
    search.currentIndex = 0;
    search.found = FALSE;
    
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProcInternal, (LPARAM)&search);
    
    if (search.found) {
        return Capture_SetRegion(state, search.bounds);
    }
    
    return FALSE;
}

// Acquire next duplicated desktop frame and query its ID3D11Texture2D.
// Returns the HRESULT from AcquireNextFrame (or from the QI), so callers can
// distinguish DXGI_ERROR_WAIT_TIMEOUT / DXGI_ERROR_ACCESS_LOST / etc.
// On S_OK: caller owns *outDesktopTexture and MUST Release it and call
// state->duplication->ReleaseFrame after use.
// On non-S_OK: no cleanup is required by the caller; *outDesktopTexture is NULL.
static HRESULT AcquireDesktopTexture(CaptureState* state, DWORD timeoutMs,
                                     DXGI_OUTDUPL_FRAME_INFO* outInfo,
                                     ID3D11Texture2D** outDesktopTexture) {
    *outDesktopTexture = NULL;
    IDXGIResource* desktopResource = NULL;
    
    HRESULT hr = state->duplication->lpVtbl->AcquireNextFrame(
        state->duplication, timeoutMs, outInfo, &desktopResource);
    if (FAILED(hr)) return hr;
    
    hr = desktopResource->lpVtbl->QueryInterface(desktopResource, &IID_ID3D11Texture2D,
                                                  (void**)outDesktopTexture);
    desktopResource->lpVtbl->Release(desktopResource);
    
    if (FAILED(hr)) {
        state->duplication->lpVtbl->ReleaseFrame(state->duplication);
        *outDesktopTexture = NULL;
        return hr;
    }
    return S_OK;
}

// Copy the configured capture region from desktopTexture into destTexture,
// then release the desktop texture and the acquired frame.
static void CopyCaptureRegionAndRelease(CaptureState* state,
                                        ID3D11Texture2D* desktopTexture,
                                        ID3D11Texture2D* destTexture) {
    D3D11_BOX srcBox = {0};
    srcBox.left = state->captureRect.left - state->outputDesc.DesktopCoordinates.left;
    srcBox.top = state->captureRect.top - state->outputDesc.DesktopCoordinates.top;
    srcBox.right = srcBox.left + state->captureWidth;
    srcBox.bottom = srcBox.top + state->captureHeight;
    srcBox.front = 0;
    srcBox.back = 1;
    
    state->context->lpVtbl->CopySubresourceRegion(
        state->context,
        (ID3D11Resource*)destTexture, 0, 0, 0, 0,
        (ID3D11Resource*)desktopTexture, 0, &srcBox);
    
    desktopTexture->lpVtbl->Release(desktopTexture);
    state->duplication->lpVtbl->ReleaseFrame(state->duplication);
}

BYTE* Capture_GetFrame(CaptureState* state, UINT64* timestamp) {
    // Precondition
    LWSR_ASSERT(state != NULL);
    
    if (!state || !state->initialized || !state->duplication) return NULL;
    
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {0};
    ID3D11Texture2D* desktopTexture = NULL;
    
    HRESULT hr = AcquireDesktopTexture(state, FRAME_ACQUIRE_TIMEOUT_MS, &frameInfo, &desktopTexture);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame, return last frame if available (for static content).
        // frameBuffer is zeroed at allocation, so the first-frame-timeout case
        // returns valid (black) pixels rather than uninitialized memory.
        if (state->frameBuffer && state->frameBufferSize > 0) {
            if (timestamp) *timestamp = state->lastFrameTime;
            return state->frameBuffer;
        }
        return NULL;
    }
    
    if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        // Desktop duplication lost - signal caller to call Capture_ReinitDuplication.
        state->accessLost = TRUE;
        return NULL;
    }
    
    if (FAILED(hr)) {
        return NULL;
    }
    
    // Create or update staging texture (sized to full desktop)
    if (!state->stagingTexture) {
        D3D11_TEXTURE2D_DESC desc = {0};
        desktopTexture->lpVtbl->GetDesc(desktopTexture, &desc);
        
        D3D11_TEXTURE2D_DESC stagingDesc = {0};
        stagingDesc.Width = desc.Width;
        stagingDesc.Height = desc.Height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        
        hr = state->device->lpVtbl->CreateTexture2D(state->device, &stagingDesc, NULL, &state->stagingTexture);
        if (FAILED(hr)) {
            desktopTexture->lpVtbl->Release(desktopTexture);
            state->duplication->lpVtbl->ReleaseFrame(state->duplication);
            return NULL;
        }
    }
    
    CopyCaptureRegionAndRelease(state, desktopTexture, state->stagingTexture);
    
    // Map staging texture to CPU memory
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    hr = state->context->lpVtbl->Map(state->context, (ID3D11Resource*)state->stagingTexture,
                                      0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return NULL;
    
    if (!state->frameBuffer) {
        state->context->lpVtbl->Unmap(state->context, (ID3D11Resource*)state->stagingTexture, 0);
        return NULL;
    }
    
    BYTE* src = (BYTE*)mapped.pData;
    BYTE* dst = state->frameBuffer;
    size_t rowBytes = (size_t)state->captureWidth * BYTES_PER_PIXEL_BGRA;
    
    for (int y = 0; y < state->captureHeight; y++) {
        memcpy(dst, src, rowBytes);
        src += mapped.RowPitch;
        dst += rowBytes;
    }
    
    state->context->lpVtbl->Unmap(state->context, (ID3D11Resource*)state->stagingTexture, 0);
    
    state->lastFrameTime = frameInfo.LastPresentTime.QuadPart;
    if (timestamp) *timestamp = state->lastFrameTime;
    
    return state->frameBuffer;
}

ID3D11Texture2D* Capture_GetFrameTexture(CaptureState* state, UINT64* timestamp) {
    // Precondition
    LWSR_ASSERT(state != NULL);
    
    if (!state || !state->initialized || !state->duplication) return NULL;
    
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {0};
    ID3D11Texture2D* desktopTexture = NULL;
    
    // 0ms timeout - caller handles frame pacing, don't wait here
    HRESULT hr = AcquireDesktopTexture(state, 0, &frameInfo, &desktopTexture);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available - return last texture to maintain frame rate
        if (state->gpuTexture) {
            if (timestamp) *timestamp = state->lastFrameTime;
            return state->gpuTexture;
        }
        return NULL;
    }
    
    if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        state->accessLost = TRUE;
        return NULL;
    }
    
    if (FAILED(hr)) {
        if (state->gpuTexture) {
            if (timestamp) *timestamp = state->lastFrameTime;
            return state->gpuTexture;
        }
        return NULL;
    }
    
    // Create or reuse GPU texture (stays on GPU, no CPU access)
    if (!state->gpuTexture) {
        D3D11_TEXTURE2D_DESC desc = {0};
        desktopTexture->lpVtbl->GetDesc(desktopTexture, &desc);
        
        D3D11_TEXTURE2D_DESC gpuDesc = {0};
        gpuDesc.Width = state->captureWidth;
        gpuDesc.Height = state->captureHeight;
        gpuDesc.MipLevels = 1;
        gpuDesc.ArraySize = 1;
        gpuDesc.Format = desc.Format;  // BGRA
        gpuDesc.SampleDesc.Count = 1;
        gpuDesc.Usage = D3D11_USAGE_DEFAULT;
        gpuDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        gpuDesc.CPUAccessFlags = 0;
        gpuDesc.MiscFlags = 0;
        
        hr = state->device->lpVtbl->CreateTexture2D(state->device, &gpuDesc, NULL, &state->gpuTexture);
        if (FAILED(hr)) {
            desktopTexture->lpVtbl->Release(desktopTexture);
            state->duplication->lpVtbl->ReleaseFrame(state->duplication);
            return NULL;
        }
    }
    
    CopyCaptureRegionAndRelease(state, desktopTexture, state->gpuTexture);
    
    state->lastFrameTime = frameInfo.LastPresentTime.QuadPart;
    if (timestamp) *timestamp = state->lastFrameTime;
    
    return state->gpuTexture;
}

int Capture_GetRefreshRate(CaptureState* state) {
    // Precondition
    LWSR_ASSERT(state != NULL);
    
    if (!state) return DEFAULT_REFRESH_RATE;  // Safe default
    
    return state->monitorRefreshRate;
}

void Capture_Shutdown(CaptureState* state) {
    // Allow NULL for convenience in cleanup code
    if (!state) return;
    
    SAFE_FREE(state->frameBuffer);
    SAFE_RELEASE(state->gpuTexture);
    SAFE_RELEASE(state->stagingTexture);
    SAFE_RELEASE(state->duplication);
    SAFE_RELEASE(state->adapter);
    SAFE_RELEASE(state->context);
    SAFE_RELEASE(state->device);
    state->initialized = FALSE;
}

BOOL Capture_ReinitDuplication(CaptureState* state) {
    // Precondition
    LWSR_ASSERT(state != NULL);
    
    if (!state) return FALSE;
    if (!state->initialized || !state->adapter) return FALSE;
    
    // Save capture region BEFORE InitDuplicationForOutput resets it to full monitor
    RECT savedRect = state->captureRect;
    
    // Release old duplication and GPU texture (has stale frames)
    ReleaseDuplication(state);
    SAFE_RELEASE(state->gpuTexture);
    
    // Recreate duplication on same output
    if (!InitDuplicationForOutput(state, state->adapter, state->monitorIndex)) {
        return FALSE;
    }
    
    // Restore capture region (aspect-ratio crop)
    if (!Capture_SetRegion(state, savedRect)) {
        return FALSE;
    }
    
    // Clear the access lost flag
    state->accessLost = FALSE;
    
    return TRUE;
}

BOOL Capture_ReadbackRegion(CaptureState* state, ID3D11Texture2D* srcTexture,
                            int srcX, int srcY, int regionW, int regionH,
                            BYTE* outBuffer, int* outStride) {
    // Preconditions
    LWSR_ASSERT(state != NULL);
    LWSR_ASSERT(srcTexture != NULL);
    LWSR_ASSERT(outBuffer != NULL);
    LWSR_ASSERT(regionW > 0 && regionH > 0);
    
    if (!state || !srcTexture || !outBuffer) return FALSE;
    if (!state->initialized || !state->device || !state->context) return FALSE;
    if (regionW <= 0 || regionH <= 0) return FALSE;
    
    BOOL result = FALSE;
    ID3D11Texture2D* staging = NULL;
    BOOL mapped = FALSE;
    D3D11_MAPPED_SUBRESOURCE mappedRes = {0};
    
    // Validate region against source texture extents
    D3D11_TEXTURE2D_DESC srcDesc = {0};
    srcTexture->lpVtbl->GetDesc(srcTexture, &srcDesc);
    if (srcX < 0 || srcY < 0 ||
        (UINT)(srcX + regionW) > srcDesc.Width ||
        (UINT)(srcY + regionH) > srcDesc.Height) {
        Logger_Log("Capture_ReadbackRegion: region (%d,%d %dx%d) out of bounds for %ux%u source\n",
                   srcX, srcY, regionW, regionH, srcDesc.Width, srcDesc.Height);
        goto cleanup;
    }
    
    // Create a CPU-readable staging texture sized to the region.
    // Allocated per-call; kill_feed_sampler only invokes this every SCAN_INTERVAL_MS
    // (seconds), so per-call create/release overhead is negligible and keeps
    // CaptureState free of caller-specific staging fields.
    D3D11_TEXTURE2D_DESC stagingDesc = {0};
    stagingDesc.Width = (UINT)regionW;
    stagingDesc.Height = (UINT)regionH;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = srcDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    
    HRESULT hr = state->device->lpVtbl->CreateTexture2D(state->device, &stagingDesc, NULL, &staging);
    if (FAILED(hr)) {
        Logger_Log("Capture_ReadbackRegion: CreateTexture2D failed (0x%08X)\n", hr);
        goto cleanup;
    }
    
    D3D11_BOX srcBox = {0};
    srcBox.left = (UINT)srcX;
    srcBox.top = (UINT)srcY;
    srcBox.right = (UINT)(srcX + regionW);
    srcBox.bottom = (UINT)(srcY + regionH);
    srcBox.front = 0;
    srcBox.back = 1;
    
    state->context->lpVtbl->CopySubresourceRegion(
        state->context,
        (ID3D11Resource*)staging, 0, 0, 0, 0,
        (ID3D11Resource*)srcTexture, 0, &srcBox);
    
    hr = state->context->lpVtbl->Map(state->context, (ID3D11Resource*)staging,
                                      0, D3D11_MAP_READ, 0, &mappedRes);
    if (FAILED(hr)) {
        Logger_Log("Capture_ReadbackRegion: Map failed (0x%08X)\n", hr);
        goto cleanup;
    }
    mapped = TRUE;
    
    // Copy out compactly: outBuffer is sized regionW*regionH*BYTES_PER_PIXEL_BGRA
    // (no padding), and outStride is reported as the compact row length so
    // callers iterating with `outStride` stay within the allocated buffer
    // regardless of the GPU staging row pitch.
    size_t rowBytes = (size_t)regionW * BYTES_PER_PIXEL_BGRA;
    BYTE* src = (BYTE*)mappedRes.pData;
    BYTE* dst = outBuffer;
    for (int y = 0; y < regionH; y++) {
        memcpy(dst, src, rowBytes);
        src += mappedRes.RowPitch;
        dst += rowBytes;
    }
    
    if (outStride) *outStride = (int)rowBytes;
    result = TRUE;
    
cleanup:
    if (mapped) {
        state->context->lpVtbl->Unmap(state->context, (ID3D11Resource*)staging, 0);
    }
    SAFE_RELEASE(staging);
    return result;
}

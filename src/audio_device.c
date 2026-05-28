/*
 * audio_device.c - Enumerate/select WASAPI audio devices
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation checks
 * - HRESULT checks use FAILED()/SUCCEEDED() macros exclusively
 * - COM errors are logged and result in graceful fallback
 * - Returns BOOL/NULL to propagate errors; callers must check
 */

#define COBJMACROS
#include <initguid.h>       /* Must come before audio_guids.h to define GUIDs */
#include "audio_device.h"
#include "audio_guids.h"
#include "logger.h"
#include "util.h"
#include "mem_utils.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

/*
 * Global MMDevice enumerator for device listing.
 * Thread Access: Write-once at init (main thread, AudioDevice_Init); read from
 *   multiple threads (settings UI on main, audio_capture/replay_buffer workers).
 *   Safe because the write completes before any reads occur.
 * Lifetime: Init to Shutdown
 * Note: Separate from g_audioEnumerator in audio_capture.c
 */
static IMMDeviceEnumerator* g_deviceEnumerator = NULL;

BOOL AudioDevice_Init(void) {
    if (g_deviceEnumerator) return TRUE;  // Already initialized
    
    HRESULT hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator_Shared,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator_Shared,
        (void**)&g_deviceEnumerator
    );
    
    if (FAILED(hr)) {
        Logger_Log("AudioDevice_Init: CoCreateInstance failed (0x%08X)\n", hr);
        return FALSE;
    }
    return TRUE;
}

void AudioDevice_Shutdown(void) {
    if (g_deviceEnumerator) {
        g_deviceEnumerator->lpVtbl->Release(g_deviceEnumerator);
        g_deviceEnumerator = NULL;
    }
}

IMMDeviceEnumerator* AudioDevice_GetEnumerator(void) {
    return g_deviceEnumerator;
}

// Enumerate devices of a specific type (render or capture)
static int EnumerateDeviceType(AudioDeviceList* list, EDataFlow dataFlow, AudioDeviceType type) {
    int added = 0;
    IMMDeviceCollection* collection = NULL;
    IMMDevice* defaultDevice = NULL;
    
    if (!g_deviceEnumerator || !list) return 0;
    
    HRESULT hr = g_deviceEnumerator->lpVtbl->EnumAudioEndpoints(
        g_deviceEnumerator,
        dataFlow,
        DEVICE_STATE_ACTIVE,
        &collection
    );
    
    if (FAILED(hr)) goto cleanup;
    
    UINT count = 0;
    hr = collection->lpVtbl->GetCount(collection, &count);
    if (FAILED(hr)) goto cleanup;
    
    // Get default device ID for comparison
    char defaultId[128] = {0};
    hr = g_deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(
        g_deviceEnumerator,
        dataFlow,
        eConsole,
        &defaultDevice
    );
    
    if (SUCCEEDED(hr) && defaultDevice) {
        LPWSTR wideId = NULL;
        HRESULT hrId = defaultDevice->lpVtbl->GetId(defaultDevice, &wideId);
        if (SUCCEEDED(hrId) && wideId) {
            if (Util_WideToUtf8(wideId, defaultId, sizeof(defaultId)) <= 0) {
                Logger_Log("AudioDevice: default device ID truncated or conversion failed\n");
                defaultId[0] = '\0';
            }
            CoTaskMemFree(wideId);
        }
    }
    
    for (UINT i = 0; i < count && list->count < MAX_AUDIO_DEVICES; i++) {
        IMMDevice* device = NULL;
        hr = collection->lpVtbl->Item(collection, i, &device);
        if (FAILED(hr)) continue;
        
        AudioDeviceInfo* info = &list->devices[list->count];
        memset(info, 0, sizeof(*info));
        
        // Get device ID
        LPWSTR wideId = NULL;
        HRESULT hrId = device->lpVtbl->GetId(device, &wideId);
        if (SUCCEEDED(hrId) && wideId) {
            if (Util_WideToUtf8(wideId, info->id, sizeof(info->id)) <= 0) {
                Logger_Log("AudioDevice: device ID truncated or conversion failed\n");
                info->id[0] = '\0';
            }
            CoTaskMemFree(wideId);
        }
        
        // Get friendly name from properties
        IPropertyStore* props = NULL;
        hr = device->lpVtbl->OpenPropertyStore(device, STGM_READ, &props);
        if (SUCCEEDED(hr)) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            hr = props->lpVtbl->GetValue(props, &PKEY_Device_FriendlyName, &varName);
            if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
                if (Util_WideToUtf8(varName.pwszVal, info->name, sizeof(info->name)) <= 0) {
                    Logger_Log("AudioDevice: device name truncated or conversion failed\n");
                    info->name[0] = '\0';
                }
            }
            PropVariantClear(&varName);
            SAFE_RELEASE(props);
        }
        
        // If no name, use ID
        if (info->name[0] == '\0') {
            strncpy(info->name, info->id, sizeof(info->name) - 1);
        }
        
        info->type = type;
        // Guard against false positive when either ID is empty (e.g. GetId failed)
        info->isDefault = (info->id[0] != '\0' && defaultId[0] != '\0' &&
                           strcmp(info->id, defaultId) == 0);
        
        SAFE_RELEASE(device);
        
        list->count++;
        added++;
    }
    
cleanup:
    SAFE_RELEASE(defaultDevice);
    SAFE_RELEASE(collection);
    return added;
}

int AudioDevice_Enumerate(AudioDeviceList* list) {
    if (!list) return 0;
    
    // Auto-initialize if needed
    if (!g_deviceEnumerator) {
        if (!AudioDevice_Init()) {
            return 0;
        }
    }
    
    list->count = 0;
    
    // Enumerate output devices (for loopback - system audio)
    EnumerateDeviceType(list, eRender, AUDIO_DEVICE_OUTPUT);
    
    // Enumerate input devices (microphones)
    EnumerateDeviceType(list, eCapture, AUDIO_DEVICE_INPUT);
    
    return list->count;
}

BOOL AudioDevice_GetById(const char* deviceId, AudioDeviceInfo* info) {
    if (!deviceId || !info) return FALSE;
    
    // Auto-initialize if needed
    if (!g_deviceEnumerator) {
        if (!AudioDevice_Init()) return FALSE;
    }
    
    // Convert device ID to wide string for COM API
    WCHAR wideId[128];
    if (MultiByteToWideChar(CP_UTF8, 0, deviceId, -1, wideId, 128) == 0) {
        return FALSE;
    }
    
    // Direct lookup via GetDevice (much faster than enumerating all)
    IMMDevice* device = NULL;
    HRESULT hr = g_deviceEnumerator->lpVtbl->GetDevice(g_deviceEnumerator, wideId, &device);
    if (FAILED(hr) || !device) return FALSE;
    
    // Copy device ID
    strncpy(info->id, deviceId, sizeof(info->id) - 1);
    info->id[sizeof(info->id) - 1] = '\0';
    
    // Get friendly name from properties
    IPropertyStore* props = NULL;
    hr = device->lpVtbl->OpenPropertyStore(device, STGM_READ, &props);
    if (SUCCEEDED(hr) && props) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->lpVtbl->GetValue(props, &PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            Util_WideToUtf8(varName.pwszVal, info->name, sizeof(info->name));
        } else {
            strncpy(info->name, deviceId, sizeof(info->name) - 1);
        }
        PropVariantClear(&varName);
        SAFE_RELEASE(props);
    } else {
        // Fallback to device ID as name
        strncpy(info->name, deviceId, sizeof(info->name) - 1);
    }
    info->name[sizeof(info->name) - 1] = '\0';
    
    // Determine device type by checking endpoint data flow
    IMMEndpoint* endpoint = NULL;
    hr = device->lpVtbl->QueryInterface(device, &IID_IMMEndpoint, (void**)&endpoint);
    if (SUCCEEDED(hr) && endpoint) {
        EDataFlow dataFlow;
        hr = endpoint->lpVtbl->GetDataFlow(endpoint, &dataFlow);
        if (SUCCEEDED(hr)) {
            info->type = (dataFlow == eRender) ? AUDIO_DEVICE_OUTPUT : AUDIO_DEVICE_INPUT;
        } else {
            info->type = AUDIO_DEVICE_OUTPUT;  // Default assumption
        }
        SAFE_RELEASE(endpoint);
    } else {
        // QueryInterface failed - device is still valid, continue with default type
        info->type = AUDIO_DEVICE_OUTPUT;
    }
    
    // Check if this is the default device
    info->isDefault = FALSE;
    IMMDevice* defaultDevice = NULL;
    EDataFlow flow = (info->type == AUDIO_DEVICE_OUTPUT) ? eRender : eCapture;
    hr = g_deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(g_deviceEnumerator, flow, eConsole, &defaultDevice);
    if (SUCCEEDED(hr) && defaultDevice) {
        LPWSTR defaultWideId = NULL;
        hr = defaultDevice->lpVtbl->GetId(defaultDevice, &defaultWideId);
        if (SUCCEEDED(hr) && defaultWideId) {
            info->isDefault = (wcscmp(wideId, defaultWideId) == 0);
            CoTaskMemFree(defaultWideId);
        }
        SAFE_RELEASE(defaultDevice);
    }
    
    SAFE_RELEASE(device);
    return TRUE;
}



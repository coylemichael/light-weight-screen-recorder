/*
 * Utility Functions Implementation
 * Shared calculations and helpers used across modules
 *
 * ERROR HANDLING PATTERN:
 * - Pure computation functions - no external resources
 * - Input validation via early return with safe defaults
 * - No HRESULT usage - these are pure C helper functions
 */

#include "util.h"
#include "logger.h"

// Calculate video bitrate based on quality preset
// Uses ShadowPlay-style scaling: base bitrate scales with resolution and FPS
UINT32 Util_CalculateBitrate(int width, int height, int fps, QualityPreset quality) {
    // Preconditions
    LWSR_ASSERT(width > 0);
    LWSR_ASSERT(height > 0);
    LWSR_ASSERT(fps > 0);
    
    // ShadowPlay-style fixed bitrates (in Mbps)
    // These are for 1440p - we scale for other resolutions
    float baseMbps;
    
    switch (quality) {
        case QUALITY_LOW:      baseMbps = BITRATE_LOW_MBPS;      break;
        case QUALITY_MEDIUM:   baseMbps = BITRATE_MEDIUM_MBPS;   break;
        case QUALITY_HIGH:     baseMbps = BITRATE_HIGH_MBPS;     break;
        case QUALITY_LOSSLESS: baseMbps = BITRATE_LOSSLESS_MBPS; break;
        default:               baseMbps = BITRATE_MEDIUM_MBPS;   break;
    }
    
    // Scale for resolution (base is 2560x1440 = 3.7MP)
    // Cast to size_t first to prevent int overflow on large resolutions
    float megapixels = (float)((size_t)width * (size_t)height) / 1000000.0f;
    float resScale = megapixels / BASE_RESOLUTION_MEGAPIXELS;
    if (resScale < MIN_RESOLUTION_SCALE) resScale = MIN_RESOLUTION_SCALE;
    if (resScale > MAX_RESOLUTION_SCALE) resScale = MAX_RESOLUTION_SCALE;
    
    // Scale for FPS (base is 60fps)
    float fpsScale = (float)fps / BASE_FPS;
    if (fpsScale < MIN_FPS_SCALE) fpsScale = MIN_FPS_SCALE;
    if (fpsScale > MAX_FPS_SCALE) fpsScale = MAX_FPS_SCALE;
    
    // Use double for intermediate calculation to avoid overflow
    double bitrateCalc = (double)baseMbps * resScale * fpsScale * 1000000.0;
    
    // Bounds: 10 Mbps minimum, 150 Mbps maximum
    if (bitrateCalc < MIN_BITRATE_BPS) bitrateCalc = MIN_BITRATE_BPS;
    if (bitrateCalc > MAX_BITRATE_BPS) bitrateCalc = MAX_BITRATE_BPS;
    
    UINT32 bitrate = (UINT32)bitrateCalc;
    
    return bitrate;
}

// Calculate precise timestamp for a frame (avoids cumulative rounding errors)
LONGLONG Util_CalculateTimestamp(int frameNumber, int fps) {
    // Preconditions
    LWSR_ASSERT(frameNumber >= 0);
    LWSR_ASSERT(fps > 0);
    
    // Using exact division: (frame * 10000000) / fps
    // This avoids cumulative rounding errors that occur with additive timing
    return (LONGLONG)frameNumber * MF_UNITS_PER_SECOND / fps;
}

// Calculate precise frame duration
LONGLONG Util_CalculateFrameDuration(int frameNumber, int fps) {
    // Preconditions
    LWSR_ASSERT(frameNumber >= 0);
    LWSR_ASSERT(fps > 0);
    
    // Duration = next_timestamp - this_timestamp
    LONGLONG thisTime = Util_CalculateTimestamp(frameNumber, fps);
    LONGLONG nextTime = Util_CalculateTimestamp(frameNumber + 1, fps);
    return nextTime - thisTime;
}

// Get aspect ratio dimensions from config index
void Util_GetAspectRatioDimensions(int aspectIndex, int* ratioW, int* ratioH) {
    // Preconditions
    LWSR_ASSERT(ratioW != NULL);
    LWSR_ASSERT(ratioH != NULL);
    
    if (!ratioW || !ratioH) return;
    
    // 0=Native, 1=16:9, 2=9:16, 3=1:1, 4=4:5, 5=16:10, 6=4:3, 7=21:9, 8=32:9
    switch (aspectIndex) {
        case 1: *ratioW = 16; *ratioH = 9;  break;  // 16:9 (YouTube, Standard)
        case 2: *ratioW = 9;  *ratioH = 16; break;  // 9:16 (TikTok, Shorts, Reels)
        case 3: *ratioW = 1;  *ratioH = 1;  break;  // 1:1 (Square - Instagram)
        case 4: *ratioW = 4;  *ratioH = 5;  break;  // 4:5 (Instagram Portrait)
        case 5: *ratioW = 16; *ratioH = 10; break;  // 16:10
        case 6: *ratioW = 4;  *ratioH = 3;  break;  // 4:3
        case 7: *ratioW = 21; *ratioH = 9;  break;  // 21:9 (Ultrawide)
        case 8: *ratioW = 32; *ratioH = 9;  break;  // 32:9 (Super Ultrawide)
        default: *ratioW = 0; *ratioH = 0;  break;  // Native (no change)
    }
}

// Calculate aspect ratio crop rectangle centered on source bounds
RECT Util_CalculateAspectRect(RECT sourceBounds, int ratioW, int ratioH) {
    int sourceW = sourceBounds.right - sourceBounds.left;
    int sourceH = sourceBounds.bottom - sourceBounds.top;
    
    int rectW, rectH;
    
    // Fit to source while maintaining aspect ratio
    if (sourceW * ratioH > sourceH * ratioW) {
        // Source is wider than aspect ratio - fit to height
        rectH = sourceH;
        rectW = (rectH * ratioW) / ratioH;
    } else {
        // Source is taller than aspect ratio - fit to width
        rectW = sourceW;
        rectH = (rectW * ratioH) / ratioW;
    }
    
    // Ensure dimensions are even (required for H.264)
    rectW = (rectW / 2) * 2;
    rectH = (rectH / 2) * 2;
    
    // Center on source
    RECT result;
    result.left = sourceBounds.left + (sourceW - rectW) / 2;
    result.top = sourceBounds.top + (sourceH - rectH) / 2;
    result.right = result.left + rectW;
    result.bottom = result.top + rectH;
    
    return result;
}

// ============================================================================
// String Conversion Utilities
// ============================================================================

// Convert wide string to UTF-8
int Util_WideToUtf8(const WCHAR* wide, char* utf8, int maxLen) {
    // Preconditions
    LWSR_ASSERT(wide != NULL);
    LWSR_ASSERT(utf8 != NULL);
    LWSR_ASSERT(maxLen > 0);
    
    if (!wide || !utf8 || maxLen <= 0) return 0;
    int result = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, maxLen, NULL, NULL);
    if (result > 0) result--;  // Exclude null terminator from count
    return result;
}

// Convert UTF-8 string to wide string
int Util_Utf8ToWide(const char* utf8, WCHAR* wide, int maxLen) {
    // Preconditions
    LWSR_ASSERT(utf8 != NULL);
    LWSR_ASSERT(wide != NULL);
    LWSR_ASSERT(maxLen > 0);
    
    if (!utf8 || !wide || maxLen <= 0) return 0;
    int result = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, maxLen);
    if (result > 0) result--;  // Exclude null terminator from count
    return result;
}

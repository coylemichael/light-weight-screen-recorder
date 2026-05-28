/*
 * nvenc_encoder.h - NVENC Hardware Encoder (CUDA Path)
 *
 * SHARED BY: replay_buffer.c, recording.c
 *
 * HEVC hardware encoding via NVIDIA NVENC API.
 *
 * Thread-safety contract:
 *   - NVENCEncoder is NOT thread-safe. All operations on a given encoder
 *     instance (SubmitFrame, SubmitTexture, GetSequenceHeader, GetQP,
 *     GetFrameSizeStats, Destroy) MUST be called from a single owning
 *     thread. The underlying CUDA context is thread-affine; calling from
 *     a different thread will push the context onto the wrong thread and
 *     corrupt encoder state.
 *   - NVENCEncoder_Create may be called concurrently from multiple
 *     threads; module-level CUDA bootstrap is guarded internally.
 */

#ifndef NVENC_ENCODER_H
#define NVENC_ENCODER_H

#include <windows.h>
#include <d3d11.h>
#include "config.h"

typedef struct {
    BYTE* data;
    DWORD size;
    LONGLONG timestamp;
    LONGLONG duration;
    BOOL isKeyframe;
} EncodedFrame;

typedef struct NVENCEncoder NVENCEncoder;

// Callback for receiving completed frames
typedef void (*EncodedFrameCallback)(EncodedFrame* frame, void* userData);

// Create encoder (D3D11 device ignored - uses CUDA internally)
NVENCEncoder* NVENCEncoder_Create(ID3D11Device* d3dDevice, int width, int height, int fps, QualityPreset quality);

// Set callback for completed frames
void NVENCEncoder_SetCallback(NVENCEncoder* enc, EncodedFrameCallback callback, void* userData);

// Submit NV12 frame for encoding (CPU buffers)
// data[0] = Y plane, data[1] = UV plane
// linesize[0] = Y stride, linesize[1] = UV stride
// Returns: 1 = success, 0 = failure
int NVENCEncoder_SubmitFrame(NVENCEncoder* enc, BYTE* data[2], int linesize[2], LONGLONG timestamp);

// Submit D3D11 texture for encoding (reads back to CPU internally)
// Returns: 1 = success, 0 = failure
int NVENCEncoder_SubmitTexture(NVENCEncoder* enc, ID3D11Texture2D* nv12Texture, LONGLONG timestamp);

// Get sequence header (VPS/SPS/PPS for HEVC)
BOOL NVENCEncoder_GetSequenceHeader(NVENCEncoder* enc, BYTE* buffer, DWORD bufferSize, DWORD* outSize);

// Stats
int NVENCEncoder_GetQP(NVENCEncoder* enc);
void NVENCEncoder_GetFrameSizeStats(NVENCEncoder* enc, UINT32* lastSize, UINT32* minSize, UINT32* maxSize, UINT32* avgSize);

// Cleanup
void NVENCEncoder_Destroy(NVENCEncoder* enc);

#endif

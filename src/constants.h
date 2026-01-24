/*
 * constants.h - Centralized Configuration Constants
 * ============================================================================
 * 
 * This header consolidates all "magic numbers" used throughout the codebase
 * into named constants. Magic numbers are raw numeric literals embedded in
 * code (like `timeout = 500`) that are problematic because:
 * 
 *   1. Their meaning is unclear without context
 *   2. The same value might be duplicated in multiple places
 *   3. Changing a value requires finding all occurrences
 *   4. Typos create subtle bugs that are hard to track down
 * 
 * By defining constants here, we gain:
 *   - Self-documenting code (MUTEX_ACQUIRE_TIMEOUT_MS vs 500)
 *   - Single point of change for tuning parameters
 *   - Compile-time validation of constant names
 *   - Clear documentation of why each value was chosen
 * 
 * USAGE: Include this header in any source file that needs these constants.
 * All values are compile-time constants using #define for zero runtime cost.
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <assert.h>

/* ============================================================================
 * ASSERTION MACROS - Debug-time Sanity Checks
 * ============================================================================
 * 
 * LWSR_ASSERT: Custom assertion macro that logs the failure before crashing.
 * This provides better diagnostics than standard assert() because:
 *   1. The failure message is written to our log file before abort
 *   2. We can see what went wrong even if the debugger isn't attached
 *   3. The log file persists after the crash for post-mortem analysis
 * 
 * LWSR_ASSERT_MSG: Like LWSR_ASSERT but with a custom message for context.
 * 
 * These macros are active in both debug and release builds by default.
 * To disable assertions in release builds, define LWSR_DISABLE_ASSERTS.
 * 
 * Usage:
 *   LWSR_ASSERT(ptr != NULL);
 *   LWSR_ASSERT_MSG(index >= 0, "Buffer index underflow");
 *   LWSR_ASSERT(count <= capacity);
 */

// Forward declaration of Logger_Log (to avoid circular include)
void Logger_Log(const char* fmt, ...);

#ifdef LWSR_DISABLE_ASSERTS
    #define LWSR_ASSERT(expr)          ((void)0)
    #define LWSR_ASSERT_MSG(expr, msg) ((void)0)
#else
    #define LWSR_ASSERT(expr) \
        do { \
            if (!(expr)) { \
                Logger_Log("ASSERTION FAILED: %s at %s:%d in %s()\n", \
                           #expr, __FILE__, __LINE__, __func__); \
                assert(expr); \
            } \
        } while (0)
    
    #define LWSR_ASSERT_MSG(expr, msg) \
        do { \
            if (!(expr)) { \
                Logger_Log("ASSERTION FAILED: %s - %s at %s:%d in %s()\n", \
                           #expr, msg, __FILE__, __LINE__, __func__); \
                assert(expr); \
            } \
        } while (0)
#endif

/* ============================================================================
 * TIME UNITS - Media Foundation and Windows Timing
 * ============================================================================
 * 
 * Media Foundation (Microsoft's multimedia framework) measures time in
 * "100-nanosecond units" also called "reference time" or "HNSTIME".
 * This is an extremely precise unit where:
 * 
 *   1 second        = 10,000,000 units (10 million)
 *   1 millisecond   = 10,000 units
 *   1 microsecond   = 10 units
 *   1 unit          = 100 nanoseconds
 * 
 * This precision is necessary for A/V synchronization where even small
 * timing errors accumulate into noticeable audio/video drift.
 * 
 * The "LL" suffix creates a 64-bit integer literal, required because
 * 32-bit integers overflow at ~4 billion (only 7 minutes in MF units).
 */
#define MF_UNITS_PER_SECOND         10000000LL
#define MF_UNITS_PER_MILLISECOND    10000LL

/* ============================================================================
 * PIXEL FORMAT SIZES
 * ============================================================================
 * 
 * Different pixel formats require different amounts of memory per pixel.
 * These constants prevent errors when calculating buffer sizes.
 * 
 * BGRA (Blue-Green-Red-Alpha): 4 bytes per pixel
 *   - This is the native format for Windows desktop composition (DWM)
 *   - The alpha channel is typically unused but present for alignment
 *   - Desktop Duplication API provides frames in this format
 * 
 * RGB24: 3 bytes per pixel (no alpha channel, rarely used in this codebase)
 */
#define BYTES_PER_PIXEL_BGRA        4
#define BYTES_PER_PIXEL_RGB24       3

/* ============================================================================
 * VIDEO CAPTURE DEFAULTS
 * ============================================================================
 * 
 * DEFAULT_REFRESH_RATE: Assumed monitor refresh rate when detection fails.
 *   Most modern monitors are 60Hz, though gaming monitors may be 144Hz+.
 *   We use this as a safe fallback that won't overtax the system.
 * 
 * DEFAULT_FPS: Target capture framerate. Recording at 60fps matches most
 *   monitor refresh rates and produces smooth video. Higher values increase
 *   file sizes proportionally without visible benefit on 60Hz displays.
 * 
 * LWSR_MAX_MONITORS: Maximum number of monitors we'll enumerate.
 *   Named with LWSR_ prefix to avoid conflict with Windows SDK's MAX_MONITORS
 *   defined in ddeml.h. 8 monitors covers virtually all real-world setups.
 * 
 * FRAME_ACQUIRE_TIMEOUT_MS: How long to wait for a new frame from Desktop
 *   Duplication API before giving up. At 60fps, frames arrive every ~16.67ms,
 *   so 16ms is slightly less than one frame period. If no frame arrives in
 *   this time, we reuse the previous frame (desktop is static).
 * 
 * GOP_LENGTH_SECONDS: "Group of Pictures" interval - how often to insert a
 *   keyframe (I-frame) in the video stream. Keyframes are complete images
 *   that don't depend on previous frames, enabling seeking. 2 seconds is a
 *   good balance between file size (more keyframes = larger) and seek
 *   responsiveness (fewer keyframes = longer seek times).
 * 
 * DWMWA_EXTENDED_FRAME_BOUNDS_CONST: DWM (Desktop Window Manager) attribute
 *   for getting a window's actual visible bounds excluding shadows.
 *   Value 9 corresponds to DWMWA_EXTENDED_FRAME_BOUNDS, but we define it
 *   ourselves for compatibility with older Windows SDK headers.
 */
#define DEFAULT_REFRESH_RATE        60
#define DEFAULT_FPS                 60
#define LWSR_MAX_MONITORS           8
#define FRAME_ACQUIRE_TIMEOUT_MS    16
#define GOP_LENGTH_SECONDS          2
#define DWMWA_EXTENDED_FRAME_BOUNDS_CONST 9

/* ============================================================================
 * NVENC QUALITY PRESETS - Quantization Parameter (QP) Values
 * ============================================================================
 * 
 * NVENC (NVIDIA's hardware encoder) uses "Constant QP" mode where you specify
 * a fixed quality level rather than a target bitrate. The Quantization
 * Parameter (QP) controls how aggressively the encoder compresses the image:
 * 
 *   QP 0  = Mathematically lossless (huge files, ~500+ Mbps)
 *   QP 15-20 = Visually lossless (can't see compression artifacts)
 *   QP 20-25 = High quality (minor artifacts visible on close inspection)
 *   QP 25-30 = Medium quality (noticeable on detailed scenes)
 *   QP 30-40 = Low quality (obvious compression, small files)
 *   QP 51 = Maximum compression (terrible quality)
 * 
 * Our presets are tuned for screen recording where sharp text and UI elements
 * are important. We use lower QP values (higher quality) than typical video
 * because compression artifacts on text are very noticeable.
 * 
 * QP_INTRA_OFFSET: Keyframes (I-frames) use a slightly lower QP than
 *   predicted frames (P-frames). This gives keyframes extra quality since
 *   they're the reference point for subsequent frames. Errors in keyframes
 *   propagate to dependent frames, so higher keyframe quality improves
 *   overall video quality.
 */
#define QP_LOW                      28      // Smaller files, some artifacts
#define QP_MEDIUM                   24      // Good balance
#define QP_HIGH                     20      // High quality, larger files
#define QP_LOSSLESS                 16      // Near-lossless, very large files
#define QP_INTRA_OFFSET             4       // Keyframe QP = QP - 4

/* ============================================================================
 * BUFFER MANAGEMENT - Sample Storage and Memory
 * ============================================================================
 * 
 * The recording pipeline uses ring buffers to store encoded video/audio
 * samples before they're written to disk. These constants control buffer
 * sizing and growth behavior.
 * 
 * MIN/MAX_BUFFER_CAPACITY: Hard limits on how many samples a buffer can hold.
 *   - Minimum prevents thrashing from constant reallocation on tiny buffers
 *   - Maximum prevents runaway memory consumption if something goes wrong
 *   At 60fps, 100 samples = 1.67 seconds, 100000 samples = 27+ minutes
 * 
 * BUFFER_CAPACITY_HEADROOM: When calculating required capacity, multiply
 *   by this factor to provide breathing room. A value of 1.5 means we
 *   allocate 50% more than the minimum needed, reducing the frequency of
 *   expensive reallocations.
 * 
 * NVENC_NUM_BUFFERS: Number of buffers in NVENC's async encoding pipeline.
 *   NVIDIA recommends minimum 4 for async operation. We use 8 to provide
 *   better pipelining - while one buffer is being encoded, others can be
 *   filled with new frames. More buffers increase latency slightly but
 *   improve throughput and reduce dropped frames under load.
 * 
 * MAX_SEQ_HEADER_SIZE: Maximum size for HEVC sequence headers (VPS/SPS/PPS).
 *   These headers contain codec configuration and must be stored separately
 *   for MP4 muxing. 256 bytes is generous; typical headers are 50-100 bytes.
 * 
 * CONFIG_BUFFER_SIZE: Scratch buffer for reading config file values.
 *   32 chars is enough for any numeric value or short string.
 */
#define MIN_BUFFER_CAPACITY         100
#define MAX_BUFFER_CAPACITY         100000
#define BUFFER_CAPACITY_HEADROOM    1.5f
#define NVENC_NUM_BUFFERS           8
#define MAX_SEQ_HEADER_SIZE         256
#define CONFIG_BUFFER_SIZE          32

/* ============================================================================
 * AUDIO BUFFER MANAGEMENT
 * ============================================================================
 * 
 * Audio uses dynamic arrays that grow as needed during recording.
 * 
 * INITIAL_AUDIO_CAPACITY: Starting size for audio sample arrays.
 *   1024 samples at 48kHz stereo = ~10ms of audio. We start small and grow.
 * 
 * AUDIO_CAPACITY_GROWTH_FACTOR: When an array fills up, multiply its
 *   capacity by this factor. Doubling (2x) is the classic approach that
 *   amortizes reallocation cost - we do fewer large allocations rather
 *   than many small ones.
 * 
 * AUDIO_MIX_CHUNK_SIZE: When mixing multiple audio sources (microphone +
 *   desktop audio), we process this many samples at a time. 4096 samples
 *   is a good balance between processing overhead (fewer chunks = less
 *   overhead) and memory locality (smaller chunks = better cache usage).
 * 
 * AUDIO_DEVICE_ID_MAX_LEN: Maximum length for audio device ID strings.
 *   Windows audio device IDs can be quite long (GUIDs + path info).
 */
#define INITIAL_AUDIO_CAPACITY          1024
#define AUDIO_CAPACITY_GROWTH_FACTOR    2
#define AUDIO_MIX_CHUNK_SIZE            4096
#define AUDIO_DEVICE_ID_MAX_LEN         256

/* ============================================================================
 * AUDIO FORMAT CONSTANTS - Sample Value Normalization
 * ============================================================================
 * 
 * Audio samples come in various bit depths. To mix and process them, we
 * normalize to floating point values in the range [-1.0, 1.0].
 * 
 * 16-BIT AUDIO (CD quality, most common):
 *   - Signed integers from -32768 to +32767
 *   - Divide by 32768.0 to normalize to [-1.0, 1.0]
 *   - AUDIO_16BIT_MAX_SIGNED (32767) used when clamping output
 * 
 * 24-BIT AUDIO (professional/high-end):
 *   - Stored as 3 bytes per sample (not aligned to a standard int type)
 *   - Range: -8388608 to +8388607
 *   - Requires manual sign extension since there's no native 24-bit int
 *   - SIGN_MASK identifies negative values (high bit set)
 *   - SIGN_EXTEND fills upper byte with 1s for negative values
 * 
 * AUDIO_VOLUME constants define the user-facing volume slider range (0-100%).
 */
#define AUDIO_16BIT_MAX             32768.0f
#define AUDIO_16BIT_MAX_SIGNED      32767.0f
#define AUDIO_24BIT_MAX             8388608.0f
#define AUDIO_24BIT_SIGN_MASK       0x800000
#define AUDIO_24BIT_SIGN_EXTEND     0xFF000000
#define AUDIO_VOLUME_MIN            0
#define AUDIO_VOLUME_MAX            100
#define AUDIO_VOLUME_DEFAULT        100

/* ============================================================================
 * REPLAY BUFFER CONFIGURATION
 * ============================================================================
 * 
 * The replay buffer continuously records to memory, keeping the last N
 * seconds available for saving. This enables "save what just happened"
 * functionality like NVIDIA ShadowPlay's Instant Replay.
 * 
 * REPLAY_DURATION_*: Bounds for how much history to keep.
 *   - Minimum 1 second allows short clips if desired
 *   - Maximum 1200 seconds (20 minutes) prevents excessive RAM usage
 *   - Default 15 seconds provides quick saves with minimal RAM usage
 * 
 * Memory usage depends heavily on video settings. At 1080p60 high quality,
 * expect roughly 100-150 MB per minute of buffer. A 20-minute buffer could
 * use 2-3 GB of RAM.
 */
#define REPLAY_DURATION_MIN_SECS    1
#define REPLAY_DURATION_MAX_SECS    1200
#define REPLAY_DURATION_DEFAULT     15

/* ============================================================================
 * TIMEOUT VALUES - Thread Synchronization and Waiting
 * ============================================================================
 * 
 * Timeouts prevent deadlocks and ensure the program remains responsive
 * even when things go wrong. All values in milliseconds.
 * 
 * MUTEX_ACQUIRE_TIMEOUT_MS: How long to wait for a mutex/keyed mutex.
 *   Keyed mutexes coordinate GPU resource access between the capture
 *   device (writing frames) and encode device (reading frames). 100ms
 *   is long enough for any legitimate delay but short enough to detect
 *   real problems quickly.
 * 
 * EVENT_WAIT_TIMEOUT_MS: Timeout for Windows event objects.
 *   Events signal when async operations complete (e.g., encoding finished).
 *   100ms allows periodic checking without busy-waiting.
 * 
 * THREAD_JOIN_TIMEOUT_MS: How long to wait for a thread to exit during
 *   shutdown. 5 seconds is generous - if a thread hasn't exited by then,
 *   something is stuck and we should warn the user.
 * 
 * AUDIO_POLL_INTERVAL_MS: How often to check for new audio data.
 *   WASAPI provides audio in chunks; 5ms polling ensures we catch new
 *   data promptly without excessive CPU usage.
 * 
 * DORMANT_THRESHOLD_MS: If no audio data arrives for this long, consider
 *   the audio device dormant (possibly muted or disconnected).
 */
#define MUTEX_ACQUIRE_TIMEOUT_MS    100
#define EVENT_WAIT_TIMEOUT_MS       100
#define THREAD_JOIN_TIMEOUT_MS      5000
#define AUDIO_POLL_INTERVAL_MS      5
#define DORMANT_THRESHOLD_MS        100.0

/* ============================================================================
 * ERROR HANDLING AND LOGGING THRESHOLDS
 * ============================================================================
 * 
 * These constants prevent log spam and enable graceful degradation when
 * errors occur repeatedly.
 * 
 * MAX_CONSECUTIVE_ERRORS: After this many consecutive errors from an
 *   operation (e.g., audio capture), stop retrying and consider it failed.
 *   Prevents infinite loops when something is fundamentally broken.
 * 
 * LOG_RATE_LIMIT: For high-frequency warnings (e.g., "frame dropped"),
 *   only log every Nth occurrence. Prevents log files from growing to
 *   gigabytes when something is persistently wrong.
 * 
 * EVICT_LOG_INTERVAL / AUDIO_EVICT_LOG_INTERVAL: When evicting old samples
 *   from buffers (normal operation in replay mode), only log occasionally.
 *   Buffer eviction happens constantly; we don't need to log every one.
 * 
 * MAX_REALLOC_FAIL_LOGS: How many times to log memory reallocation failures
 *   before going silent. If we're out of memory, spamming logs won't help.
 * 
 * EMERGENCY_KEEP_FRACTION: When buffer overflow forces emergency eviction,
 *   keep this fraction of samples (75%). Provides some safety margin while
 *   freeing enough space to continue operation.
 */
#define MAX_CONSECUTIVE_ERRORS      100
#define LOG_RATE_LIMIT              100
#define EVICT_LOG_INTERVAL          300
#define AUDIO_EVICT_LOG_INTERVAL    500
#define MAX_REALLOC_FAIL_LOGS       5
#define EMERGENCY_KEEP_FRACTION     0.75f

/* ============================================================================
 * BITRATE CALCULATION - Adaptive Quality Scaling
 * ============================================================================
 * 
 * Rather than hardcoding bitrates, we calculate them based on resolution
 * and framerate, similar to NVIDIA ShadowPlay. This ensures appropriate
 * quality across different capture scenarios.
 * 
 * BASE BITRATES: Target Mbps for each quality level at 1440p60.
 *   These values are calibrated for screen recording where sharp text
 *   and UI elements matter. They're higher than typical video bitrates
 *   because screen content has more fine detail than camera footage.
 * 
 * BASE_RESOLUTION_MEGAPIXELS: Reference point for scaling (2560×1440).
 *   2560 * 1440 = 3,686,400 pixels ≈ 3.7 megapixels
 * 
 * BASE_FPS: Reference framerate for scaling calculations.
 * 
 * SCALE BOUNDS: Limits on how much resolution/fps affect bitrate.
 *   - MIN/MAX_RESOLUTION_SCALE: Prevents extreme bitrates for unusual
 *     resolutions. A 4K capture doesn't need 4x the bitrate of 1080p.
 *   - MIN/MAX_FPS_SCALE: Similar bounds for framerate. 120fps doesn't
 *     need double the bitrate of 60fps.
 * 
 * BITRATE_BPS BOUNDS: Absolute limits regardless of calculation.
 *   - Minimum 10 Mbps ensures usable quality even for tiny captures
 *   - Maximum 150 Mbps prevents unreasonable file sizes
 */
#define BITRATE_LOW_MBPS            60.0f
#define BITRATE_MEDIUM_MBPS         75.0f
#define BITRATE_HIGH_MBPS           90.0f
#define BITRATE_LOSSLESS_MBPS       130.0f
#define BASE_RESOLUTION_MEGAPIXELS  3.7f
#define BASE_FPS                    60.0f
#define MIN_RESOLUTION_SCALE        0.5f
#define MAX_RESOLUTION_SCALE        2.5f
#define MIN_FPS_SCALE               0.5f
#define MAX_FPS_SCALE               2.0f
#define MIN_BITRATE_BPS             10000000.0
#define MAX_BITRATE_BPS             150000000.0

/* ============================================================================
 * AAC AUDIO ENCODER
 * ============================================================================
 * 
 * AAC-LC (Low Complexity) is the most widely compatible AAC profile,
 * supported by virtually all devices and players.
 * 
 * AAC_LC_PROFILE_LEVEL: MPEG-4 Audio Object Type + Profile Level.
 *   0x29 = 41 decimal = AAC-LC at Level 2 (supports up to 48kHz stereo).
 *   This value is written to the MP4 container's audio config.
 * 
 * AAC_OUTPUT_BUFFER_SIZE: Scratch buffer for encoded AAC frames.
 *   8KB is generous; AAC frames at typical bitrates are 200-500 bytes.
 */
#define AAC_LC_PROFILE_LEVEL        0x29
#define AAC_OUTPUT_BUFFER_SIZE      8192

/* ============================================================================
 * WASAPI AUDIO CAPTURE
 * ============================================================================
 * 
 * WASAPI (Windows Audio Session API) is the low-level audio API we use
 * for capturing system audio and microphone input.
 * 
 * WASAPI_BUFFER_DURATION_100NS: Requested audio buffer size in 100ns units.
 *   1,000,000 units = 100 milliseconds of audio buffering.
 *   Larger buffers reduce CPU overhead but increase latency.
 *   100ms is a good balance for recording (latency doesn't matter).
 */
#define WASAPI_BUFFER_DURATION_100NS 1000000

/* ============================================================================
 * VIDEO CODEC PROFILES
 * ============================================================================
 * 
 * H264_PROFILE_HIGH: The H.264 High Profile (100) enables advanced
 *   compression features like 8x8 transforms and CABAC entropy coding.
 *   This produces smaller files than Baseline or Main profiles at the
 *   same quality, and is universally supported by modern decoders.
 *   
 *   Value 100 corresponds to eAVEncH264VProfile_High in Media Foundation.
 */
#define H264_PROFILE_HIGH           100

/* ============================================================================
 * USER INTERFACE CONSTANTS
 * ============================================================================
 * 
 * CONTROL_WINDOW_TOP_OFFSET: The floating control toolbar appears this
 *   many pixels below the top of the captured region. 80px keeps it
 *   visible but out of the way of most title bars.
 * 
 * GDIP_STROKE_OFFSET: When drawing with GDI+, lines are positioned by
 *   their center. Adding 0.5 to coordinates aligns strokes to pixel
 *   boundaries for crisp rendering.
 * 
 * BORDER_COLOR_*: RGB components of the recording indicator border.
 *   Red (220, 50, 50) is the traditional "recording" color and stands
 *   out against most content without being eye-searing.
 */
#define CONTROL_WINDOW_TOP_OFFSET   80
#define GDIP_STROKE_OFFSET          0.5f
#define BORDER_COLOR_R              220
#define BORDER_COLOR_G              50
#define BORDER_COLOR_B              50

/* ============================================================================
 * WINDOWS MESSAGE IDS - Custom Application Messages
 * ============================================================================
 * 
 * Windows reserves WM_USER and above for application-defined messages.
 * We use these for inter-thread/inter-process communication.
 * 
 * WM_LWSR_STOP: Posted to trigger recording stop from another instance
 *   or from a global hotkey. Using a named constant prevents collisions
 *   if we add more custom messages later.
 */
#define WM_LWSR_STOP                (WM_USER + 1)

/* ============================================================================
 * FALLBACK FILE PATHS
 * ============================================================================
 * 
 * FALLBACK_RECORDINGS_PATH: Where to save recordings if the configured
 *   path is invalid or inaccessible. C:\Recordings is a reasonable default
 *   that exists on all Windows systems (we create it if needed).
 */
#define FALLBACK_RECORDINGS_PATH    "C:\\Recordings"

/* ============================================================================
 * AAC ENCODER CONSTANTS
 * ============================================================================
 * 
 * Audio encoding parameters for AAC-LC encoding via Media Foundation.
 * 
 * AAC_SAMPLE_RATE: 48kHz is the standard sample rate for video production.
 *   This matches most HDMI audio and ensures best quality with WASAPI loopback.
 * 
 * AAC_CHANNELS: Stereo (2 channels) for standard desktop audio.
 * 
 * AAC_BITRATE: 192kbps provides excellent quality for stereo AAC-LC.
 *   This is the sweet spot between file size and transparency.
 * 
 * AAC_LC_PROFILE_LEVEL: AAC-LC (Low Complexity) profile level indication.
 *   0x29 = Level 2 which supports up to 48kHz stereo.
 * 
 * AAC_OUTPUT_BUFFER_SIZE: Buffer size for encoded AAC output frames.
 *   8KB is sufficient for 192kbps AAC at any frame size.
 */
#define AAC_SAMPLE_RATE             48000
#define AAC_CHANNELS                2
#define AAC_BITRATE                 192000
#define AAC_LC_PROFILE_LEVEL        0x29
#define AAC_OUTPUT_BUFFER_SIZE      8192

/* ============================================================================
 * OVERLAY UI CONSTANTS
 * ============================================================================
 * 
 * Layout and timing constants for the overlay selection UI.
 * 
 * CONTROL_PANEL_WIDTH/HEIGHT: Dimensions of the floating control toolbar.
 *   730px width accommodates capture mode buttons + icon buttons.
 *   44px height is compact but touch-friendly.
 * 
 * CROSSHAIR_SIZE: Size of the dimension indicator that follows the cursor
 *   during area selection. Shows current width x height.
 * 
 * SELECTION_HANDLE_SIZE: Diameter of the resize handles shown at corners
 *   and edges of a completed selection rectangle.
 * 
 * OVERLAY_HIDE_SETTLE_MS: Wait time after hiding overlay windows before
 *   capturing the screen. This allows the window manager to complete the
 *   hide animation and repaint the underlying content. Without this delay,
 *   captures may include ghosting or partially-hidden overlay elements.
 *   Note: This uses RedrawWindow + Sleep rather than just Sleep because
 *   we need to ensure the system has actually processed the hide request.
 */
#define CONTROL_PANEL_WIDTH         730
#define CONTROL_PANEL_HEIGHT        44
#define CROSSHAIR_SIZE              80
#define SELECTION_HANDLE_SIZE       10
#define OVERLAY_HIDE_SETTLE_MS      50

#endif // CONSTANTS_H

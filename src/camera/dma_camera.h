#pragma once
// ── dma_camera.h ──────────────────────────────────────────────────────────────
// Zero-copy DMA camera using libcamera + EGL dmabuf import.
//
// Frames are captured by the ISP in NV12 format and their dmabuf file
// descriptors are imported directly into OpenGL ES as EGLImages — the pixel
// data never touches the CPU.  A GLSL ES shader converts NV12 → RGB on the GPU.
//
// Thread model
// ─────────────
//  • capture thread  — runs libcamera event loop; writes to ready_slot_
//  • render  thread  — calls draw(); reads ready_slot_
//
// Buffer state machine (3 slots)
//   IDLE ──queue──► CAPTURING ──complete──► READY ──draw──► RENDERING
//    ▲                                                            │
//    └──────────────── return after draw ────────────────────────┘
//
// init() MUST be called from the render thread (after the GL context is current).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <array>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace libcamera {
    class Camera;
    class CameraConfiguration;
    class CameraManager;
    class ControlList;
    class FrameBuffer;
    class FrameBufferAllocator;
    class Request;
    class Stream;
}

class DmaCamera {
public:
    struct Config {
        int         libcamera_id = 0;
        std::string model_name;   // if non-empty, match camera by model string first
        std::string camera_id;    // exact libcamera id; if set, takes priority over the above
        int         width        = 1280;
        int         height       = 800;
        int         fps          = 60;
        // Display rotation applied in the NV12 vertex shader (no readback,
        // ~free). Snaps to one of {0, 90, 180, 270}; other values are
        // clamped to the nearest multiple at set time. Mounting protogen
        // CSI cameras sideways on the helmet is the main use case.
        int         rotation_deg = 0;
    };

    // A capture mode, tagged with where we learned about it (flags OR together
    // when sources agree on a size):
    //   kModeScaled — ISP-resizable size synthesized from the continuous range
    //   kModeSensor — discrete size the camera reported at startup (RAW stream
    //                 role = the kernel driver's real sensor mode list)
    //   kModeSpec   — manufacturer mode table, matched via the sensor name
    //                 (libcamera id / config.txt dtoverlay scan)
    // max_fps is 0 when unknown; the startup probe fills it from
    // FrameDurationLimits on the actual platform (spec nominals are fallback).
    static constexpr uint8_t kModeScaled = 1;
    static constexpr uint8_t kModeSensor = 2;
    static constexpr uint8_t kModeSpec   = 4;
    struct Mode {
        int     width   = 0;
        int     height  = 0;
        int     max_fps = 0;   // 0 = unknown (not yet probed)
        uint8_t src     = 0;   // kMode* flags
    };

    DmaCamera();
    ~DmaCamera();

    // Call after GL context is current.
    bool init(libcamera::CameraManager* lcam_mgr, const Config& cfg,
              const char* nv12_vs_path, const char* nv12_fs_path);
    void shutdown();

    // Draw the latest frame filling the current GL viewport.
    // zoom=1.0 → full frame; zoom>1.0 → digital crop around (cx,cy).
    // Returns false if no frame is available yet.
    bool draw(float zoom = 1.0f, float cx = 0.5f, float cy = 0.5f);

    // Hot-swap the capture resolution / frame-rate without destroying GL resources.
    // Stops capture, reconfigures libcamera, reallocates DMA buffers, restarts.
    // Must be called from the render thread. Returns false on failure (camera
    // reverts to previous config and capture is restarted).
    bool reconfigure(int width, int height, int fps);

    // ── Focus / Exposure control (libcamera controls API) ─────────────────────
    void start_autofocus();
    void stop_autofocus();
    void set_focus_position(int pos);   // 0-1000
    int  get_focus_position() const;
    bool is_af_locked() const;
    bool is_af_scanning() const;
    void set_exposure_ev(float ev);     // -3.0 to +3.0 (AE compensation; requires AE on)
    void set_shutter_speed_us(int us);  // microseconds (requires AE off / manual mode)
    void set_ae_enable(bool ae_on);     // true = auto-exposure, false = manual shutter

    // ── White balance control (libcamera controls API) ─────────────────────────
    void set_awb_enable(bool awb_on);              // true = auto WB, false = manual
    void set_colour_gains(float rg, float bg);     // raw ISP gains (typically 0.5–4.0)
    void set_colour_temp(int kelvin);              // maps 2800–7500 K → ColourGains LUT

    // ── Autofocus tuning (PDAF sensors: IMX708 / Arducam 16MP / 64MP) ──────────
    void set_af_range(int range);   // 0 Normal, 1 Macro, 2 Full
    void set_af_speed(int speed);   // 0 Normal, 1 Fast
    int  af_range() const { return cur_af_range_.load(); }
    int  af_speed() const { return cur_af_speed_.load(); }

    // ── Auto-exposure tuning ──────────────────────────────────────────────────
    void  set_analogue_gain(float gain);   // manual sensor gain (>=1.0); <=0 → auto
    void  set_ae_metering(int mode);       // 0 Centre-weighted, 1 Spot, 2 Matrix
    void  set_ae_constraint(int mode);     // 0 Normal, 1 Highlight, 2 Shadows
    void  set_ae_exposure_mode(int mode);  // 0 Normal, 1 Short, 2 Long
    void  set_flicker_mode(int mode);      // 0 Off, 1 Auto, 2 50 Hz, 3 60 Hz
    float analogue_gain_target() const { return cur_gain_.load(); }
    int   ae_metering()    const { return cur_ae_metering_.load(); }
    int   ae_constraint()  const { return cur_ae_constraint_.load(); }
    int   ae_exposure_mode() const { return cur_ae_exp_mode_.load(); }
    int   flicker_mode()   const { return cur_flicker_.load(); }

    // ── White-balance preset modes (alternative to manual gains / Kelvin) ──────
    void set_awb_mode(int mode);    // 0 Auto,1 Incandescent,2 Tungsten,3 Fluorescent,
                                    // 4 Indoor,5 Daylight,6 Cloudy (matches libcamera)
    int  awb_mode() const { return cur_awb_mode_.load(); }

    // ── ISP image tuning ──────────────────────────────────────────────────────
    void  set_brightness(float v);         // -1.0 .. 1.0  (0 = neutral)
    void  set_contrast(float v);           //  0.0 .. 2.0  (1 = neutral)
    void  set_saturation(float v);         //  0.0 .. 2.0  (1 = neutral, 0 = mono)
    void  set_sharpness(float v);          //  0.0 .. 2.0  (1 = neutral)
    void  set_noise_reduction(int mode);   // 0 Off,1 Fast,2 High Quality,3 Minimal
    float brightness() const { return cur_brightness_.load(); }
    float contrast()   const { return cur_contrast_.load(); }
    float saturation() const { return cur_saturation_.load(); }
    float sharpness()  const { return cur_sharpness_.load(); }
    int   noise_reduction() const { return cur_nr_.load(); }

    // ── On-sensor HDR (IMX708 — Camera Module 3 / Arducam Hawkeye) ─────────────
    // Values match libcamera HdrMode: 0 Off, 2 MultiExposure, 3 SingleExposure,
    // 4 Night. Needs a recent libcamera that exposes HdrMode as a runtime control.
    void set_hdr_mode(int mode);
    int  hdr_mode() const { return cur_hdr_.load(); }

    bool  is_ok()           const { return ok_; }
    int   width()           const { return cfg_.width; }
    int   height()          const { return cfg_.height; }
    int   fps()             const { return cfg_.fps; }
    const std::string& model_name() const { return cfg_.model_name; }
    const std::string& camera_id()  const { return cfg_.camera_id; }

    // Save the current full-resolution frame to `path` as a JPEG, by mapping the
    // libcamera NV12 buffer on the CPU and converting via OpenCV (so it works at
    // full sensor res without a huge GPU FBO). Render thread, NV12 only. Returns
    // false if there's no frame / the format isn't NV12.
    bool grab_still(const std::string& path);
    float analogue_gain()   const { return last_analogue_gain_.load(); }

    // Measured capture rate, derived from the per-frame FrameDuration metadata
    // (0 until the first frame completes). Lets the menu show the real fps the
    // sensor settled on rather than the requested target.
    float measured_fps()    const {
        int64_t us = last_frame_dur_us_.load();
        return us > 0 ? 1.0e6f / static_cast<float>(us) : 0.0f;
    }

    // Resolutions this sensor reports for the negotiated pixel format, captured
    // once during init(). Empty if init() hasn't run / the sensor reported none.
    // Sorted largest-area first. Each DmaCamera is one physical sensor, so this
    // is inherently per-camera.
    const std::vector<Mode>& supported_modes() const { return modes_; }

    // Display rotation (0, 90, 180, 270 — snapped). Live-tunable from any
    // thread; the next draw() picks up the change via an atomic read.
    void set_rotation(int deg);
    int  rotation() const { return rotation_deg_.load(); }

private:
    enum class SlotState { IDLE, CAPTURING, READY, RENDERING };

    struct Slot {
        libcamera::FrameBuffer* buffer  = nullptr;
        libcamera::Request*     request = nullptr;
        EGLImageKHR             img_y   = EGL_NO_IMAGE_KHR;
        EGLImageKHR             img_uv  = EGL_NO_IMAGE_KHR;
        SlotState               state   = SlotState::IDLE;
    };

    bool configure_camera();
    bool allocate_buffers_and_egl();
    bool create_egl_image(const libcamera::FrameBuffer* buf, Slot& slot);
    bool load_egl_procs();
    bool create_gl_resources(const char* vs_path, const char* fs_path);
    void start_capture();
    void event_loop();
    void on_request_complete(libcamera::Request* req);
    // Apply any pending control updates to a request's ControlList before requeuing.
    void apply_pending_controls(libcamera::ControlList& ctrls);

    // Negotiated pixel format (set by configure_camera before allocate_buffers_and_egl)
    uint32_t fmt_drm_    = 0x3231564Eu; // DRM_FORMAT_NV12 default
    int      fmt_planes_ = 2;           // number of DMA planes for the format

    // libcamera
    libcamera::CameraManager*                         lcam_mgr_  = nullptr;
    std::shared_ptr<libcamera::Camera>                camera_;
    std::unique_ptr<libcamera::CameraConfiguration>   cam_cfg_;
    std::unique_ptr<libcamera::FrameBufferAllocator>  allocator_;
    libcamera::Stream*                                stream_    = nullptr;
    uint32_t                                          stride_    = 0;

    // Slots
    static constexpr int NUM_SLOTS = 3;
    std::array<Slot, NUM_SLOTS>                       slots_;
    // createRequest() returns a unique_ptr the caller owns — keep them here so
    // they're freed on shutdown/reconfigure. Slot::request is a raw view into
    // these (must stay valid while queued with the camera).
    std::vector<std::unique_ptr<libcamera::Request>>  requests_;
    std::unordered_map<libcamera::FrameBuffer*, int>  buf_to_slot_;
    std::unordered_map<libcamera::Request*, int>      req_to_slot_;

    // Frame handoff
    std::mutex handoff_mtx_;
    int        ready_slot_  = -1;
    int        render_slot_ = -1;

    // EGL
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC            pfn_create_image_   = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC           pfn_destroy_image_  = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_img_target_tex_ = nullptr;

    // GLES resources
    // tex_y_ is a GL_TEXTURE_EXTERNAL_OES texture bound to the full NV12 EGLImage.
    // No separate UV texture — the driver handles YCbCr→RGB via samplerExternalOES.
    GLuint tex_y_      = 0;
    GLuint nv12_prog_  = 0;
    GLuint quad_vbo_   = 0;
    GLint  loc_tex_y_  = -1;   // uniform location of "tex" (samplerExternalOES)
    GLint  loc_zoom_   = -1;   // uniform location of "u_zoom"
    GLint  loc_center_ = -1;   // uniform location of "u_center"
    GLint  loc_rot_    = -1;   // uniform location of "u_rotation_rad"

    // Live-tunable display rotation. Atomic so set_rotation() can be
    // called from menu / main without coordinating with the render thread.
    std::atomic<int> rotation_deg_ { 0 };

    // Capture thread — event_loop() blocks on stop_cv_ until shutdown (libcamera
    // dispatches events internally; the thread only has to exist, not poll).
    std::atomic<bool>       running_ { false };
    std::thread             event_thread_;
    std::mutex              stop_mtx_;
    std::condition_variable stop_cv_;

    // ── Pending controls ──────────────────────────────────────────────────────
    // Written by any thread (main/menu), consumed atomically by capture thread
    // before the next request is requeued.  Sentinel -1 / -9999 = no-op.
    std::atomic<int>   pending_af_mode_    { -1 };       // AfModeEnum value, -1 = no-op
    std::atomic<float> pending_ev_         { -9999.0f }; // ExposureValue, sentinel = no-op
    std::atomic<int>   pending_shutter_us_ { -1 };       // ExposureTime µs, -1 = no-op
    std::atomic<int>   pending_ae_enable_  { -1 };       // 1=on 0=off -1=no-op
    std::atomic<float> pending_lens_pos_   { -1.0f };    // LensPosition diopters, -1 = no-op
    std::atomic<int>   pending_awb_enable_ { -1 };       // AwbEnable: 1=on 0=off -1=no-op
    std::atomic<float> pending_rg_gain_    { -1.0f };    // ColourGains R, -1 = no-op
    std::atomic<float> pending_bg_gain_    { -1.0f };    // ColourGains B, -1 = no-op

    // Extended controls (AF tuning / AE tuning / WB mode / ISP image / HDR).
    // pending_* sentinels: ints -1 = no-op; floats -1.0f (or -9999.0f where the
    // valid range includes negatives, i.e. Brightness). Applied + reset in
    // apply_pending_controls(); each is guarded there by controls().count().
    std::atomic<int>   pending_af_range_     { -1 };
    std::atomic<int>   pending_af_speed_     { -1 };
    std::atomic<float> pending_gain_         { -1.0f };   // AnalogueGain, <=0 = no-op
    std::atomic<int>   pending_ae_metering_  { -1 };
    std::atomic<int>   pending_ae_constraint_{ -1 };
    std::atomic<int>   pending_ae_exp_mode_  { -1 };
    std::atomic<int>   pending_flicker_      { -1 };
    std::atomic<int>   pending_awb_mode_     { -1 };
    std::atomic<float> pending_brightness_   { -9999.0f }; // valid range incl. negatives
    std::atomic<float> pending_contrast_     { -1.0f };
    std::atomic<float> pending_saturation_   { -1.0f };
    std::atomic<float> pending_sharpness_    { -1.0f };
    std::atomic<int>   pending_nr_           { -1 };
    std::atomic<int>   pending_hdr_          { -1 };

    // Last-requested values, so the menu can show the active option / slider
    // position (pending_* are one-shot and reset after apply). Seeded to the
    // libcamera/ISP neutral defaults.
    std::atomic<int>   cur_af_range_     { 0 };
    std::atomic<int>   cur_af_speed_     { 0 };
    std::atomic<float> cur_gain_         { 0.0f };   // 0 = auto
    std::atomic<int>   cur_ae_metering_  { 0 };
    std::atomic<int>   cur_ae_constraint_{ 0 };
    std::atomic<int>   cur_ae_exp_mode_  { 0 };
    std::atomic<int>   cur_flicker_      { 0 };
    std::atomic<int>   cur_awb_mode_     { 0 };
    std::atomic<float> cur_brightness_   { 0.0f };
    std::atomic<float> cur_contrast_     { 1.0f };
    std::atomic<float> cur_saturation_   { 1.0f };
    std::atomic<float> cur_sharpness_    { 1.0f };
    std::atomic<int>   cur_nr_           { 0 };
    std::atomic<int>   cur_hdr_          { 0 };

    // ── Latest camera metadata (written by capture thread via atomics) ────────
    std::atomic<int>   last_af_state_      { 0 };        // AfState enum value
    std::atomic<float> last_lens_pos_      { 0.0f };     // LensPosition diopters
    std::atomic<float> last_analogue_gain_ { 1.0f };     // AnalogueGain (1.0 = ISO100 equiv)
    std::atomic<int64_t> last_frame_dur_us_ { 0 };       // FrameDuration µs (0 = no frame yet)

    // Sensor-reported capture modes for the negotiated format (filled in init()).
    std::vector<Mode> modes_;

    Config cfg_;
    bool   ok_ = false;
};

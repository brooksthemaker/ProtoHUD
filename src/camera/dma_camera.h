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
#include <memory>
#include <mutex>
#include <thread>
#include <array>
#include <unordered_map>

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
        int libcamera_id = 0;
        int width        = 1280;
        int height       = 800;
        int fps          = 60;
    };

    DmaCamera();
    ~DmaCamera();

    // Call after GL context is current.
    bool init(libcamera::CameraManager* lcam_mgr, const Config& cfg,
              const char* nv12_vs_path, const char* nv12_fs_path);
    void shutdown();

    // Draw the latest frame filling the current GL viewport.
    // Returns false if no frame is available yet.
    bool draw();

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

    bool is_ok()  const { return ok_; }
    int  width()  const { return cfg_.width; }
    int  height() const { return cfg_.height; }

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

    // Capture thread
    std::atomic<bool> running_ { false };
    std::thread       event_thread_;

    // ── Pending controls ──────────────────────────────────────────────────────
    // Written by any thread (main/menu), consumed atomically by capture thread
    // before the next request is requeued.  Sentinel -1 / -9999 = no-op.
    std::atomic<int>   pending_af_mode_    { -1 };       // AfModeEnum value, -1 = no-op
    std::atomic<float> pending_ev_         { -9999.0f }; // ExposureValue, sentinel = no-op
    std::atomic<int>   pending_shutter_us_ { -1 };       // ExposureTime µs, -1 = no-op
    std::atomic<int>   pending_ae_enable_  { -1 };       // 1=on 0=off -1=no-op
    std::atomic<float> pending_lens_pos_   { -1.0f };    // LensPosition diopters, -1 = no-op

    // ── Latest camera metadata (written by capture thread via atomics) ────────
    std::atomic<int>   last_af_state_      { 0 };        // AfState enum value
    std::atomic<float> last_lens_pos_      { 0.0f };     // LensPosition diopters

    Config cfg_;
    bool   ok_ = false;
};

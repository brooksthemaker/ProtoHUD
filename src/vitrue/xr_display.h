#pragma once
// ── xr_display.h ──────────────────────────────────────────────────────────────
// GLFW + EGL window management for the VITURE Beast XR glasses.
//
// Creates a GLFW window on the glasses monitor (or desktop fallback),
// allocates per-eye GLES FBOs, and composites them SBS on submit.
//
// Thread safety: eye_left()/eye_right()/submit_frame() — render thread only.
//               Device control calls — any thread.
//               Callbacks fire on internal SDK threads.

#include "../app_state.h"
#include "../gl_utils.h"

#include <GLFW/glfw3.h>
#include <atomic>
#include <functional>
#include <mutex>

typedef void* XRDeviceProviderHandle;

using GlassStateChangedCb = std::function<void(int state_id, int value)>;
using ImuPoseCb           = std::function<void(float roll, float pitch, float yaw)>;

struct XRConfig {
    int  product_id      = 0;
    int  monitor_index   = -1;   // -1 = auto (prefer 3840-wide monitor)
    int  target_fps      = 90;
    bool use_beast_camera = true;
    bool enable_imu       = true;
};

class XRDisplay {
public:
    explicit XRDisplay(const XRConfig& cfg);
    ~XRDisplay();

    // Opens the GLFW window and initialises the glasses SDK.
    // Must be called before any GL drawing. Returns false if glasses not found
    // (falls back to 1920×1080 desktop window).
    bool init();
    void shutdown();

    // Per-eye render targets. Bind fbo before drawing, unbind after.
    gl::Fbo& eye_left()  { return rt_left_;  }
    gl::Fbo& eye_right() { return rt_right_; }

    // Composite both eye FBOs side-by-side into the default framebuffer.
    // Does NOT swap — call present() afterwards (after ImGui overlay).
    void composite();

    // Swap the GLFW window buffer and poll events.
    // Call after composite() and after the ImGui overlay is rendered.
    void present();

    // Convenience: composite() + present() in one call (no ImGui overlay).
    void submit_frame() { composite(); present(); }

    // ── Device control ────────────────────────────────────────────────────────
    void set_brightness(int level);
    void set_3d_mode(bool is_3d);
    void set_dimming(int level);
    void set_hud_brightness(int level);
    void recenter_tracking();
    void toggle_gaze_lock();

    // ── Callbacks ─────────────────────────────────────────────────────────────
    void on_state_changed(GlassStateChangedCb cb) { state_cb_ = std::move(cb); }
    void on_imu_pose     (ImuPoseCb cb)           { imu_cb_   = std::move(cb); }

    // ── Info ──────────────────────────────────────────────────────────────────
    bool glasses_found()  const { return device_ != nullptr; }
    int  product_id()     const { return product_id_; }
    int  eye_width()      const { return eye_w_; }
    int  eye_height()     const { return eye_h_; }
    int  display_width()  const { return disp_w_; }
    int  display_height() const { return disp_h_; }

    GLFWwindow* glfw_window() const { return window_; }

    ImuPose get_latest_imu_pose() const {
        return { imu_roll_.load(), imu_pitch_.load(), imu_yaw_.load() };
    }

private:
    bool find_and_connect();
    void open_imu();
    void set_sbs_display_mode();
    GLFWmonitor* choose_monitor() const;
    bool create_blit_program();

    // SDK callbacks (static trampolines)
    static void s_state_cb(int id, int val);
    static void s_imu_cb  (float* data, uint64_t ts);
    static XRDisplay* s_instance_;

    XRConfig              cfg_;
    XRDeviceProviderHandle device_     = nullptr;
    int                   product_id_ = 0;
    int                   eye_w_      = 1920;
    int                   eye_h_      = 1080;
    int                   disp_w_     = 1920;
    int                   disp_h_     = 1080;

    GLFWwindow*  window_  = nullptr;
    gl::Fbo      rt_left_;
    gl::Fbo      rt_right_;

    // Blit program: composites eye FBO textures to the main framebuffer
    GLuint blit_prog_ = 0;
    GLuint quad_vbo_  = 0;

    GlassStateChangedCb   state_cb_;
    ImuPoseCb             imu_cb_;
    std::mutex            cb_mtx_;

    std::atomic<float>    imu_roll_  { 0.f };
    std::atomic<float>    imu_pitch_ { 0.f };
    std::atomic<float>    imu_yaw_   { 0.f };
};

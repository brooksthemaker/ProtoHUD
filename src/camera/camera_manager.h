#pragma once

#include "dma_camera.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GLES2/gl2.h>
#include <opencv2/videoio.hpp>

namespace libcamera { class CameraManager; }
class QrScanner;

struct CamConfig {
    int         libcamera_id = 0;
    std::string model_name;   // if non-empty, select camera by model string first
    int         width        = 1280;
    int         height       = 800;
    int         fps          = 60;
    // Display rotation applied in the NV12 vertex shader (0/90/180/270;
    // other values are snapped at set time). Seed value at startup; runtime
    // changes go through CameraManager::set_owl_*_rotation.
    int         rotation_deg = 0;
};

struct UsbCamConfig {
    std::string device;
    int   width             = 1280;
    int   height            = 720;
    int   fps               = 30;
    float brightness        = 1.0f;  // software multiplier: 1.0=normal, 2.0=double, 0.5=half
    // Exposure / framerate
    bool  dynamic_framerate = false;  // false = disable fps throttle from long exposures
    bool  auto_exposure     = true;   // false = manual exposure
    int   exposure_time     = 157;    // 1-5000 (100µs units), used when auto_exposure=false
    // White balance
    bool  auto_wb           = true;   // false = manual white balance
    int   wb_temp           = 4600;   // 2800-6500 K, used when auto_wb=false
    bool  flip              = false;  // true = rotate 180° (correct upside-down mount)
    // Adaptive software brightness compensation
    bool  auto_brightness        = false; // enable per-frame luma feedback loop
    float auto_brightness_target = 100.f; // target mean luma 0-255 (default ~39%)
};

// Which image source feeds a given eye / a multi-cam quadrant. CSI is the
// "this eye's own CSI" used by the per-eye pickers; CSI_LEFT / CSI_RIGHT name a
// specific OWLsight camera and are used by the multi-cam quadrant pickers.
enum class EyeSource { CSI, USB1, USB2, USB3, CSI_LEFT, CSI_RIGHT };

// CameraManager owns:
//   • Two DmaCamera instances (CSI cameras, default OWLsight) — zero-copy NV12 path
//   • Three OpenCV VideoCapture instances (USB cams) — CPU→GPU upload path
//
// CSI cameras: call draw_owl_left/right() inside an eye FBO (fills current viewport).
// USB cams:    call get_usb1/2/3(GLuint&) to fetch the latest RGBA texture id,
//              or call draw_tex_fullscreen(tex) to blit a USB frame into an eye FBO.
//
// init() MUST be called from the render thread, after the GL context is current.

class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    bool init(const CamConfig& left, const CamConfig& right,
              const UsbCamConfig& usb1, const UsbCamConfig& usb2,
              const UsbCamConfig& usb3,
              const char* nv12_vs_path, const char* nv12_fs_path);
    void shutdown();

    // ── CSI cameras (zero-copy DMA) ──────────────────────────────────────────
    // Draw directly into the current render target (fills viewport).
    // zoom=1.0 → full frame; zoom>1.0 → digital crop around (cx,cy).
    // Returns false if no frame is ready yet.
    bool draw_owl_left( float zoom = 1.0f, float cx = 0.5f, float cy = 0.5f);
    bool draw_owl_right(float zoom = 1.0f, float cx = 0.5f, float cy = 0.5f);

    // Blit a USB camera RGBA texture as a fullscreen quad into the currently-bound FBO.
    // Equivalent to the CSI draw path but sourced from a CPU-uploaded GL_TEXTURE_2D.
    // Returns false if tex == 0. Call from the render thread inside an active FBO.
    bool draw_tex_fullscreen(GLuint tex) const;

    // ── USB cameras (CPU→GPU upload) ──────────────────────────────────────────
    // Upload latest frame; out receives the GL texture id (0 if unavailable).
    // Returns true if a new frame was uploaded this call.
    bool get_usb1(GLuint& out);
    bool get_usb2(GLuint& out);
    bool get_usb3(GLuint& out);

    // ── Resolution hot-swap ───────────────────────────────────────────────────
    // Changes the OWLsight capture resolution/fps by re-initialising the
    // camera(s) at the new size — a clean release + fresh configure, the same
    // path used at boot. NOT an in-place libcamera reconfigure: on the Pi ISP
    // that throws "BackEnd::finalise: TDN output not enabled" (libpisp) and
    // aborts the process. Per-eye variants change only one camera's config.
    // Must be called from the render thread (DmaCamera::init needs the GL ctx).
    bool set_resolution(int width, int height, int fps);          // both eyes
    bool set_owl_left_resolution(int width, int height, int fps);
    bool set_owl_right_resolution(int width, int height, int fps);

    int current_width()  const { return owl_left_  ? owl_left_->width()  :
                                        owl_right_ ? owl_right_->width()  : 0; }
    int current_height() const { return owl_left_  ? owl_left_->height() :
                                        owl_right_ ? owl_right_->height() : 0; }

    // ── Direct camera access (focus/exposure control) ─────────────────────────
    // CSI display rotation (0, 90, 180, 270). Live-tunable; the next draw
    // picks up the change. Cheap — pure UV math in the NV12 vertex shader.
    void set_owl_left_rotation(int deg)  { if (owl_left_)  owl_left_->set_rotation(deg);  }
    void set_owl_right_rotation(int deg) { if (owl_right_) owl_right_->set_rotation(deg); }
    int  owl_left_rotation()  const { return owl_left_  ? owl_left_->rotation()  : 0; }
    int  owl_right_rotation() const { return owl_right_ ? owl_right_->rotation() : 0; }

    DmaCamera* owl_left()  { return owl_left_.get();  }
    DmaCamera* owl_right() { return owl_right_.get(); }

    // Configured model name for each CSI camera, or empty if the slot is absent.
    std::string owl_left_model()  const { return owl_left_  ? owl_left_->model_name()  : std::string(); }
    std::string owl_right_model() const { return owl_right_ ? owl_right_->model_name() : std::string(); }

    // ── USB open / close (safe to call from any thread) ──────────────────────
    void open_usb1();
    void close_usb1();
    void open_usb2();
    void close_usb2();
    void open_usb3();
    void close_usb3();

    // ── USB device enumeration ────────────────────────────────────────────────
    // Returns all V4L2 USB capture devices currently visible on the system.
    // Each entry holds the device path and a human-readable card/driver label.
    struct UsbDeviceInfo { std::string path; std::string name; };
    std::vector<UsbDeviceInfo> list_usb_devices() const;

    // Close the slot, point it at a new device path, and reopen it.
    // Pass an empty string to disconnect the slot without reopening.
    // Safe to call from the menu/render thread.
    void reassign_usb1(const std::string& path);
    void reassign_usb2(const std::string& path);
    void reassign_usb3(const std::string& path);

    // ── USB camera scan ───────────────────────────────────────────────────────
    // Stops the capture thread, probes /dev/video0..N until a device opens,
    // then restarts the thread. Safe to call from the render thread.
    // Returns true if the slot is now open.
    bool scan_usb1();
    bool scan_usb2();
    bool scan_usb3();

    // ── CSI (OWLsight) re-init ────────────────────────────────────────────────
    // Tear down both CSI cameras + the libcamera manager, re-enumerate, and
    // re-init. Recovers a sensor that was missing / wedged at boot (the usual
    // "one eye dark until reboot" symptom) without a full reboot. MUST be called
    // from the render thread (DmaCamera::init needs the GL context current).
    // Returns true if at least one CSI camera came up.
    bool reinit_owls();

    // Bumped every time the OWLsight cameras are rebuilt (reinit_owls). The
    // render loop watches this to re-apply AppState-held settings the rebuilt
    // cameras don't keep on their own (focus mode/position, AWB-enable toggle).
    uint32_t reinit_generation() const { return reinit_gen_; }

    // ── Status ────────────────────────────────────────────────────────────────
    bool owl_left_ok()  const { return owl_left_  && owl_left_->is_ok();  }
    bool owl_right_ok() const { return owl_right_ && owl_right_->is_ok(); }
    bool usb1_ok()      const { return usb1_ok_; }
    bool usb2_ok()      const { return usb2_ok_; }
    bool usb3_ok()      const { return usb3_ok_; }

    // ── Auto-reconnect ────────────────────────────────────────────────────────
    // When enabled, the capture thread will attempt to reopen a disconnected
    // USB camera every ~5 s. Calling close_usbN() disables reconnect for that
    // camera so an explicit close stays closed.
    void set_usb1_reconnect(bool v) { usb1_reconnect_ = v; }
    void set_usb2_reconnect(bool v) { usb2_reconnect_ = v; }
    void set_usb3_reconnect(bool v) { usb3_reconnect_ = v; }
    bool usb1_reconnect_enabled() const { return usb1_reconnect_; }
    bool usb2_reconnect_enabled() const { return usb2_reconnect_; }
    bool usb3_reconnect_enabled() const { return usb3_reconnect_; }

    // ── QR / barcode scanning ─────────────────────────────────────────────────
    // set_qr_scanner: register the shared scanner instance (call before init).
    // enable_qr_usb:  hot-toggle USB scanning from any thread (atomic).
    void set_qr_scanner(QrScanner* s)  { qr_scanner_  = s; }
    void enable_qr_usb(bool v)         { qr_scan_usb_ = v; }

    // ── USB brightness (software multiplier, safe to call from any thread) ────
    void  set_usb1_brightness(float v) { usb1_brightness_ = v; }
    void  set_usb2_brightness(float v) { usb2_brightness_ = v; }
    void  set_usb3_brightness(float v) { usb3_brightness_ = v; }
    float usb1_brightness()      const { return usb1_brightness_.load(); }
    float usb2_brightness()      const { return usb2_brightness_.load(); }
    float usb3_brightness()      const { return usb3_brightness_.load(); }

    // ── USB V4L2 control (applies ioctl live; camera may briefly drop frames) ─
    void set_usb1_ctrl(uint32_t id, int32_t value);
    void set_usb2_ctrl(uint32_t id, int32_t value);
    void set_usb3_ctrl(uint32_t id, int32_t value);

    // ── USB config access (for menu read-back and config-save persistence) ────
    // Writes hold the per-camera cap mutex: the capture thread reads the cfg
    // (device path string) under that mutex for its reconnect check, and a
    // concurrent std::string assign vs. read is UB.
    void update_usb1_cfg(const UsbCamConfig& c) {
        std::lock_guard<std::mutex> lk(usb1_cap_mtx_);
        usb1_cfg_ = c;
        usb1_flip_                   = c.flip;
        usb1_auto_brightness_        = c.auto_brightness;
        usb1_auto_brightness_target_ = c.auto_brightness_target;
    }
    void update_usb2_cfg(const UsbCamConfig& c) {
        std::lock_guard<std::mutex> lk(usb2_cap_mtx_);
        usb2_cfg_ = c;
        usb2_flip_                   = c.flip;
        usb2_auto_brightness_        = c.auto_brightness;
        usb2_auto_brightness_target_ = c.auto_brightness_target;
    }
    void update_usb3_cfg(const UsbCamConfig& c) {
        std::lock_guard<std::mutex> lk(usb3_cap_mtx_);
        usb3_cfg_ = c;
        usb3_flip_                   = c.flip;
        usb3_auto_brightness_        = c.auto_brightness;
        usb3_auto_brightness_target_ = c.auto_brightness_target;
    }
    const UsbCamConfig& usb1_cfg() const { return usb1_cfg_; }
    const UsbCamConfig& usb2_cfg() const { return usb2_cfg_; }
    const UsbCamConfig& usb3_cfg() const { return usb3_cfg_; }

private:
    // One capture loop per USB camera (cam = 0/1/2 → usb1/2/3). Running each
    // camera on its own thread means a slow/blocking cap.read() on one device
    // can't add latency to the others (they used to share a single serial
    // loop). Started/stopped together via the helpers below.
    void usb_capture_thread(int cam);
    void start_usb_threads();   // (re)spawn any camera thread that isn't running
    void stop_usb_threads();    // clear running_ and join all three
    // Render-thread USB upload. upload_usb_slot() swaps the freshest pixels out
    // from under the slot lock (so the GL work never blocks the capture thread),
    // then upload_texture() pushes them to the GL texture — via an orphaned PBO
    // (async DMA, no pipeline stall) on ES3, or a synchronous client-pointer
    // upload on ES2. Both run on the render thread only.
    bool upload_usb_slot(TexSlot& s, GLuint& out);
    void upload_texture (TexSlot& s, int w, int h);
    // Stop the capture thread, probe video devices, restart thread.
    bool scan_usb(cv::VideoCapture& cap, std::atomic<bool>& ok,
                  UsbCamConfig& cfg,
                  const std::vector<std::string>& skip_paths = {});

    // RGBA fullscreen quad (for USB-as-eye-source path)
    GLuint rgba_prog_     = 0;
    GLuint rgba_quad_vbo_ = 0;
    GLint  rgba_tex_loc_  = -1;

    // Shared libcamera camera manager (one per process)
    std::unique_ptr<libcamera::CameraManager> lcam_mgr_;

    // OWLsight cameras (zero-copy DMA)
    std::unique_ptr<DmaCamera> owl_left_;
    std::unique_ptr<DmaCamera> owl_right_;
    // Stored so reinit_owls() can re-run the enumeration + init.
    CamConfig    owl_left_cfg_, owl_right_cfg_;
    uint32_t     reinit_gen_ = 0;   // ++ on each reinit_owls(); see reinit_generation()
    std::string  nv12_vs_, nv12_fs_;
    void init_owls();   // (re)resolve + (re)create the two DmaCameras from the stored cfgs

    // USB cameras (OpenCV capture)
    UsbCamConfig     usb1_cfg_, usb2_cfg_, usb3_cfg_;
    cv::VideoCapture usb_cap1_, usb_cap2_, usb_cap3_;
    std::mutex       usb1_cap_mtx_, usb2_cap_mtx_, usb3_cap_mtx_;
    std::atomic<bool> usb1_ok_ { false };
    std::atomic<bool> usb2_ok_ { false };
    std::atomic<bool> usb3_ok_ { false };
    std::atomic<bool> usb1_reconnect_ { false };
    std::atomic<bool> usb2_reconnect_ { false };
    std::atomic<bool> usb3_reconnect_ { false };
    // Written only by the capture thread; no extra sync needed.
    std::chrono::steady_clock::time_point usb1_last_retry_ {};
    std::chrono::steady_clock::time_point usb2_last_retry_ {};
    std::chrono::steady_clock::time_point usb3_last_retry_ {};

    // Per-USB-camera slot: capture thread writes buf/w/h, render thread uploads
    struct TexSlot {
        GLuint              tex   = 0;   // GL texture id (render thread only)
        int                 tex_w = 0;   // current allocation size
        int                 tex_h = 0;
        std::mutex          mtx;
        std::vector<uint8_t> buf;
        int                 w = 0, h = 0;
        bool                dirty = false;
        // Render-thread-only async-upload state (never touched under mtx):
        GLuint               pbo = 0;     // pixel-unpack buffer for async DMA
        std::vector<uint8_t> upbuf;       // pixels swapped out of buf for upload
    };
    TexSlot usb1_slot_, usb2_slot_, usb3_slot_;

    std::atomic<float> usb1_brightness_ { 1.0f };
    std::atomic<float> usb2_brightness_ { 1.0f };
    std::atomic<float> usb3_brightness_ { 1.0f };
    std::atomic<bool>  usb1_flip_       { false };
    std::atomic<bool>  usb2_flip_       { false };
    std::atomic<bool>  usb3_flip_       { false };
    std::atomic<bool>  usb1_auto_brightness_        { false };
    std::atomic<bool>  usb2_auto_brightness_        { false };
    std::atomic<bool>  usb3_auto_brightness_        { false };
    std::atomic<float> usb1_auto_brightness_target_ { 100.f };
    std::atomic<float> usb2_auto_brightness_target_ { 100.f };
    std::atomic<float> usb3_auto_brightness_target_ { 100.f };

    std::atomic<bool> running_     { false };
    std::thread       usb_threads_[3];   // one per usb camera (see usb_capture_thread)
    // Background reopen launched by reassign_usbN (open_v4l2 can block for
    // seconds, so it can't run on the menu thread). Owned — joined before a
    // new reopen is launched and in shutdown(), so the thread can never
    // outlive *this (a detached thread here was a use-after-free on exit).
    std::thread       usb_open_threads_[3];

    QrScanner*        qr_scanner_  { nullptr };
    std::atomic<bool> qr_scan_usb_ { false };
};

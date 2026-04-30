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

struct CamConfig {
    int  libcamera_id = 0;
    int  width        = 1280;
    int  height       = 800;
    int  fps          = 60;
};

struct UsbCamConfig {
    std::string device;
    int   width      = 1280;
    int   height     = 720;
    int   fps        = 30;
    float brightness = 1.0f;  // software multiplier: 1.0=normal, 2.0=double, 0.5=half
};

// CameraManager owns:
//   • Two DmaCamera instances (OWLsight left + right) — zero-copy NV12 path
//   • Two OpenCV VideoCapture instances (USB cams) — CPU→GPU upload path
//
// OWLsight: call draw_owl_left/right() inside an eye FBO (fills current viewport).
// USB cams: call get_usb1/usb2(GLuint&) to fetch the latest RGBA texture id.
//
// init() MUST be called from the render thread, after the GL context is current.

class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    bool init(const CamConfig& left, const CamConfig& right,
              const UsbCamConfig& usb1, const UsbCamConfig& usb2,
              const char* nv12_vs_path, const char* nv12_fs_path);
    void shutdown();

    // ── OWLsight (zero-copy DMA) ──────────────────────────────────────────────
    // Draw directly into the current render target (fills viewport).
    // Returns false if no frame is ready yet.
    bool draw_owl_left();
    bool draw_owl_right();

    // ── USB cameras (CPU→GPU upload) ──────────────────────────────────────────
    // Upload latest frame; out receives the GL texture id (0 if unavailable).
    // Returns true if a new frame was uploaded this call.
    bool get_usb1(GLuint& out);
    bool get_usb2(GLuint& out);

    // ── Resolution hot-swap ───────────────────────────────────────────────────
    // Reconfigures both OWLsight cameras to the given resolution and fps.
    // Returns true only if both cameras succeeded (or only one is present).
    // Must be called from the render thread.
    bool set_resolution(int width, int height, int fps);

    int current_width()  const { return owl_left_  ? owl_left_->width()  :
                                        owl_right_ ? owl_right_->width()  : 0; }
    int current_height() const { return owl_left_  ? owl_left_->height() :
                                        owl_right_ ? owl_right_->height() : 0; }

    // ── Direct camera access (focus/exposure control) ─────────────────────────
    DmaCamera* owl_left()  { return owl_left_.get();  }
    DmaCamera* owl_right() { return owl_right_.get(); }

    // ── USB open / close (safe to call from any thread) ──────────────────────
    void open_usb1();
    void close_usb1();
    void open_usb2();
    void close_usb2();

    // ── USB camera scan ───────────────────────────────────────────────────────
    // Stops the capture thread, probes /dev/video0..N until a device opens,
    // then restarts the thread. Safe to call from the render thread.
    // Returns true if the slot is now open.
    bool scan_usb1();
    bool scan_usb2();

    // ── Status ────────────────────────────────────────────────────────────────
    bool owl_left_ok()  const { return owl_left_  && owl_left_->is_ok();  }
    bool owl_right_ok() const { return owl_right_ && owl_right_->is_ok(); }
    bool usb1_ok()      const { return usb1_ok_; }
    bool usb2_ok()      const { return usb2_ok_; }

    // ── Auto-reconnect ────────────────────────────────────────────────────────
    // When enabled, the capture thread will attempt to reopen a disconnected
    // USB camera every ~5 s. Calling close_usbN() disables reconnect for that
    // camera so an explicit close stays closed.
    void set_usb1_reconnect(bool v) { usb1_reconnect_ = v; }
    void set_usb2_reconnect(bool v) { usb2_reconnect_ = v; }
    bool usb1_reconnect_enabled() const { return usb1_reconnect_; }
    bool usb2_reconnect_enabled() const { return usb2_reconnect_; }

    // ── USB brightness (software multiplier, safe to call from any thread) ────
    void  set_usb1_brightness(float v) { usb1_brightness_ = v; }
    void  set_usb2_brightness(float v) { usb2_brightness_ = v; }
    float usb1_brightness()      const { return usb1_brightness_.load(); }
    float usb2_brightness()      const { return usb2_brightness_.load(); }

private:
    void usb_capture_thread();
    // Upload pixel data to a GL texture; creates/reallocates as needed.
    void upload_texture(GLuint& tex, int w, int h, const unsigned char* rgba);
    // Stop the capture thread, probe video devices, restart thread.
    bool scan_usb(cv::VideoCapture& cap, std::atomic<bool>& ok,
                  const UsbCamConfig& cfg);

    // Shared libcamera camera manager (one per process)
    std::unique_ptr<libcamera::CameraManager> lcam_mgr_;

    // OWLsight cameras (zero-copy DMA)
    std::unique_ptr<DmaCamera> owl_left_;
    std::unique_ptr<DmaCamera> owl_right_;

    // USB cameras (OpenCV capture)
    UsbCamConfig     usb1_cfg_, usb2_cfg_;
    cv::VideoCapture usb_cap1_, usb_cap2_;
    std::mutex       usb1_cap_mtx_, usb2_cap_mtx_;
    std::atomic<bool> usb1_ok_ { false };
    std::atomic<bool> usb2_ok_ { false };
    std::atomic<bool> usb1_reconnect_ { false };
    std::atomic<bool> usb2_reconnect_ { false };
    // Written only by the capture thread; no extra sync needed.
    std::chrono::steady_clock::time_point usb1_last_retry_ {};
    std::chrono::steady_clock::time_point usb2_last_retry_ {};

    // Per-USB-camera slot: capture thread writes buf/w/h, render thread uploads
    struct TexSlot {
        GLuint              tex   = 0;   // GL texture id (render thread only)
        int                 tex_w = 0;   // current allocation size
        int                 tex_h = 0;
        std::mutex          mtx;
        std::vector<uint8_t> buf;
        int                 w = 0, h = 0;
        bool                dirty = false;
    };
    TexSlot usb1_slot_, usb2_slot_;

    std::atomic<float> usb1_brightness_ { 1.0f };
    std::atomic<float> usb2_brightness_ { 1.0f };

    std::atomic<bool> running_ { false };
    std::thread       usb_thread_;
};

#include "camera_manager.h"

#include <libcamera/libcamera.h>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

// NOTE: do NOT add 'using namespace libcamera' here — libcamera::CameraManager
// would collide with our own CameraManager class.

CameraManager::CameraManager()  = default;
CameraManager::~CameraManager() { shutdown(); }

// Returns true if the device is a V4L2 video-capture node driven by uvcvideo
// (i.e. a real USB camera, not a libcamera ISP pipeline node).
static bool is_usb_capture_device(const std::string& path, std::string* info_out = nullptr) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct v4l2_capability cap {};
    bool ok = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        if (info_out) {
            *info_out = std::string(reinterpret_cast<char*>(cap.driver))
                      + ": " + reinterpret_cast<char*>(cap.card);
        }
        // uvcvideo = USB Video Class — the standard Linux USB camera driver
        ok = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
             (std::string(reinterpret_cast<char*>(cap.driver)) == "uvcvideo");
    }
    close(fd);
    return ok;
}

// Shared helper: verify it's a USB camera, open via OpenCV, negotiate MJPEG.
static bool open_v4l2(cv::VideoCapture& cap, const UsbCamConfig& cfg,
                      const char* name) {
    std::string info;
    if (!is_usb_capture_device(cfg.device, &info)) {
        std::cerr << "[cam] " << name << ": " << cfg.device
                  << " is not a USB camera (driver=" << info
                  << ") — see '[cam] USB cameras:' list above for correct paths\n";
        return false;
    }
    cap.open(cfg.device, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::cerr << "[cam] " << name << ": open failed (" << cfg.device
                  << ") — check permissions (sudo usermod -aG video $USER)\n";
        return false;
    }
    // Prefer MJPEG: wider camera support and lower USB bandwidth than raw YUV.
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  cfg.width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, cfg.height);
    cap.set(cv::CAP_PROP_FPS,          cfg.fps);
    int aw = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int ah = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    int af = static_cast<int>(cap.get(cv::CAP_PROP_FPS));
    std::cerr << "[cam] " << name << ": opened " << cfg.device
              << " — negotiated " << aw << "x" << ah << " @" << af << "fps\n";
    return true;
}

bool CameraManager::init(const CamConfig& left, const CamConfig& right,
                         const UsbCamConfig& usb1, const UsbCamConfig& usb2,
                         const char* nv12_vs, const char* nv12_fs) {
    // ── libcamera manager (shared between both DmaCamera instances) ───────────
    lcam_mgr_ = std::make_unique<libcamera::CameraManager>();
    if (lcam_mgr_->start()) {
        std::cerr << "[cam] libcamera manager start failed\n";
        lcam_mgr_.reset();
        // Non-fatal — USB cameras can still work
    }

    // ── OWLsight cameras (zero-copy DMA) ─────────────────────────────────────
    if (lcam_mgr_) {
        DmaCamera::Config lc { left.libcamera_id, left.width, left.height, left.fps };
        owl_left_ = std::make_unique<DmaCamera>();
        if (!owl_left_->init(lcam_mgr_.get(), lc, nv12_vs, nv12_fs)) {
            std::cerr << "[cam] OWLsight left init failed\n";
            owl_left_.reset();
        }

        DmaCamera::Config rc { right.libcamera_id, right.width, right.height, right.fps };
        owl_right_ = std::make_unique<DmaCamera>();
        if (!owl_right_->init(lcam_mgr_.get(), rc, nv12_vs, nv12_fs)) {
            std::cerr << "[cam] OWLsight right init failed\n";
            owl_right_.reset();
        }
    }

    // ── USB cameras (OpenCV) ──────────────────────────────────────────────────
    usb1_cfg_ = usb1;
    usb2_cfg_ = usb2;

    // Scan /dev/video0-19 and list only real USB cameras (uvcvideo driver)
    std::cerr << "[cam] USB cameras found:\n";
    bool found_any = false;
    for (int i = 0; i <= 19; i++) {
        std::string d = "/dev/video" + std::to_string(i);
        std::string info;
        if (access(d.c_str(), F_OK) == 0 && is_usb_capture_device(d, &info)) {
            std::cerr << "  " << d << "  (" << info << ")\n";
            found_any = true;
        }
    }
    if (!found_any) std::cerr << "  (none — are USB cameras plugged in?)\n";

    usb1_ok_ = !usb1.device.empty() && open_v4l2(usb_cap1_, usb1, "usb1");
    usb2_ok_ = !usb2.device.empty() && open_v4l2(usb_cap2_, usb2, "usb2");

    // Start thread whenever USB devices are configured so open/close can work
    // later even if the cameras were not available at startup.
    if (!usb1.device.empty() || !usb2.device.empty()) {
        running_ = true;
        usb_thread_ = std::thread(&CameraManager::usb_capture_thread, this);
    }

    return owl_left_ok() || owl_right_ok() || usb1_ok_ || usb2_ok_;
}

void CameraManager::shutdown() {
    running_ = false;
    if (usb_thread_.joinable()) usb_thread_.join();

    owl_left_.reset();
    owl_right_.reset();

    usb_cap1_.release();
    usb_cap2_.release();

    // Clean up GL textures allocated for USB cams (must be called from render thread)
    if (usb1_slot_.tex) { glDeleteTextures(1, &usb1_slot_.tex); usb1_slot_.tex = 0; }
    if (usb2_slot_.tex) { glDeleteTextures(1, &usb2_slot_.tex); usb2_slot_.tex = 0; }

    if (lcam_mgr_) { lcam_mgr_->stop(); lcam_mgr_.reset(); }
}

// ── OWLsight draw (zero-copy, render thread) ──────────────────────────────────

bool CameraManager::draw_owl_left()  { return owl_left_  && owl_left_->draw();  }
bool CameraManager::draw_owl_right() { return owl_right_ && owl_right_->draw(); }

// ── Resolution hot-swap ────────────────────────────────────────────────────────

bool CameraManager::set_resolution(int width, int height, int fps) {
    bool ok = true;
    if (owl_left_)  ok &= owl_left_->reconfigure(width, height, fps);
    if (owl_right_) ok &= owl_right_->reconfigure(width, height, fps);
    if (ok)
        std::cout << "[cam] resolution set to " << width << "×" << height
                  << " @" << fps << "fps\n";
    else
        std::cerr << "[cam] one or both cameras failed resolution change\n";
    return ok;
}

// ── USB camera capture thread ─────────────────────────────────────────────────

void CameraManager::usb_capture_thread() {
    cv::Mat frame, rgba;
    int frames1 = 0, empty1 = 0;
    int frames2 = 0, empty2 = 0;

    auto capture = [&](cv::VideoCapture& cap, std::mutex& cap_mtx,
                       TexSlot& slot, std::atomic<bool>& ok_flag,
                       int& good, int& bad, const char* name) -> bool {
        bool got_frame = false;
        {
            std::lock_guard<std::mutex> lk(cap_mtx);
            if (!cap.isOpened()) { ok_flag = false; return false; }
            cap.read(frame);
            if (!frame.empty()) {
                cv::cvtColor(frame, rgba, cv::COLOR_BGR2RGBA);
                got_frame = true;
                ++good;
                if (good == 1)
                    std::cerr << "[cam] " << name << ": first frame received ("
                              << frame.cols << "x" << frame.rows << ")\n";
            } else {
                if (++bad % 30 == 1)
                    std::cerr << "[cam] " << name << ": " << bad
                              << " empty reads (good=" << good << ")\n";
            }
        }
        if (got_frame) {
            std::lock_guard<std::mutex> lk(slot.mtx);
            slot.w = rgba.cols;
            slot.h = rgba.rows;
            slot.buf.resize(static_cast<size_t>(rgba.total()) * 4);
            std::memcpy(slot.buf.data(), rgba.data, slot.buf.size());
            slot.dirty = true;
        }
        return true;
    };

    while (running_) {
        bool any = capture(usb_cap1_, usb1_cap_mtx_, usb1_slot_, usb1_ok_, frames1, empty1, "usb1");
        any      |= capture(usb_cap2_, usb2_cap_mtx_, usb2_slot_, usb2_ok_, frames2, empty2, "usb2");
        if (!any)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ── USB open / close (callable from any thread) ───────────────────────────────

void CameraManager::open_usb1() {
    std::lock_guard<std::mutex> lk(usb1_cap_mtx_);
    if (usb1_cfg_.device.empty()) {
        std::cerr << "[cam] usb1: no device configured\n"; return;
    }
    if (usb_cap1_.isOpened()) {
        std::cerr << "[cam] usb1: already open\n"; return;
    }
    usb1_ok_ = open_v4l2(usb_cap1_, usb1_cfg_, "usb1");
}

void CameraManager::close_usb1() {
    std::lock_guard<std::mutex> lk(usb1_cap_mtx_);
    usb_cap1_.release();
    usb1_ok_ = false;
    std::cerr << "[cam] usb1: closed\n";
}

void CameraManager::open_usb2() {
    std::lock_guard<std::mutex> lk(usb2_cap_mtx_);
    if (usb2_cfg_.device.empty()) {
        std::cerr << "[cam] usb2: no device configured\n"; return;
    }
    if (usb_cap2_.isOpened()) {
        std::cerr << "[cam] usb2: already open\n"; return;
    }
    usb2_ok_ = open_v4l2(usb_cap2_, usb2_cfg_, "usb2");
}

void CameraManager::close_usb2() {
    std::lock_guard<std::mutex> lk(usb2_cap_mtx_);
    usb_cap2_.release();
    usb2_ok_ = false;
    std::cerr << "[cam] usb2: closed\n";
}

// ── USB texture upload (render thread) ────────────────────────────────────────
// Creates or reallocates the GL texture if dimensions changed, then uploads pixels.

void CameraManager::upload_texture(GLuint& tex, int w, int h,
                                   const unsigned char* rgba) {
    if (tex == 0) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    // glTexImage2D on dimension change, glTexSubImage2D otherwise (faster)
    // We track tex_w/tex_h in the slot; compare via the caller's tracking.
    // Since upload_texture doesn't have slot access, we always use TexImage2D
    // here and let the caller track when realloc is needed via tex_w/tex_h.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    glBindTexture(GL_TEXTURE_2D, 0);
}

bool CameraManager::get_usb1(GLuint& out) {
    std::lock_guard<std::mutex> lk(usb1_slot_.mtx);
    if (!usb1_slot_.dirty || usb1_slot_.buf.empty()) { out = usb1_slot_.tex; return false; }
    upload_texture(usb1_slot_.tex, usb1_slot_.w, usb1_slot_.h, usb1_slot_.buf.data());
    usb1_slot_.dirty = false;
    out = usb1_slot_.tex;
    return true;
}

bool CameraManager::get_usb2(GLuint& out) {
    std::lock_guard<std::mutex> lk(usb2_slot_.mtx);
    if (!usb2_slot_.dirty || usb2_slot_.buf.empty()) { out = usb2_slot_.tex; return false; }
    upload_texture(usb2_slot_.tex, usb2_slot_.w, usb2_slot_.h, usb2_slot_.buf.data());
    usb2_slot_.dirty = false;
    out = usb2_slot_.tex;
    return true;
}

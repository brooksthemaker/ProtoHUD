#include "camera_manager.h"
#include "qr_scanner.h"
#include "../gl_utils.h"

#include <libcamera/libcamera.h>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

// NOTE: do NOT add 'using namespace libcamera' here — libcamera::CameraManager
// would collide with our own CameraManager class.

CameraManager::CameraManager()  = default;
CameraManager::~CameraManager() { shutdown(); }

// Returns true if the device is a usable video-capture node.
// Rejects known libcamera ISP/pipeline drivers that have VIDEO_CAPTURE
// but produce no frames when opened directly.
static bool is_usb_capture_device(const std::string& path, std::string* info_out = nullptr) {
    // Use O_RDWR so only devices that OpenCV can also open are reported.
    // O_RDONLY can succeed on ISP metadata nodes without video-group membership,
    // causing a misleading permission error when OpenCV then tries O_RDWR.
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    struct v4l2_capability cap {};
    bool ok = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        std::string driver(reinterpret_cast<char*>(cap.driver));
        std::string card  (reinterpret_cast<char*>(cap.card));
        if (info_out) *info_out = driver + ": " + card;
        // Block known RPi ISP pipeline drivers — they open but yield no frames
        bool is_isp = (driver == "rp1-cfe"      ||
                       driver == "bcm2835-isp"   ||
                       driver == "pispbe"         ||
                       driver.find("pisp") != std::string::npos);
        ok = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) && !is_isp;
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
                  << " is an ISP/pipeline node, not a camera (driver=" << info
                  << ") — use a path from the '[cam] USB cameras found:' list\n";
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
    cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 0.75);  // request auto-exposure (driver best-effort)
    // Minimise the driver-side V4L2 queue so cap.read() returns the freshest
    // frame instead of the oldest one in a deep ring. Without this OpenCV's
    // V4L2 backend defaults to a 4-deep queue, adding up to ~3 frame
    // intervals (~100 ms @30fps) of motion-to-photon latency. Best-effort:
    // not every backend/driver honours it, so we don't check the return.
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // Apply UVC controls not exposed by OpenCV via direct V4L2 ioctl.
    {
        int fd = open(cfg.device.c_str(), O_RDWR);
        if (fd >= 0) {
            auto set_ctrl = [&](uint32_t id, int32_t val) {
                v4l2_control c{}; c.id = id; c.value = val;
                ioctl(fd, VIDIOC_S_CTRL, &c);
            };
            // Disable dynamic framerate throttle (keeps fps at configured rate).
            set_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, cfg.dynamic_framerate ? 1 : 0);
            set_ctrl(V4L2_CID_EXPOSURE_AUTO, cfg.auto_exposure ? 3 : 1);  // 3=AP, 1=Manual
            if (!cfg.auto_exposure)
                set_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, cfg.exposure_time);
            set_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, cfg.auto_wb ? 1 : 0);
            if (!cfg.auto_wb)
                set_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, cfg.wb_temp);
            close(fd);
        }
    }

    int aw = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int ah = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    int af = static_cast<int>(cap.get(cv::CAP_PROP_FPS));
    std::cerr << "[cam] " << name << ": opened " << cfg.device
              << " — negotiated " << aw << "x" << ah << " @" << af << "fps\n";
    return true;
}

bool CameraManager::init(const CamConfig& left, const CamConfig& right,
                         const UsbCamConfig& usb1, const UsbCamConfig& usb2,
                         const UsbCamConfig& usb3,
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
        // Resolve both eyes to EXACT, distinct camera ids from a single enumeration.
        // (Re-querying cameras() per DmaCamera could return a different order once
        // the first camera is acquired/started, making the second eye resolve to
        // the already-running camera — "Camera in Running state".)
        auto cams = lcam_mgr_->cameras();
        auto resolve = [&](const CamConfig& cc) -> std::string {
            if (!cc.model_name.empty()) {
                int m = 0;
                for (auto& c : cams) {
                    try {
                        auto md = c->properties().get(libcamera::properties::Model);
                        if (md && *md == cc.model_name) {
                            if (m == cc.libcamera_id) return c->id();
                            ++m;
                        }
                    } catch (...) {}
                }
            }
            if (cc.libcamera_id >= 0 && cc.libcamera_id < static_cast<int>(cams.size()))
                return cams[cc.libcamera_id]->id();
            return {};
        };
        std::string left_id  = resolve(left);
        std::string right_id = resolve(right);
        // Never let both eyes land on the same physical camera.
        if (!left_id.empty() && left_id == right_id) {
            right_id.clear();
            for (auto& c : cams) if (c->id() != left_id) { right_id = c->id(); break; }
        }
        std::cout << "[cam] OWL ids: left='" << left_id << "' right='" << right_id << "'\n";

        // Only init a camera we actually resolved to a real id. With an empty id
        // DmaCamera would fall back to the first/only camera — which is the one the
        // other eye already holds — producing "Camera in Running state" on acquire.
        // When only one OWLsight enumerates (the other is unplugged or wedged),
        // skip the missing eye cleanly; the feed just shows mono.
        if (left_id.empty()) {
            std::cerr << "[cam] OWLsight left: no camera resolved — skipping "
                         "(only " << cams.size() << " libcamera device(s) present)\n";
        } else {
            DmaCamera::Config lc { left.libcamera_id, left.model_name, left_id, left.width, left.height, left.fps };
            owl_left_ = std::make_unique<DmaCamera>();
            if (!owl_left_->init(lcam_mgr_.get(), lc, nv12_vs, nv12_fs)) {
                std::cerr << "[cam] OWLsight left init failed\n";
                owl_left_.reset();
            }
        }

        if (right_id.empty()) {
            std::cerr << "[cam] OWLsight right: no second camera resolved — skipping "
                         "(running mono). If you expect two CSI cameras, a reboot "
                         "usually clears a wedged sensor — check 'rpicam-hello --list-cameras'.\n";
        } else {
            DmaCamera::Config rc { right.libcamera_id, right.model_name, right_id, right.width, right.height, right.fps };
            owl_right_ = std::make_unique<DmaCamera>();
            if (!owl_right_->init(lcam_mgr_.get(), rc, nv12_vs, nv12_fs)) {
                std::cerr << "[cam] OWLsight right init failed\n";
                owl_right_.reset();
            }
        }
    }

    // ── USB cameras (OpenCV) ──────────────────────────────────────────────────
    usb1_cfg_ = usb1;
    usb2_cfg_ = usb2;
    usb3_cfg_ = usb3;
    usb1_brightness_ = usb1.brightness;
    usb2_brightness_ = usb2.brightness;
    usb3_brightness_ = usb3.brightness;
    usb1_flip_                   = usb1.flip;
    usb2_flip_                   = usb2.flip;
    usb3_flip_                   = usb3.flip;
    usb1_auto_brightness_        = usb1.auto_brightness;
    usb1_auto_brightness_target_ = usb1.auto_brightness_target;
    usb2_auto_brightness_        = usb2.auto_brightness;
    usb2_auto_brightness_target_ = usb2.auto_brightness_target;
    usb3_auto_brightness_        = usb3.auto_brightness;
    usb3_auto_brightness_target_ = usb3.auto_brightness_target;

    // Scan /dev/video0-63 and list non-ISP capture nodes
    std::cerr << "[cam] USB cameras found:\n";
    bool found_any = false;
    for (int i = 0; i <= 63; i++) {
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
    usb3_ok_ = !usb3.device.empty() && open_v4l2(usb_cap3_, usb3, "usb3");

    // Auto-scan: if a camera failed to open with the configured path, probe all
    // /dev/video* nodes (before the capture thread starts) to find a working one.
    auto startup_scan = [&](cv::VideoCapture& cap, std::atomic<bool>& ok_flag,
                             UsbCamConfig& cfg, const char* name,
                             std::initializer_list<std::string> skip_paths) {
        if (ok_flag) return;
        std::cerr << "[cam] " << name << ": auto-scanning for a camera...\n";
        for (int dev = 0; dev < 64; dev++) {
            std::string path = "/dev/video" + std::to_string(dev);
            bool skip = false;
            for (const auto& s : skip_paths) if (!s.empty() && path == s) { skip = true; break; }
            if (skip) continue;
            if (!is_usb_capture_device(path)) continue;
            UsbCamConfig try_cfg = cfg;
            try_cfg.device = path;
            if (open_v4l2(cap, try_cfg, name)) {
                cv::Mat f;
                if (cap.read(f) && !f.empty()) {
                    cfg.device = path;
                    ok_flag = true;
                    std::cerr << "[cam] " << name << ": found at " << path << "\n";
                    return;
                }
                cap.release();
            }
        }
        std::cerr << "[cam] " << name << ": auto-scan found no working camera\n";
    };
    startup_scan(usb_cap1_, usb1_ok_, usb1_cfg_, "usb1", {});
    startup_scan(usb_cap2_, usb2_ok_, usb2_cfg_, "usb2", {usb1_cfg_.device});
    startup_scan(usb_cap3_, usb3_ok_, usb3_cfg_, "usb3", {usb1_cfg_.device, usb2_cfg_.device});

    // Start thread whenever USB devices are configured so open/close can work
    // later even if the cameras were not available at startup.
    if (!usb1.device.empty() || !usb2.device.empty() || !usb3.device.empty()) {
        start_usb_threads();
    }

    // ── RGBA fullscreen shader (USB-as-eye-source) ────────────────────────────
    static const char* kRgbaVS = R"glsl(
        attribute vec2 a_pos;
        attribute vec2 a_uv;
        varying vec2 v_uv;
        void main() {
            gl_Position = vec4(a_pos, 0.0, 1.0);
            v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
        }
    )glsl";
    static const char* kRgbaFS = R"glsl(
        precision mediump float;
        uniform sampler2D u_tex;
        varying vec2 v_uv;
        void main() {
            gl_FragColor = texture2D(u_tex, v_uv);
        }
    )glsl";
    rgba_prog_ = gl::build_program_from_strings(kRgbaVS, kRgbaFS);
    if (!rgba_prog_) std::cerr << "[cam] RGBA fullscreen shader failed to compile\n";
    rgba_tex_loc_  = glGetUniformLocation(rgba_prog_, "u_tex");
    rgba_quad_vbo_ = gl::make_quad_vbo();

    return owl_left_ok() || owl_right_ok() || usb1_ok_ || usb2_ok_ || usb3_ok_;
}

void CameraManager::shutdown() {
    stop_usb_threads();

    owl_left_.reset();
    owl_right_.reset();

    usb_cap1_.release();
    usb_cap2_.release();
    usb_cap3_.release();

    // Clean up GL textures allocated for USB cams (must be called from render thread)
    if (usb1_slot_.tex) { glDeleteTextures(1, &usb1_slot_.tex); usb1_slot_.tex = 0; }
    if (usb2_slot_.tex) { glDeleteTextures(1, &usb2_slot_.tex); usb2_slot_.tex = 0; }
    if (usb3_slot_.tex) { glDeleteTextures(1, &usb3_slot_.tex); usb3_slot_.tex = 0; }

    if (rgba_prog_)     { glDeleteProgram(rgba_prog_);         rgba_prog_     = 0; }
    if (rgba_quad_vbo_) { glDeleteBuffers(1, &rgba_quad_vbo_); rgba_quad_vbo_ = 0; }

    if (lcam_mgr_) { lcam_mgr_->stop(); lcam_mgr_.reset(); }
}

// ── CSI camera draw (zero-copy, render thread) ────────────────────────────────

bool CameraManager::draw_owl_left( float zoom, float cx, float cy) { return owl_left_  && owl_left_->draw(zoom, cx, cy);  }
bool CameraManager::draw_owl_right(float zoom, float cx, float cy) { return owl_right_ && owl_right_->draw(zoom, cx, cy); }

bool CameraManager::draw_tex_fullscreen(GLuint tex) const {
    if (!tex || !rgba_prog_ || !rgba_quad_vbo_) return false;
    glUseProgram(rgba_prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (rgba_tex_loc_ >= 0) glUniform1i(rgba_tex_loc_, 0);
    gl::bind_quad(rgba_quad_vbo_);
    gl::draw_quad();
    gl::unbind_quad();
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return true;
}

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

void CameraManager::start_usb_threads() {
    running_ = true;
    for (int cam = 0; cam < 3; ++cam)
        if (!usb_threads_[cam].joinable())
            usb_threads_[cam] = std::thread(&CameraManager::usb_capture_thread, this, cam);
}

void CameraManager::stop_usb_threads() {
    running_ = false;
    for (auto& t : usb_threads_)
        if (t.joinable()) t.join();
}

void CameraManager::usb_capture_thread(int cam) {
    // Per-camera member dispatch — each thread owns exactly one camera's
    // capture device, slot, control atomics, and reconnect bookkeeping.
    cv::VideoCapture*   cap      = nullptr;
    std::mutex*         cap_mtx  = nullptr;
    TexSlot*            slot     = nullptr;
    std::atomic<bool>*  ok_flag  = nullptr;
    std::atomic<float>* brightness_ref = nullptr;
    std::atomic<bool>*  flip_ref = nullptr;
    std::atomic<bool>*  auto_brightness_ref = nullptr;
    std::atomic<float>* auto_brightness_target_ref = nullptr;
    std::atomic<bool>*  reconnect_ref = nullptr;
    UsbCamConfig*       cfg_ref  = nullptr;
    std::chrono::steady_clock::time_point* last_retry = nullptr;
    std::function<void()> reopen;
    const char* name = "usb?";
    switch (cam) {
        case 0: cap=&usb_cap1_; cap_mtx=&usb1_cap_mtx_; slot=&usb1_slot_; ok_flag=&usb1_ok_;
                brightness_ref=&usb1_brightness_; flip_ref=&usb1_flip_;
                auto_brightness_ref=&usb1_auto_brightness_;
                auto_brightness_target_ref=&usb1_auto_brightness_target_;
                reconnect_ref=&usb1_reconnect_; cfg_ref=&usb1_cfg_;
                last_retry=&usb1_last_retry_; reopen=[this]{ open_usb1(); }; name="usb1"; break;
        case 1: cap=&usb_cap2_; cap_mtx=&usb2_cap_mtx_; slot=&usb2_slot_; ok_flag=&usb2_ok_;
                brightness_ref=&usb2_brightness_; flip_ref=&usb2_flip_;
                auto_brightness_ref=&usb2_auto_brightness_;
                auto_brightness_target_ref=&usb2_auto_brightness_target_;
                reconnect_ref=&usb2_reconnect_; cfg_ref=&usb2_cfg_;
                last_retry=&usb2_last_retry_; reopen=[this]{ open_usb2(); }; name="usb2"; break;
        case 2: cap=&usb_cap3_; cap_mtx=&usb3_cap_mtx_; slot=&usb3_slot_; ok_flag=&usb3_ok_;
                brightness_ref=&usb3_brightness_; flip_ref=&usb3_flip_;
                auto_brightness_ref=&usb3_auto_brightness_;
                auto_brightness_target_ref=&usb3_auto_brightness_target_;
                reconnect_ref=&usb3_reconnect_; cfg_ref=&usb3_cfg_;
                last_retry=&usb3_last_retry_; reopen=[this]{ open_usb3(); }; name="usb3"; break;
        default: return;
    }

    cv::Mat frame, rgba;            // thread-local — no sharing between cameras now
    int good = 0, bad = 0, consec = 0, luma_tick = 0;

    // Consecutive empty reads before declaring a camera disconnected.
    // At ~30fps with near-instant V4L2 failures this triggers in ~1 s.
    constexpr int kDisconnectThreshold = 30;
    // How often to attempt a reconnect when enabled.
    constexpr auto kReconnectInterval = std::chrono::seconds(5);

    while (running_) {
        bool got_frame = false;
        {
            std::lock_guard<std::mutex> lk(*cap_mtx);
            if (!cap->isOpened()) {
                *ok_flag = false;
            } else {
                cap->read(frame);
                if (!frame.empty()) {
                    // Adaptive luma compensation: sample every 15 frames (~2/sec at 30fps)
                    if (auto_brightness_ref->load() && ++luma_tick >= 15) {
                        luma_tick = 0;
                        cv::Scalar m = cv::mean(frame);  // BGR: [0]=B [1]=G [2]=R
                        float luma = m[2] * 0.299f + m[1] * 0.587f + m[0] * 0.114f;
                        if (luma > 1.f) {
                            float target = auto_brightness_target_ref->load();
                            float ratio  = target / luma;
                            float cur    = brightness_ref->load();
                            float next   = cur + (cur * ratio - cur) * 0.15f;
                            next = std::clamp(next, 0.25f, 4.0f);
                            brightness_ref->store(next);
                        }
                    }
                    float b = brightness_ref->load();
                    if (b != 1.0f)
                        frame.convertTo(frame, -1, static_cast<double>(b), 0.0);
                    if (flip_ref->load())
                        cv::flip(frame, frame, -1);  // -1 = 180° rotation (both axes)
                    cv::cvtColor(frame, rgba, cv::COLOR_BGR2RGBA);
                    // QR scan: convert BGR frame to grayscale and submit to scanner.
                    // submit_gray() is rate-limited internally; safe to call every frame.
                    if (qr_scanner_ && qr_scan_usb_.load()) {
                        cv::Mat gray;
                        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                        qr_scanner_->submit_gray(
                            std::vector<uint8_t>(gray.data,
                                                 gray.data + gray.total()),
                            gray.cols, gray.rows);
                    }
                    got_frame = true;
                    consec = 0;
                    ++good;
                    if (good == 1)
                        std::cerr << "[cam] " << name << ": first frame received ("
                                  << frame.cols << "x" << frame.rows << ")\n";
                } else {
                    if (++bad % 30 == 1)
                        std::cerr << "[cam] " << name << ": " << bad
                                  << " empty reads (good=" << good << ")\n";
                    // Release on disconnect. A camera that has *never* delivered a frame
                    // (good == 0 — e.g. a wrong/ISP /dev/video node) is also released,
                    // after a longer grace period, so it can't spin-read forever and
                    // peg a CPU core / starve the render thread.
                    ++consec;
                    const int thresh = (good > 0) ? kDisconnectThreshold : 90;  // ~0.9s grace
                    if (consec >= thresh) {
                        std::cerr << "[cam] " << name
                                  << (good > 0 ? ": disconnected\n" : ": no frames — releasing\n");
                        *ok_flag = false;
                        cap->release();  // isOpened() now returns false
                        consec = 0;
                    }
                }
            }
        }
        if (got_frame) {
            std::lock_guard<std::mutex> lk(slot->mtx);
            slot->w = rgba.cols;
            slot->h = rgba.rows;
            slot->buf.resize(static_cast<size_t>(rgba.total()) * 4);
            std::memcpy(slot->buf.data(), rgba.data, slot->buf.size());
            slot->dirty = true;
        }

        // Auto-reconnect: periodically reopen this camera when enabled.
        auto now = std::chrono::steady_clock::now();
        if (reconnect_ref->load() && !ok_flag->load() && !cfg_ref->device.empty() &&
            now - *last_retry >= kReconnectInterval) {
            *last_retry = now;
            consec = 0; bad = 0;  // reset counters for the new attempt
            std::cerr << "[cam] " << name << ": auto-reconnect attempt\n";
            reopen();  // cap mutex already released above
        }

        // Only a real frame counts as "activity". An open-but-empty camera sleeps
        // instead of hot-spinning on a dead device.
        if (!got_frame)
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
    usb1_reconnect_ = false;   // explicit close — don't auto-reopen
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
    usb2_reconnect_ = false;   // explicit close — don't auto-reopen
    std::lock_guard<std::mutex> lk(usb2_cap_mtx_);
    usb_cap2_.release();
    usb2_ok_ = false;
    std::cerr << "[cam] usb2: closed\n";
}

void CameraManager::open_usb3() {
    std::lock_guard<std::mutex> lk(usb3_cap_mtx_);
    if (usb3_cfg_.device.empty()) {
        std::cerr << "[cam] usb3: no device configured\n"; return;
    }
    if (usb_cap3_.isOpened()) {
        std::cerr << "[cam] usb3: already open\n"; return;
    }
    usb3_ok_ = open_v4l2(usb_cap3_, usb3_cfg_, "usb3");
}

void CameraManager::close_usb3() {
    usb3_reconnect_ = false;   // explicit close — don't auto-reopen
    std::lock_guard<std::mutex> lk(usb3_cap_mtx_);
    usb_cap3_.release();
    usb3_ok_ = false;
    std::cerr << "[cam] usb3: closed\n";
}

// ── USB texture upload (render thread) ────────────────────────────────────────
// Creates or reallocates the GL texture if dimensions changed, then uploads pixels.

void CameraManager::upload_texture(GLuint& tex, int w, int h,
                                   const unsigned char* rgba,
                                   int& prev_w, int& prev_h) {
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

    if (w != prev_w || h != prev_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        prev_w = w; prev_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

// ── USB camera scan/reconnect ──────────────────────────────────────────────────
// Stops the capture thread so no concurrent cap.read() is in flight, then probes
// /dev/video0..9 until a device opens successfully. The thread is restarted if
// at least one USB slot is now open. Returns true if this slot is now open.

bool CameraManager::scan_usb(cv::VideoCapture& cap, std::atomic<bool>& ok,
                              UsbCamConfig& cfg,
                              const std::vector<std::string>& skip_paths)
{
    // Stop all capture threads so nothing else is calling cap.read() while we
    // probe /dev/video* (the scan opens devices that another camera thread
    // might otherwise grab mid-iteration).
    stop_usb_threads();

    cap.release();
    ok = false;

    for (int dev = 0; dev < 64; dev++) {
        std::string path = "/dev/video" + std::to_string(dev);
        bool skip = false;
        for (const auto& s : skip_paths) if (!s.empty() && path == s) { skip = true; break; }
        if (skip) continue;
        if (!is_usb_capture_device(path)) continue;
        UsbCamConfig test_cfg = cfg;
        test_cfg.device = path;
        if (open_v4l2(cap, test_cfg, "scan")) {
            cv::Mat test;
            if (cap.read(test) && !test.empty()) {
                std::cerr << "[cam] USB scan found device at " << path << "\n";
                cfg.device = path;
                ok = true;
                break;
            }
            cap.release();
        }
    }

    if (!ok) {
        std::cerr << "[cam] USB scan found no working device\n"
                  << "  If cameras are plugged in, check group membership:\n"
                  << "    sudo usermod -aG video $USER  (then log out and back in)\n";
    }

    // Restart the capture threads if any slot is now usable
    if (usb1_ok_ || usb2_ok_ || usb3_ok_) {
        start_usb_threads();
    }

    return ok.load();
}

bool CameraManager::scan_usb1() { return scan_usb(usb_cap1_, usb1_ok_, usb1_cfg_); }
bool CameraManager::scan_usb2() { return scan_usb(usb_cap2_, usb2_ok_, usb2_cfg_, {usb1_cfg_.device}); }
bool CameraManager::scan_usb3() { return scan_usb(usb_cap3_, usb3_ok_, usb3_cfg_, {usb1_cfg_.device, usb2_cfg_.device}); }

std::vector<CameraManager::UsbDeviceInfo> CameraManager::list_usb_devices() const {
    std::vector<UsbDeviceInfo> result;
    for (int i = 0; i < 64; ++i) {
        std::string path = "/dev/video" + std::to_string(i);
        std::string name;
        if (is_usb_capture_device(path, &name))
            result.push_back({std::move(path), std::move(name)});
    }
    return result;
}

// Close the slot, update its device path, and reopen it on a background thread.
// The capture lambda holds the per-camera mutex during cap.read(); close_usbN()
// acquires the same mutex, so the swap is safe without stopping the thread.
void CameraManager::reassign_usb1(const std::string& path) {
    UsbCamConfig c = usb1_cfg_; c.device = path; update_usb1_cfg(c);
    close_usb1();  // acquires usb1_cap_mtx_, releases cap, clears reconnect
    if (!path.empty()) {
        usb1_reconnect_ = true;
        std::thread([this]{ open_usb1(); }).detach();
    }
}
void CameraManager::reassign_usb2(const std::string& path) {
    UsbCamConfig c = usb2_cfg_; c.device = path; update_usb2_cfg(c);
    close_usb2();
    if (!path.empty()) {
        usb2_reconnect_ = true;
        std::thread([this]{ open_usb2(); }).detach();
    }
}
void CameraManager::reassign_usb3(const std::string& path) {
    UsbCamConfig c = usb3_cfg_; c.device = path; update_usb3_cfg(c);
    close_usb3();
    if (!path.empty()) {
        usb3_reconnect_ = true;
        std::thread([this]{ open_usb3(); }).detach();
    }
}

static void v4l2_set_ctrl(const std::string& dev, uint32_t id, int32_t val) {
    int fd = open(dev.c_str(), O_RDWR);
    if (fd < 0) return;
    v4l2_control c{}; c.id = id; c.value = val;
    ioctl(fd, VIDIOC_S_CTRL, &c);
    close(fd);
}
void CameraManager::set_usb1_ctrl(uint32_t id, int32_t val) { v4l2_set_ctrl(usb1_cfg_.device, id, val); }
void CameraManager::set_usb2_ctrl(uint32_t id, int32_t val) { v4l2_set_ctrl(usb2_cfg_.device, id, val); }
void CameraManager::set_usb3_ctrl(uint32_t id, int32_t val) { v4l2_set_ctrl(usb3_cfg_.device, id, val); }

bool CameraManager::get_usb1(GLuint& out) {
    std::lock_guard<std::mutex> lk(usb1_slot_.mtx);
    if (!usb1_slot_.dirty || usb1_slot_.buf.empty()) { out = usb1_slot_.tex; return false; }
    upload_texture(usb1_slot_.tex, usb1_slot_.w, usb1_slot_.h, usb1_slot_.buf.data(),
                   usb1_slot_.tex_w, usb1_slot_.tex_h);
    usb1_slot_.dirty = false;
    out = usb1_slot_.tex;
    return true;
}

bool CameraManager::get_usb2(GLuint& out) {
    std::lock_guard<std::mutex> lk(usb2_slot_.mtx);
    if (!usb2_slot_.dirty || usb2_slot_.buf.empty()) { out = usb2_slot_.tex; return false; }
    upload_texture(usb2_slot_.tex, usb2_slot_.w, usb2_slot_.h, usb2_slot_.buf.data(),
                   usb2_slot_.tex_w, usb2_slot_.tex_h);
    usb2_slot_.dirty = false;
    out = usb2_slot_.tex;
    return true;
}

bool CameraManager::get_usb3(GLuint& out) {
    std::lock_guard<std::mutex> lk(usb3_slot_.mtx);
    if (!usb3_slot_.dirty || usb3_slot_.buf.empty()) { out = usb3_slot_.tex; return false; }
    upload_texture(usb3_slot_.tex, usb3_slot_.w, usb3_slot_.h, usb3_slot_.buf.data(),
                   usb3_slot_.tex_w, usb3_slot_.tex_h);
    usb3_slot_.dirty = false;
    out = usb3_slot_.tex;
    return true;
}

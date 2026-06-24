#include "dma_camera.h"
#include "../gl_utils.h"

#include <libcamera/libcamera.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <algorithm>
#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <thread>
#include <vector>

// DRM fourcc codes — avoids depending on drm/drm_fourcc.h
#define DRM_FORMAT_NV12   0x3231564Eu  // semi-planar 4:2:0: Y + interleaved UV
#define DRM_FORMAT_YUV420 0x32315559u  // planar 4:2:0: Y + U + V
#define DRM_FORMAT_YUYV   0x56595559u  // packed 4:2:2: Y0 Cb Y1 Cr interleaved

using namespace libcamera;

// ── Construction / destruction ────────────────────────────────────────────────

DmaCamera::DmaCamera()  = default;
DmaCamera::~DmaCamera() { shutdown(); }

// ── init() — must be called from render thread ────────────────────────────────

bool DmaCamera::init(CameraManager* lcam_mgr, const Config& cfg,
                     const char* vs_path, const char* fs_path) {
    cfg_      = cfg;
    lcam_mgr_ = lcam_mgr;

    if (!load_egl_procs())           { std::cerr << "[dma] EGL procs unavailable\n"; return false; }
    if (!configure_camera())         return false;
    if (!allocate_buffers_and_egl()) return false;
    if (!create_gl_resources(vs_path, fs_path)) return false;

    start_capture();
    ok_ = true;
    return true;
}

// ── Load EGL extension function pointers ─────────────────────────────────────

bool DmaCamera::load_egl_procs() {
    egl_display_ = eglGetCurrentDisplay();
    if (egl_display_ == EGL_NO_DISPLAY) {
        std::cerr << "[dma] no current EGL display — call init() after GL context is current\n";
        return false;
    }

    pfn_create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    pfn_destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    pfn_img_target_tex_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!pfn_create_image_ || !pfn_destroy_image_ || !pfn_img_target_tex_) {
        std::cerr << "[dma] required EGL/GL extensions not available\n"
                  << "      Need: EGL_KHR_image_base, GL_OES_EGL_image\n";
        return false;
    }
    return true;
}

// ── libcamera configuration ───────────────────────────────────────────────────

bool DmaCamera::configure_camera() {
    auto cameras = lcam_mgr_->cameras();

    // ── Select camera ─────────────────────────────────────────────────────────
    // Preferred: an exact libcamera id string resolved once by CameraManager (so
    // both eyes get distinct cameras regardless of enumeration order between calls).
    if (!cfg_.camera_id.empty()) {
        camera_ = lcam_mgr_->get(cfg_.camera_id);
        if (!camera_)
            std::cerr << "[dma] camera id '" << cfg_.camera_id << "' not found\n";
    }
    // When several cameras share the model (e.g. two OWLsights = two ov64a40),
    // pick the libcamera_id-th MATCH, not just the first — otherwise both eyes
    // resolve to the same physical camera and the second acquire() fails with
    // "Camera in Running state".
    if (!camera_ && !cfg_.model_name.empty()) {
        int match = 0;
        for (auto& desc : cameras) {
            auto c = lcam_mgr_->get(desc->id());
            try {
                auto m = c->properties().get(properties::Model);
                if (m && *m == cfg_.model_name) {
                    if (match == cfg_.libcamera_id) { camera_ = c; break; }
                    ++match;
                }
            } catch (...) {}
        }
        if (!camera_)
            std::cerr << "[dma] model '" << cfg_.model_name << "' #" << cfg_.libcamera_id
                      << " not found, falling back to id " << cfg_.libcamera_id << "\n";
    }
    if (!camera_) {
        if (static_cast<int>(cameras.size()) <= cfg_.libcamera_id) {
            std::cerr << "[dma] camera id " << cfg_.libcamera_id
                      << " not found (" << cameras.size() << " cameras)\n";
            return false;
        }
        camera_ = lcam_mgr_->get(cameras[cfg_.libcamera_id]->id());
    }
    if (!camera_ || camera_->acquire()) {
        std::cerr << "[dma] failed to acquire camera id " << cfg_.libcamera_id
                  << " — already in use? (check for a stray protohud, or reboot if a "
                     "camera got wedged)\n";
        return false;
    }

    // ── Log supported modes so users can identify valid resolutions ───────────
    {
        auto probe = camera_->generateConfiguration({ StreamRole::Viewfinder });
        std::cout << "[dma] camera " << cfg_.libcamera_id << " supported modes:\n";
        for (size_t i = 0; i < probe->size(); ++i) {
            const auto& s = probe->at(i);
            std::cout << "  [" << i << "] " << s.pixelFormat
                      << " " << s.size.width << "×" << s.size.height << "\n";
        }
    }

    // ── Format negotiation: NV12 → YUV420 → YUYV ─────────────────────────────
    // NV12 is preferred (native ISP semi-planar, zero-copy via EGL).
    // YUV420 (3-plane) and YUYV (packed) are fallbacks for sensors/bridges that
    // do not expose NV12. The samplerExternalOES path handles all three on V3D.
    const struct { libcamera::PixelFormat fmt; uint32_t drm; int planes; const char* name; }
    kFmtPrefs[] = {
        { formats::NV12,   DRM_FORMAT_NV12,   2, "NV12"   },
        { formats::YUV420, DRM_FORMAT_YUV420, 3, "YUV420" },
        { formats::YUYV,   DRM_FORMAT_YUYV,   1, "YUYV"   },
    };

    bool configured = false;
    for (const auto& fp : kFmtPrefs) {
        cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        auto& sc = cam_cfg_->at(0);
        sc.pixelFormat = fp.fmt;
        sc.size        = { static_cast<unsigned>(cfg_.width),
                           static_cast<unsigned>(cfg_.height) };
        sc.bufferCount = NUM_SLOTS;
        auto st = cam_cfg_->validate();
        if (st == CameraConfiguration::Invalid) continue;

        if (st == CameraConfiguration::Adjusted) {
            std::cout << "[dma] size adjusted: "
                      << cfg_.width << "×" << cfg_.height
                      << " → " << sc.size.width << "×" << sc.size.height << "\n";
            cfg_.width  = sc.size.width;
            cfg_.height = sc.size.height;
        }
        fmt_drm_    = fp.drm;
        fmt_planes_ = fp.planes;
        configured  = true;
        std::cout << "[dma] camera " << cfg_.libcamera_id
                  << " using format " << fp.name << "\n";
        break;
    }
    if (!configured) {
        std::cerr << "[dma] no supported pixel format (tried NV12, YUV420, YUYV)\n";
        return false;
    }

    if (camera_->configure(cam_cfg_.get())) {
        std::cerr << "[dma] camera_->configure() failed\n";
        return false;
    }

    auto& sc = cam_cfg_->at(0);
    stream_ = sc.stream();
    stride_ = sc.stride;

    // ── Enumerate the sensor's supported resolutions for this format ──────────
    // Captured once so the menu can offer the camera's REAL modes instead of a
    // fixed preset list (the cause of "pick 1440p, nothing changes" on sensors
    // that don't support it). sizes() is the discrete set libcamera reports for
    // the negotiated pixel format; sorted largest-area first for stable order.
    // max_fps is left 0 here (the in-depth path can probe FrameDurationLimits
    // per mode later); selecting a mode requests a target fps and libcamera
    // clamps it to what the sensor can do at that size.
    modes_.clear();
    try {
        const auto& sf  = sc.formats();
        auto discrete   = sf.sizes(sc.pixelFormat);
        if (!discrete.empty()) {
            for (const auto& s : discrete)
                modes_.push_back({ static_cast<int>(s.width),
                                   static_cast<int>(s.height), 0 });
        } else {
            // The Raspberry Pi ISP pipeline reports a CONTINUOUS size range, not
            // a discrete list, so sizes() is empty. Offer the standard
            // resolutions that fall inside [min,max] (honouring the step), plus
            // the sensor's native max — that's what makes per-camera options show.
            const auto rng = sf.range(sc.pixelFormat);
            const int minw = static_cast<int>(rng.min.width);
            const int minh = static_cast<int>(rng.min.height);
            const int maxw = static_cast<int>(rng.max.width);
            const int maxh = static_cast<int>(rng.max.height);
            const int hs   = rng.hStep > 0 ? static_cast<int>(rng.hStep) : 2;
            const int vs   = rng.vStep > 0 ? static_cast<int>(rng.vStep) : 2;
            static const int kCand[][2] = {
                {640,480}, {1280,720}, {1280,800}, {1456,1088}, {1536,864},
                {1920,1080}, {2304,1296}, {2560,1440}, {3840,2160},
            };
            auto fits = [&](int w, int h){
                return w >= minw && w <= maxw && h >= minh && h <= maxh
                    && (w - minw) % hs == 0 && (h - minh) % vs == 0;
            };
            for (const auto& c : kCand)
                if (fits(c[0], c[1])) modes_.push_back({ c[0], c[1], 0 });
            if (maxw > 0 && maxh > 0) modes_.push_back({ maxw, maxh, 0 }); // native
        }
        std::sort(modes_.begin(), modes_.end(),
                  [](const Mode& a, const Mode& b){
                      return static_cast<long>(a.width) * a.height
                           > static_cast<long>(b.width) * b.height;
                  });
        modes_.erase(std::unique(modes_.begin(), modes_.end(),
                     [](const Mode& a, const Mode& b){
                         return a.width == b.width && a.height == b.height;
                     }), modes_.end());
    } catch (...) {}

    // ── Probe each mode's max fps ─────────────────────────────────────────────
    // Configure the camera at each size and read the shortest frame duration the
    // FrameDurationLimits control allows → max fps (this is the figure
    // `rpicam-hello --list-cameras` prints). We do NOT start streaming, so the
    // libpisp "TDN" abort (a streaming-time bug) isn't triggered. Afterwards we
    // restore the real target configuration before buffers are allocated.
    // Bounded so a sensor with a long discrete-mode list doesn't stall startup.
    if (!modes_.empty()) {
        const PixelFormat pf = cam_cfg_->at(0).pixelFormat;
        int probed = 0;
        for (auto& m : modes_) {
            if (probed >= 40) break;
            ++probed;
            try {
                auto pc = camera_->generateConfiguration({ StreamRole::Viewfinder });
                if (!pc || pc->empty()) continue;
                auto& psc = pc->at(0);
                psc.pixelFormat = pf;
                psc.size = { static_cast<unsigned>(m.width),
                             static_cast<unsigned>(m.height) };
                if (pc->validate() == CameraConfiguration::Invalid) continue;
                if (camera_->configure(pc.get()) != 0) continue;
                const auto& cim = camera_->controls();
                if (cim.count(&controls::FrameDurationLimits)) {
                    int64_t min_dur =
                        cim.at(&controls::FrameDurationLimits).min().get<int64_t>();
                    if (min_dur > 0)
                        m.max_fps = static_cast<int>((1000000LL + min_dur / 2) / min_dur);
                }
            } catch (...) {}
        }
        // Restore the configuration we actually want to run.
        camera_->configure(cam_cfg_.get());
        stream_ = cam_cfg_->at(0).stream();
        stride_ = cam_cfg_->at(0).stride;
    }

    std::cout << "[dma] camera " << cfg_.libcamera_id
              << " configured: " << cfg_.width << "×" << cfg_.height
              << " stride=" << stride_ << " fps=" << cfg_.fps
              << " (" << modes_.size() << " modes reported)\n";
    return true;
}

// ── Buffer allocation + EGLImage creation ─────────────────────────────────────

bool DmaCamera::allocate_buffers_and_egl() {
    requests_.clear();   // drop any requests left over from a failed attempt

    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
    if (allocator_->allocate(stream_) < 0) {
        std::cerr << "[dma] frame buffer allocation failed\n";
        return false;
    }

    const auto& buffers = allocator_->buffers(stream_);
    if (static_cast<int>(buffers.size()) < NUM_SLOTS) {
        std::cerr << "[dma] got fewer buffers than NUM_SLOTS\n";
        return false;
    }

    for (int i = 0; i < NUM_SLOTS; i++) {
        FrameBuffer* buf = buffers[i].get();
        slots_[i].buffer = buf;
        slots_[i].state  = SlotState::IDLE;
        buf_to_slot_[buf] = i;

        if (!create_egl_image(buf, slots_[i])) return false;

        // Create libcamera Request (one per slot). createRequest() hands us
        // ownership — store it in requests_ (freed on shutdown/reconfigure)
        // and keep a raw pointer in the slot for queueRequest/reuse.
        auto req = camera_->createRequest();
        if (!req) {
            std::cerr << "[dma] createRequest failed for slot " << i << "\n";
            return false;
        }
        slots_[i].request = req.get();
        slots_[i].request->addBuffer(stream_, buf);
        req_to_slot_[slots_[i].request] = i;
        requests_.push_back(std::move(req));
    }
    return true;
}

bool DmaCamera::create_egl_image(const FrameBuffer* buf, Slot& slot) {
    const auto& planes = buf->planes();
    if (static_cast<int>(planes.size()) < fmt_planes_) {
        std::cerr << "[dma] buffer has " << planes.size()
                  << " planes but format requires " << fmt_planes_ << "\n";
        return false;
    }

    auto pitch = static_cast<EGLint>(stride_);

    // Build the EGL attribute list for the negotiated DRM format.
    // NV12 / YUV420 / YUYV all use the same EGL_LINUX_DMA_BUF_EXT path;
    // only the fourcc and plane count differ.
    std::vector<EGLint> attr = {
        EGL_WIDTH,                         cfg_.width,
        EGL_HEIGHT,                        cfg_.height,
        EGL_LINUX_DRM_FOURCC_EXT,          (EGLint)fmt_drm_,
        // Plane 0 — Y (luma) or packed data (YUYV)
        EGL_DMA_BUF_PLANE0_FD_EXT,         planes[0].fd.get(),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,     (EGLint)planes[0].offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,      pitch,
    };
    if (fmt_planes_ >= 2) {
        // NV12: UV plane has the same row stride as Y (interleaved, same width).
        // YUV420: U plane is half width, so stride is halved.
        EGLint plane1_pitch = (fmt_drm_ == DRM_FORMAT_NV12) ? pitch : pitch / 2;
        attr.insert(attr.end(), {
            EGL_DMA_BUF_PLANE1_FD_EXT,     planes[1].fd.get(),
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)planes[1].offset,
            EGL_DMA_BUF_PLANE1_PITCH_EXT,  plane1_pitch,
        });
    }
    if (fmt_planes_ >= 3) {
        // Plane 2 — V/Cr (YUV420 only)
        attr.insert(attr.end(), {
            EGL_DMA_BUF_PLANE2_FD_EXT,     planes[2].fd.get(),
            EGL_DMA_BUF_PLANE2_OFFSET_EXT, (EGLint)planes[2].offset,
            EGL_DMA_BUF_PLANE2_PITCH_EXT,  pitch / 2,
        });
    }
    attr.push_back(EGL_NONE);

    slot.img_y = pfn_create_image_(egl_display_, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, nullptr, attr.data());
    if (slot.img_y == EGL_NO_IMAGE_KHR) {
        std::cerr << "[dma] eglCreateImageKHR failed (err 0x"
                  << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }
    slot.img_uv = EGL_NO_IMAGE_KHR;  // unused — full frame encoded in img_y
    return true;
}

// ── GL resources (textures + shader + quad VBO) ───────────────────────────────

bool DmaCamera::create_gl_resources(const char* vs_path, const char* fs_path) {
    // Single GL_TEXTURE_EXTERNAL_OES texture — the driver handles NV12 plane layout.
    // Only GL_CLAMP_TO_EDGE and GL_LINEAR/GL_NEAREST are valid for external OES.
    glGenTextures(1, &tex_y_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_y_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    // NV12 shader — uses samplerExternalOES (driver handles YCbCr→RGB)
    nv12_prog_ = gl::build_program(vs_path, fs_path);
    if (!nv12_prog_) {
        std::cerr << "[dma] NV12 shader failed to load\n";
        return false;
    }

    // Cache uniform locations
    loc_tex_y_  = glGetUniformLocation(nv12_prog_, "tex");
    loc_zoom_   = glGetUniformLocation(nv12_prog_, "u_zoom");
    loc_center_ = glGetUniformLocation(nv12_prog_, "u_center");
    loc_rot_    = glGetUniformLocation(nv12_prog_, "u_rotation_rad");

    // Seed from cfg now so the first frame already shows the configured
    // orientation; set_rotation snaps the value.
    set_rotation(cfg_.rotation_deg);

    glUseProgram(nv12_prog_);
    if (loc_tex_y_ >= 0) glUniform1i(loc_tex_y_, 0);   // GL_TEXTURE0
    if (loc_zoom_   >= 0) glUniform1f(loc_zoom_,  1.0f);
    if (loc_center_ >= 0) glUniform2f(loc_center_, 0.5f, 0.5f);
    if (loc_rot_    >= 0) glUniform1f(loc_rot_,   0.0f);
    glUseProgram(0);

    // Fullscreen quad VBO (NDC, shared across draws)
    quad_vbo_ = gl::make_quad_vbo();

    return true;
}

// ── Capture ───────────────────────────────────────────────────────────────────

void DmaCamera::start_capture() {
    camera_->requestCompleted.connect(this, &DmaCamera::on_request_complete);

    running_ = true;
    event_thread_ = std::thread([this] { event_loop(); });

    // Set frame rate via FrameDurationLimits.
    // libcamera on Bullseye does not expose frameRate on StreamConfiguration.
    ControlList startCtrls(controls::controls);
    try {
        if (camera_->controls().count(&controls::FrameDurationLimits)) {
            int64_t fd = 1000000LL / std::max(1, cfg_.fps);
            startCtrls.set(controls::FrameDurationLimits,
                           Span<const int64_t, 2>({ fd, fd }));
        }
    } catch (...) {}

    if (camera_->start(&startCtrls)) {
        // Older libcamera build may not accept a ControlList on start(); retry bare.
        if (camera_->start()) {
            std::cerr << "[dma] camera_->start() failed\n";
            return;
        }
    }

    // Queue all slots for initial capture
    std::lock_guard<std::mutex> lk(handoff_mtx_);
    for (int i = 0; i < NUM_SLOTS; i++) {
        slots_[i].state = SlotState::CAPTURING;
        camera_->queueRequest(slots_[i].request);
    }
}

void DmaCamera::event_loop() {
    // The libcamera CameraManager on Pi OS Bullseye dispatches events
    // internally; we do not need to call processEvents() ourselves.
    // This thread simply keeps running so that the thread handle stays
    // valid for the lifetime of the capture session. Block until stop —
    // the previous 5 ms poll burned ~200 scheduler wakeups/s per camera
    // doing nothing.
    std::unique_lock<std::mutex> lk(stop_mtx_);
    stop_cv_.wait(lk, [this] { return !running_.load(); });
}

// ── Request-complete callback (capture thread) ────────────────────────────────

void DmaCamera::on_request_complete(Request* req) {
    if (!running_ || req->status() != Request::RequestComplete) return;

    // ── Save metadata from the completed request (atomic writes) ─────────────
    try {
        const auto& meta     = req->metadata();
        const auto& camCtrls = camera_->controls();   // ControlInfoMap (supported)
        // ControlList::get() returns std::optional<T> in libcamera >= 0.7
        if (camCtrls.count(&controls::AfState))
            last_af_state_.store(meta.get(controls::AfState).value_or(0));
        if (camCtrls.count(&controls::LensPosition))
            last_lens_pos_.store(meta.get(controls::LensPosition).value_or(0.0f));
        if (camCtrls.count(&controls::AnalogueGain))
            last_analogue_gain_.store(meta.get(controls::AnalogueGain).value_or(1.0f));
        // Real per-frame duration (µs) → drives measured_fps() in the menu.
        last_frame_dur_us_.store(meta.get(controls::FrameDuration).value_or(0));
    } catch (...) {}

    auto it = req_to_slot_.find(req);
    if (it == req_to_slot_.end()) return;
    int slot_idx = it->second;

    std::lock_guard<std::mutex> lk(handoff_mtx_);

    // If there's already an unconsumed READY frame, drop it and requeue it
    // with any pending control updates.
    if (ready_slot_ >= 0) {
        int drop = ready_slot_;
        slots_[drop].state = SlotState::CAPTURING;
        auto* drop_req = slots_[drop].request;
        drop_req->reuse(Request::ReuseBuffers);
        apply_pending_controls(drop_req->controls());
        camera_->queueRequest(drop_req);
    }

    slots_[slot_idx].state = SlotState::READY;
    ready_slot_ = slot_idx;
}

// ── draw() — render thread ────────────────────────────────────────────────────
// Draws the latest frame filling the current GL viewport.
// Returns false if no frame has ever arrived.

bool DmaCamera::draw(float zoom, float cx, float cy) {
    int slot = -1;

    {
        std::lock_guard<std::mutex> lk(handoff_mtx_);
        if (ready_slot_ < 0) {
            // No new frame — keep displaying the previous one if available
            if (render_slot_ < 0) return false;
            slot = render_slot_;
        } else {
            // Promote ready → rendering; release previous render slot back to camera
            if (render_slot_ >= 0) {
                slots_[render_slot_].state = SlotState::CAPTURING;
                auto* prev = slots_[render_slot_].request;
                prev->reuse(Request::ReuseBuffers);
                apply_pending_controls(prev->controls());
                camera_->queueRequest(prev);
            }
            render_slot_ = ready_slot_;
            ready_slot_  = -1;
            slots_[render_slot_].state = SlotState::RENDERING;
            slot = render_slot_;
        }
    }

    // ── Bind NV12 EGLImage to the external OES texture ───────────────────────
    // Re-binding each frame is lightweight (just updates the texture backing pointer).

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_y_);
    pfn_img_target_tex_(GL_TEXTURE_EXTERNAL_OES, slots_[slot].img_y);

    // ── Draw fullscreen NV12 → RGB quad ──────────────────────────────────────

    glUseProgram(nv12_prog_);

    // Apply digital zoom; clamp zoom to [1.0, 8.0] to avoid degenerate UVs.
    float safe_zoom = (zoom < 1.0f) ? 1.0f : (zoom > 8.0f ? 8.0f : zoom);
    if (loc_zoom_   >= 0) glUniform1f(loc_zoom_,  safe_zoom);
    if (loc_center_ >= 0) glUniform2f(loc_center_, cx, cy);
    if (loc_rot_    >= 0) {
        const float rad = static_cast<float>(rotation_deg_.load()) *
                          3.14159265358979323846f / 180.0f;
        glUniform1f(loc_rot_, rad);
    }

    gl::bind_quad(quad_vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glUseProgram(0);

    // Unbind the external OES texture (leave texture unit 0 active, unbound)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    return true;
}

void DmaCamera::set_rotation(int deg) {
    // Snap to the nearest 90° step and wrap into [0, 360). Anything else
    // would skew the camera image — there's no rendering reason to allow
    // arbitrary angles here and it'd confuse the menu UI.
    int snapped = ((deg + 45) / 90) * 90;
    snapped = ((snapped % 360) + 360) % 360;
    rotation_deg_.store(snapped);
}

// ── shutdown ──────────────────────────────────────────────────────────────────

void DmaCamera::shutdown() {
    // Guard against double-shutdown, but always release camera_ if it was acquired
    // (camera_ is set in configure_camera() before EGL work, so it must be
    // cleaned up even when ok_ is false due to a later EGL failure).
    if (!ok_ && !running_ && !camera_) return;

    ok_ = false;
    {
        // Store under stop_mtx_ so event_loop can't miss the wakeup between
        // its predicate check and the wait.
        std::lock_guard<std::mutex> lk(stop_mtx_);
        running_ = false;
    }
    stop_cv_.notify_all();
    if (event_thread_.joinable()) event_thread_.join();

    if (camera_) {
        camera_->stop();  // safe to call even if capture was never started
        camera_->requestCompleted.disconnect(this, &DmaCamera::on_request_complete);
    }

    // Destroy EGL images
    for (auto& s : slots_) {
        if (s.img_y  != EGL_NO_IMAGE_KHR && pfn_destroy_image_)
            pfn_destroy_image_(egl_display_, s.img_y);
        if (s.img_uv != EGL_NO_IMAGE_KHR && pfn_destroy_image_)
            pfn_destroy_image_(egl_display_, s.img_uv);
        s.img_y = s.img_uv = EGL_NO_IMAGE_KHR;
        s.request = nullptr;   // owned by requests_, freed below
    }
    // Free the requests we own (camera is stopped — none are in flight).
    requests_.clear();
    req_to_slot_.clear();

    if (tex_y_)     { glDeleteTextures(1, &tex_y_);    tex_y_    = 0; }
    if (nv12_prog_) { glDeleteProgram(nv12_prog_);     nv12_prog_ = 0; }
    if (quad_vbo_)  { glDeleteBuffers(1, &quad_vbo_);  quad_vbo_  = 0; }

    if (allocator_) allocator_->free(stream_);
    if (camera_)    { camera_->release(); camera_.reset(); }
}

// ── reconfigure() — hot-swap resolution / fps ────────────────────────────────
// Stops capture, reconfigures libcamera, reallocates DMA+EGL buffers, restarts.
// The GL shader and quad VBO are preserved — they don't depend on resolution.

bool DmaCamera::reconfigure(int width, int height, int fps) {
    if (!camera_) return false;

    // 1. Stop capture thread
    {
        std::lock_guard<std::mutex> lk(stop_mtx_);
        running_ = false;
    }
    stop_cv_.notify_all();
    if (event_thread_.joinable()) event_thread_.join();

    camera_->stop();
    camera_->requestCompleted.disconnect(this, &DmaCamera::on_request_complete);

    // 2. Destroy EGL images for all slots
    for (auto& s : slots_) {
        if (s.img_y  != EGL_NO_IMAGE_KHR && pfn_destroy_image_)
            pfn_destroy_image_(egl_display_, s.img_y);
        if (s.img_uv != EGL_NO_IMAGE_KHR && pfn_destroy_image_)
            pfn_destroy_image_(egl_display_, s.img_uv);
        s.img_y     = EGL_NO_IMAGE_KHR;
        s.img_uv    = EGL_NO_IMAGE_KHR;
        s.buffer    = nullptr;
        s.request   = nullptr;
        s.state     = SlotState::IDLE;
    }

    // 3. Free the old requests (camera stopped, none in flight) and the
    //    libcamera buffer allocations
    requests_.clear();
    if (allocator_) {
        allocator_->free(stream_);
        allocator_.reset();
    }
    buf_to_slot_.clear();
    req_to_slot_.clear();
    {
        std::lock_guard<std::mutex> lk(handoff_mtx_);
        ready_slot_  = -1;
        render_slot_ = -1;
    }

    // 4. Save old config in case we need to roll back
    int old_w = cfg_.width, old_h = cfg_.height, old_fps = cfg_.fps;
    cfg_.width  = width;
    cfg_.height = height;
    cfg_.fps    = fps;

    // 5. Reconfigure — reuse the same format that was negotiated at init time.
    // Changing resolution does not change the pixel format, so we keep fmt_drm_/fmt_planes_.
    const struct { libcamera::PixelFormat fmt; uint32_t drm; int planes; } kPrefs[] = {
        { formats::NV12,   0x3231564Eu, 2 },
        { formats::YUV420, 0x32315559u, 3 },
        { formats::YUYV,   0x56595559u, 1 },
    };

    bool recfg_ok = false;
    for (const auto& fp : kPrefs) {
        cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        auto& sc = cam_cfg_->at(0);
        sc.pixelFormat = fp.fmt;
        sc.size        = { static_cast<unsigned>(cfg_.width),
                           static_cast<unsigned>(cfg_.height) };
        sc.bufferCount = NUM_SLOTS;
        auto st = cam_cfg_->validate();
        if (st == CameraConfiguration::Invalid) continue;
        if (st == CameraConfiguration::Adjusted) {
            std::cout << "[dma] reconfigure: size adjusted "
                      << cfg_.width << "×" << cfg_.height
                      << " → " << sc.size.width << "×" << sc.size.height << "\n";
            cfg_.width  = sc.size.width;
            cfg_.height = sc.size.height;
        }
        fmt_drm_    = fp.drm;
        fmt_planes_ = fp.planes;
        recfg_ok    = true;
        break;
    }
    if (!recfg_ok) {
        std::cerr << "[dma] reconfigure: no valid format found — reverting\n";
        cfg_.width = old_w;  cfg_.height = old_h;  cfg_.fps = old_fps;
        if (allocate_buffers_and_egl()) start_capture();
        return false;
    }

    if (camera_->configure(cam_cfg_.get())) {
        std::cerr << "[dma] reconfigure: camera_->configure() failed — reverting\n";
        cfg_.width = old_w;  cfg_.height = old_h;  cfg_.fps = old_fps;
        cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        cam_cfg_->at(0).size = { static_cast<unsigned>(old_w), static_cast<unsigned>(old_h) };
        cam_cfg_->validate();
        camera_->configure(cam_cfg_.get());
        stream_ = cam_cfg_->at(0).stream();
        stride_ = cam_cfg_->at(0).stride;
        if (allocate_buffers_and_egl()) start_capture();
        return false;
    }

    auto& sc = cam_cfg_->at(0);
    stream_ = sc.stream();
    stride_ = sc.stride;

    // 6. Reallocate DMA buffers and EGL images
    if (!allocate_buffers_and_egl()) {
        std::cerr << "[dma] reconfigure: buffer allocation failed\n";
        ok_ = false;
        return false;
    }

    // 7. Restart capture
    start_capture();

    std::cout << "[dma] reconfigured camera " << cfg_.libcamera_id
              << " → " << cfg_.width << "×" << cfg_.height
              << " @" << cfg_.fps << "fps\n";
    return true;
}

// ── Camera controls API ───────────────────────────────────────────────────────
//
// Controls cannot be set directly on a running camera via camera_->setControls()
// (that method does not exist in the Bullseye libcamera).  Instead we use a
// "pending controls" pattern:
//   • The public set_* functions write to atomic members (pending_*).
//   • apply_pending_controls() is called just before each request is requeued
//     (in on_request_complete and draw()), injecting the controls into the
//     Request::controls() ControlList for the next capture cycle.
//   • AF state and lens position are read back from request metadata and stored
//     in last_af_state_ / last_lens_pos_ atomics.

void DmaCamera::apply_pending_controls(ControlList& ctrls) {
    int af = pending_af_mode_.exchange(-1);
    if (af >= 0)
        ctrls.set(controls::AfMode, af);

    // AeEnable must be applied before ExposureTime so manual shutter takes effect.
    int ae = pending_ae_enable_.exchange(-1);
    if (ae >= 0)
        ctrls.set(controls::AeEnable, ae == 1);

    float ev = pending_ev_.exchange(-9999.0f);
    if (ev > -9998.0f)
        ctrls.set(controls::ExposureValue, ev);

    int sh = pending_shutter_us_.exchange(-1);
    if (sh > 0)
        ctrls.set(controls::ExposureTime, sh);

    float lp = pending_lens_pos_.exchange(-1.0f);
    if (lp >= 0.0f)
        ctrls.set(controls::LensPosition, lp);

    int awb = pending_awb_enable_.exchange(-1);
    if (awb >= 0 && camera_->controls().count(&controls::AwbEnable))
        ctrls.set(controls::AwbEnable, awb == 1);

    float rg = pending_rg_gain_.exchange(-1.0f);
    float bg = pending_bg_gain_.exchange(-1.0f);
    if (rg >= 0.0f && bg >= 0.0f && camera_->controls().count(&controls::ColourGains)) {
        float gains[2] = { rg, bg };
        ctrls.set(controls::ColourGains, Span<const float, 2>(gains));
    }

    // ── Extended controls ─────────────────────────────────────────────────────
    // Menu values are chosen to match libcamera's enum numeric values, so each
    // is forwarded straight through. count() guards mean a sensor that lacks a
    // control silently ignores it rather than erroring.
    const auto& cc = camera_->controls();

    int afr = pending_af_range_.exchange(-1);
    if (afr >= 0 && cc.count(&controls::AfRange)) ctrls.set(controls::AfRange, afr);

    int afs = pending_af_speed_.exchange(-1);
    if (afs >= 0 && cc.count(&controls::AfSpeed)) ctrls.set(controls::AfSpeed, afs);

    float ag = pending_gain_.exchange(-1.0f);
    if (ag > 0.0f && cc.count(&controls::AnalogueGain)) ctrls.set(controls::AnalogueGain, ag);

    int aem = pending_ae_metering_.exchange(-1);
    if (aem >= 0 && cc.count(&controls::AeMeteringMode)) ctrls.set(controls::AeMeteringMode, aem);

    int aecn = pending_ae_constraint_.exchange(-1);
    if (aecn >= 0 && cc.count(&controls::AeConstraintMode)) ctrls.set(controls::AeConstraintMode, aecn);

    int aex = pending_ae_exp_mode_.exchange(-1);
    if (aex >= 0 && cc.count(&controls::AeExposureMode)) ctrls.set(controls::AeExposureMode, aex);

    int fl = pending_flicker_.exchange(-1);
    if (fl >= 0 && cc.count(&controls::AeFlickerMode)) {
        // libcamera AeFlickerMode: 0 Off, 1 Manual, 2 Auto. Menu: 0 Off, 1 Auto,
        // 2 = 50 Hz, 3 = 60 Hz → Manual + an explicit period (µs).
        if (fl == 0)      ctrls.set(controls::AeFlickerMode, 0);   // Off
        else if (fl == 1) ctrls.set(controls::AeFlickerMode, 2);   // Auto
        else {
            ctrls.set(controls::AeFlickerMode, 1);                 // Manual
            if (cc.count(&controls::AeFlickerPeriod))
                ctrls.set(controls::AeFlickerPeriod, fl == 2 ? 10000 : 8333); // 50/60 Hz
        }
    }

    int awbm = pending_awb_mode_.exchange(-1);
    if (awbm >= 0 && cc.count(&controls::AwbMode)) {
        if (cc.count(&controls::AwbEnable)) ctrls.set(controls::AwbEnable, true);
        ctrls.set(controls::AwbMode, awbm);
    }

    float br = pending_brightness_.exchange(-9999.0f);
    if (br > -9998.0f && cc.count(&controls::Brightness)) ctrls.set(controls::Brightness, br);

    float ct = pending_contrast_.exchange(-1.0f);
    if (ct >= 0.0f && cc.count(&controls::Contrast)) ctrls.set(controls::Contrast, ct);

    float sa = pending_saturation_.exchange(-1.0f);
    if (sa >= 0.0f && cc.count(&controls::Saturation)) ctrls.set(controls::Saturation, sa);

    float sp = pending_sharpness_.exchange(-1.0f);
    if (sp >= 0.0f && cc.count(&controls::Sharpness)) ctrls.set(controls::Sharpness, sp);

    int nr = pending_nr_.exchange(-1);
    if (nr >= 0 && cc.count(&controls::draft::NoiseReductionMode))
        ctrls.set(controls::draft::NoiseReductionMode, nr);

    int hdr = pending_hdr_.exchange(-1);
    if (hdr >= 0 && cc.count(&controls::HdrMode)) ctrls.set(controls::HdrMode, hdr);
}

void DmaCamera::start_autofocus() {
    if (!camera_) return;
    if (camera_->controls().count(&controls::AfMode))
        // AfModeContinuous: scans immediately and keeps tracking without needing
        // a separate AfTrigger = AfTriggerStart control.
        // AfModeAuto (one-shot stills mode) does nothing until a trigger arrives,
        // which is why the startup autofocus appeared to have no effect.
        pending_af_mode_.store(controls::AfModeContinuous);
}

void DmaCamera::stop_autofocus() {
    if (!camera_) return;
    if (camera_->controls().count(&controls::AfMode))
        pending_af_mode_.store(controls::AfModeManual);
}

void DmaCamera::set_focus_position(int pos) {
    if (!camera_ || pos < 0 || pos > 1000) return;
    if (camera_->controls().count(&controls::LensPosition)) {
        // LensPosition is in diopters: 0.0 = infinity, ~10.0 = closest.
        // Map our 0-1000 scale linearly over 0-10 diopters.
        pending_lens_pos_.store((pos / 1000.0f) * 10.0f);
    }
}

int DmaCamera::get_focus_position() const {
    // Convert diopters (0-10) back to our 0-1000 scale.
    float lp = last_lens_pos_.load();
    return static_cast<int>((lp / 10.0f) * 1000.0f);
}

bool DmaCamera::is_af_locked() const {
    return last_af_state_.load() == controls::AfStateFocused;
}

bool DmaCamera::is_af_scanning() const {
    return last_af_state_.load() == controls::AfStateScanning;
}

void DmaCamera::set_exposure_ev(float ev) {
    if (!camera_) return;
    if (camera_->controls().count(&controls::ExposureValue))
        pending_ev_.store(ev);
}

void DmaCamera::set_shutter_speed_us(int us) {
    if (!camera_) return;
    if (camera_->controls().count(&controls::ExposureTime))
        pending_shutter_us_.store(us);
}

void DmaCamera::set_ae_enable(bool ae_on) {
    if (!camera_) return;
    if (camera_->controls().count(&controls::AeEnable))
        pending_ae_enable_.store(ae_on ? 1 : 0);
}

void DmaCamera::set_awb_enable(bool awb_on) {
    if (!camera_) return;
    if (camera_->controls().count(&controls::AwbEnable))
        pending_awb_enable_.store(awb_on ? 1 : 0);
}

void DmaCamera::set_colour_gains(float rg, float bg) {
    if (!camera_) return;
    if (camera_->controls().count(&controls::ColourGains)) {
        pending_rg_gain_.store(rg);
        pending_bg_gain_.store(bg);
    }
}

void DmaCamera::set_colour_temp(int kelvin) {
    // Approximate Pi ISP ColourGains (Rg, Bg) for common colour temperatures.
    // Lower Kelvin = warmer/more red = higher Rg, lower Bg.
    // Higher Kelvin = cooler/more blue = lower Rg, higher Bg.
    static const struct { int k; float rg, bg; } lut[] = {
        { 2800, 2.20f, 1.15f },
        { 3500, 1.90f, 1.40f },
        { 4500, 1.60f, 1.65f },
        { 5600, 1.30f, 1.90f },
        { 7000, 1.05f, 2.30f },
    };
    static constexpr int n = sizeof(lut) / sizeof(lut[0]);
    if (kelvin <= lut[0].k)   { set_colour_gains(lut[0].rg, lut[0].bg); return; }
    if (kelvin >= lut[n-1].k) { set_colour_gains(lut[n-1].rg, lut[n-1].bg); return; }
    for (int i = 1; i < n; i++) {
        if (kelvin <= lut[i].k) {
            float t = static_cast<float>(kelvin - lut[i-1].k) /
                      static_cast<float>(lut[i].k - lut[i-1].k);
            set_colour_gains(lut[i-1].rg + t * (lut[i].rg - lut[i-1].rg),
                             lut[i-1].bg + t * (lut[i].bg - lut[i-1].bg));
            return;
        }
    }
}

// ── Extended controls ─────────────────────────────────────────────────────────
// Each setter records the value for the menu (cur_*) and queues it for the next
// request (pending_*). apply_pending_controls() guards each by controls().count()
// so a sensor that lacks a given control simply ignores it.
void DmaCamera::set_af_range(int range) {
    if (!camera_) return;
    cur_af_range_.store(range); pending_af_range_.store(range);
}
void DmaCamera::set_af_speed(int speed) {
    if (!camera_) return;
    cur_af_speed_.store(speed); pending_af_speed_.store(speed);
}
void DmaCamera::set_analogue_gain(float gain) {
    if (!camera_) return;
    cur_gain_.store(gain); pending_gain_.store(gain);
}
void DmaCamera::set_ae_metering(int mode) {
    if (!camera_) return;
    cur_ae_metering_.store(mode); pending_ae_metering_.store(mode);
}
void DmaCamera::set_ae_constraint(int mode) {
    if (!camera_) return;
    cur_ae_constraint_.store(mode); pending_ae_constraint_.store(mode);
}
void DmaCamera::set_ae_exposure_mode(int mode) {
    if (!camera_) return;
    cur_ae_exp_mode_.store(mode); pending_ae_exp_mode_.store(mode);
}
void DmaCamera::set_flicker_mode(int mode) {
    if (!camera_) return;
    cur_flicker_.store(mode); pending_flicker_.store(mode);
}
void DmaCamera::set_awb_mode(int mode) {
    if (!camera_) return;
    cur_awb_mode_.store(mode); pending_awb_mode_.store(mode);
}
void DmaCamera::set_brightness(float v) {
    if (!camera_) return;
    cur_brightness_.store(v); pending_brightness_.store(v);
}
void DmaCamera::set_contrast(float v) {
    if (!camera_) return;
    cur_contrast_.store(v); pending_contrast_.store(v);
}
void DmaCamera::set_saturation(float v) {
    if (!camera_) return;
    cur_saturation_.store(v); pending_saturation_.store(v);
}
void DmaCamera::set_sharpness(float v) {
    if (!camera_) return;
    cur_sharpness_.store(v); pending_sharpness_.store(v);
}
void DmaCamera::set_noise_reduction(int mode) {
    if (!camera_) return;
    cur_nr_.store(mode); pending_nr_.store(mode);
}
void DmaCamera::set_hdr_mode(int mode) {
    if (!camera_) return;
    cur_hdr_.store(mode); pending_hdr_.store(mode);
}

// ── Full-resolution still grab (CPU NV12 → JPEG) ──────────────────────────────
bool DmaCamera::grab_still(const std::string& path) {
    if (fmt_drm_ != DRM_FORMAT_NV12) {
        std::cerr << "[dma] grab_still: only NV12 supported\n";
        return false;
    }
    // Latest complete frame buffer (held briefly under the handoff lock).
    FrameBuffer* buf = nullptr;
    {
        std::lock_guard<std::mutex> lk(handoff_mtx_);
        int s = (ready_slot_ >= 0) ? ready_slot_ : render_slot_;
        if (s >= 0 && s < static_cast<int>(slots_.size())) buf = slots_[s].buffer;
    }
    if (!buf) { std::cerr << "[dma] grab_still: no frame ready\n"; return false; }

    const int w = cfg_.width, h = cfg_.height, stride = stride_;
    if (w <= 0 || h <= 0 || stride < w) return false;

    const auto& planes = buf->planes();
    if (planes.empty()) return false;

    const int   fd0  = planes[0].fd.get();
    const size_t len0 = static_cast<size_t>(planes[0].offset) + planes[0].length;
    void* base0 = ::mmap(nullptr, len0, PROT_READ, MAP_SHARED, fd0, 0);
    if (base0 == MAP_FAILED) { std::cerr << "[dma] grab_still: mmap Y failed\n"; return false; }

    const uint8_t* y_ptr = static_cast<const uint8_t*>(base0) + planes[0].offset;
    const uint8_t* uv_ptr = nullptr;
    void*  base1 = nullptr;  size_t len1 = 0;
    if (planes.size() >= 2) {
        const int fd1 = planes[1].fd.get();
        if (fd1 == fd0) {
            uv_ptr = static_cast<const uint8_t*>(base0) + planes[1].offset;
        } else {
            len1  = static_cast<size_t>(planes[1].offset) + planes[1].length;
            base1 = ::mmap(nullptr, len1, PROT_READ, MAP_SHARED, fd1, 0);
            if (base1 == MAP_FAILED) { ::munmap(base0, len0); return false; }
            uv_ptr = static_cast<const uint8_t*>(base1) + planes[1].offset;
        }
    } else {
        // Single-plane NV12: UV directly follows Y.
        uv_ptr = y_ptr + static_cast<size_t>(stride) * h;
    }

    bool ok = false;
    try {
        // Repack into a tightly-packed NV12 (drops row stride padding) so OpenCV
        // can convert it: h rows of Y then h/2 rows of interleaved UV, step = w.
        cv::Mat nv12(h + h / 2, w, CV_8UC1);
        for (int r = 0; r < h; ++r)
            std::memcpy(nv12.ptr(r), y_ptr + static_cast<size_t>(r) * stride, w);
        for (int r = 0; r < h / 2; ++r)
            std::memcpy(nv12.ptr(h + r), uv_ptr + static_cast<size_t>(r) * stride, w);

        cv::Mat bgr;
        cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
        ok = cv::imwrite(path, bgr, { cv::IMWRITE_JPEG_QUALITY, 92 });
    } catch (const std::exception& e) {
        std::cerr << "[dma] grab_still: " << e.what() << "\n";
    }

    ::munmap(base0, len0);
    if (base1) ::munmap(base1, len1);
    if (ok) std::cout << "[dma] grab_still: saved " << w << "×" << h << " → " << path << "\n";
    return ok;
}

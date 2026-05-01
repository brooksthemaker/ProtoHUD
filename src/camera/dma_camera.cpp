#include "dma_camera.h"
#include "../gl_utils.h"

#include <libcamera/libcamera.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <iostream>
#include <cstring>
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

    // ── Select camera: by model string if provided, else by index ────────────
    if (!cfg_.model_name.empty()) {
        for (auto& desc : cameras) {
            auto c = lcam_mgr_->get(desc->id());
            try {
                auto m = c->properties().get(properties::Model);
                if (m && *m == cfg_.model_name) {
                    camera_ = c;
                    break;
                }
            } catch (...) {}
        }
        if (!camera_)
            std::cerr << "[dma] model '" << cfg_.model_name
                      << "' not found, falling back to id " << cfg_.libcamera_id << "\n";
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
        std::cerr << "[dma] failed to acquire camera\n";
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

    std::cout << "[dma] camera " << cfg_.libcamera_id
              << " configured: " << cfg_.width << "×" << cfg_.height
              << " stride=" << stride_ << " fps=" << cfg_.fps << "\n";
    return true;
}

// ── Buffer allocation + EGLImage creation ─────────────────────────────────────

bool DmaCamera::allocate_buffers_and_egl() {
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

        // Create libcamera Request (one per slot)
        slots_[i].request = camera_->createRequest().release();
        if (!slots_[i].request) {
            std::cerr << "[dma] createRequest failed for slot " << i << "\n";
            return false;
        }
        slots_[i].request->addBuffer(stream_, buf);
        req_to_slot_[slots_[i].request] = i;
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

    // Cache the sampler uniform location (uniform name "tex" in nv12.fs)
    loc_tex_y_ = glGetUniformLocation(nv12_prog_, "tex");

    glUseProgram(nv12_prog_);
    if (loc_tex_y_ >= 0) glUniform1i(loc_tex_y_, 0);   // GL_TEXTURE0
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
    // valid for the lifetime of the capture session.
    while (running_)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

bool DmaCamera::draw() {
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

    gl::bind_quad(quad_vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glUseProgram(0);

    // Unbind the external OES texture (leave texture unit 0 active, unbound)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    return true;
}

// ── shutdown ──────────────────────────────────────────────────────────────────

void DmaCamera::shutdown() {
    // Guard against double-shutdown, but always release camera_ if it was acquired
    // (camera_ is set in configure_camera() before EGL work, so it must be
    // cleaned up even when ok_ is false due to a later EGL failure).
    if (!ok_ && !running_ && !camera_) return;

    ok_      = false;
    running_ = false;
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
        // Requests are owned by the camera; don't delete them
    }

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
    running_ = false;
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

    // 3. Free libcamera buffer allocations
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

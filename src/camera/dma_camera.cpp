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

// DRM fourcc codes — avoids depending on drm/drm_fourcc.h
#define DRM_FORMAT_R8   0x20203852u   // 'R','8',' ',' '
#define DRM_FORMAT_RG88 0x38384752u   // 'R','G','8','8'

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
    if (static_cast<int>(cameras.size()) <= cfg_.libcamera_id) {
        std::cerr << "[dma] camera id " << cfg_.libcamera_id
                  << " not found (" << cameras.size() << " cameras)\n";
        return false;
    }

    camera_ = lcam_mgr_->get(cameras[cfg_.libcamera_id]->id());
    if (!camera_ || camera_->acquire()) {
        std::cerr << "[dma] failed to acquire camera " << cfg_.libcamera_id << "\n";
        return false;
    }

    cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
    auto& sc = cam_cfg_->at(0);
    sc.pixelFormat = formats::NV12;   // native ISP output — no ISP RGB conversion
    sc.size        = { static_cast<unsigned>(cfg_.width),
                       static_cast<unsigned>(cfg_.height) };
    sc.bufferCount = NUM_SLOTS;
    // Frame rate is set via FrameDurationLimits control on camera_->start(),
    // not via StreamConfiguration (frameRate field does not exist on Bullseye).

    auto status = cam_cfg_->validate();
    if (status == CameraConfiguration::Invalid) {
        std::cerr << "[dma] camera configuration invalid\n";
        return false;
    }
    if (status == CameraConfiguration::Adjusted) {
        // libcamera snapped the requested size to the nearest supported mode.
        // Update cfg_ so EGL image dimensions match the actual buffer layout.
        std::cout << "[dma] camera " << cfg_.libcamera_id
                  << " size adjusted by libcamera: "
                  << cfg_.width << "×" << cfg_.height
                  << " → " << sc.size.width << "×" << sc.size.height << "\n";
        cfg_.width  = sc.size.width;
        cfg_.height = sc.size.height;
    }
    if (camera_->configure(cam_cfg_.get())) {
        std::cerr << "[dma] camera_->configure() failed\n";
        return false;
    }

    stream_ = sc.stream();
    stride_ = sc.stride;

    std::cout << "[dma] camera " << cfg_.libcamera_id
              << " configured: " << cfg_.width << "×" << cfg_.height
              << " NV12 stride=" << stride_ << " fps=" << cfg_.fps << "\n";
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
    if (planes.size() < 2) {
        std::cerr << "[dma] NV12 buffer has fewer than 2 planes\n";
        return false;
    }

    int fd_y    = planes[0].fd.get();
    int fd_uv   = planes[1].fd.get();
    auto off_y  = static_cast<EGLint>(planes[0].offset);
    auto off_uv = static_cast<EGLint>(planes[1].offset);
    auto pitch  = static_cast<EGLint>(stride_);

    // Y plane — full resolution, single channel (R8)
    {
        EGLint attr[] = {
            EGL_WIDTH,                        cfg_.width,
            EGL_HEIGHT,                       cfg_.height,
            EGL_LINUX_DRM_FOURCC_EXT,         (EGLint)DRM_FORMAT_R8,
            EGL_DMA_BUF_PLANE0_FD_EXT,        fd_y,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,    off_y,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,     pitch,
            EGL_NONE
        };
        slot.img_y = pfn_create_image_(egl_display_, EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
        if (slot.img_y == EGL_NO_IMAGE_KHR) {
            std::cerr << "[dma] eglCreateImageKHR for Y plane failed (err 0x"
                      << std::hex << eglGetError() << std::dec << ")\n";
            return false;
        }
    }

    // UV plane — half resolution, dual channel (RG88 = Cb in R, Cr in G)
    {
        EGLint attr[] = {
            EGL_WIDTH,                        cfg_.width  / 2,
            EGL_HEIGHT,                       cfg_.height / 2,
            EGL_LINUX_DRM_FOURCC_EXT,         (EGLint)DRM_FORMAT_RG88,
            EGL_DMA_BUF_PLANE0_FD_EXT,        fd_uv,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,    off_uv,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,     pitch,
            EGL_NONE
        };
        slot.img_uv = pfn_create_image_(egl_display_, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
        if (slot.img_uv == EGL_NO_IMAGE_KHR) {
            std::cerr << "[dma] eglCreateImageKHR for UV plane failed (err 0x"
                      << std::hex << eglGetError() << std::dec << ")\n";
            pfn_destroy_image_(egl_display_, slot.img_y);
            slot.img_y = EGL_NO_IMAGE_KHR;
            return false;
        }
    }
    return true;
}

// ── GL resources (textures + shader + quad VBO) ───────────────────────────────

bool DmaCamera::create_gl_resources(const char* vs_path, const char* fs_path) {
    // Y and UV textures — backing store is set per-frame via EGLImageTargetTexture2DOES
    glGenTextures(1, &tex_y_);
    glGenTextures(1, &tex_uv_);

    auto setup_tex = [](GLuint id) {
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    setup_tex(tex_y_);
    setup_tex(tex_uv_);
    glBindTexture(GL_TEXTURE_2D, 0);

    // NV12 → RGB shader
    nv12_prog_ = gl::build_program(vs_path, fs_path);
    if (!nv12_prog_) {
        std::cerr << "[dma] NV12 shader failed to load\n";
        return false;
    }

    // Cache uniform locations
    loc_tex_y_  = glGetUniformLocation(nv12_prog_, "tex_y");
    loc_tex_uv_ = glGetUniformLocation(nv12_prog_, "tex_uv");

    // Tell the shader which texture units to sample
    glUseProgram(nv12_prog_);
    if (loc_tex_y_  >= 0) glUniform1i(loc_tex_y_,  0);  // GL_TEXTURE0
    if (loc_tex_uv_ >= 0) glUniform1i(loc_tex_uv_, 1);  // GL_TEXTURE1
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

    // ── Bind EGLImages to GL textures ────────────────────────────────────────
    // Re-binding each frame is lightweight (just updates the texture backing pointer).

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    pfn_img_target_tex_(GL_TEXTURE_2D, slots_[slot].img_y);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    pfn_img_target_tex_(GL_TEXTURE_2D, slots_[slot].img_uv);

    // ── Draw fullscreen NV12 → RGB quad ──────────────────────────────────────

    glUseProgram(nv12_prog_);

    gl::bind_quad(quad_vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glUseProgram(0);

    // Restore texture unit 0 as active (convention for rest of pipeline)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

// ── shutdown ──────────────────────────────────────────────────────────────────

void DmaCamera::shutdown() {
    if (!ok_ && !running_) return;
    ok_ = false;

    running_ = false;
    if (event_thread_.joinable()) event_thread_.join();

    if (camera_) {
        camera_->stop();
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

    if (tex_y_)     { glDeleteTextures(1, &tex_y_);     tex_y_     = 0; }
    if (tex_uv_)    { glDeleteTextures(1, &tex_uv_);    tex_uv_    = 0; }
    if (nv12_prog_) { glDeleteProgram(nv12_prog_);       nv12_prog_ = 0; }
    if (quad_vbo_)  { glDeleteBuffers(1, &quad_vbo_);   quad_vbo_  = 0; }

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

    // 5. Reconfigure the libcamera stream (camera stays acquired)
    cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
    auto& sc = cam_cfg_->at(0);
    sc.pixelFormat = formats::NV12;
    sc.size        = { static_cast<unsigned>(cfg_.width),
                       static_cast<unsigned>(cfg_.height) };
    sc.bufferCount = NUM_SLOTS;
    // frameRate is set via FrameDurationLimits in start_capture(), not here.

    auto rcfg_status = cam_cfg_->validate();
    if (rcfg_status == CameraConfiguration::Adjusted) {
        std::cout << "[dma] reconfigure: size adjusted "
                  << cfg_.width << "×" << cfg_.height
                  << " → " << sc.size.width << "×" << sc.size.height << "\n";
        cfg_.width  = sc.size.width;
        cfg_.height = sc.size.height;
    }
    if (rcfg_status == CameraConfiguration::Invalid) {
        std::cerr << "[dma] reconfigure: invalid configuration "
                  << width << "×" << height << " @" << fps << "fps — reverting\n";
        cfg_.width = old_w;  cfg_.height = old_h;  cfg_.fps = old_fps;
        cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        auto& sc2 = cam_cfg_->at(0);
        sc2.pixelFormat = formats::NV12;
        sc2.size = { static_cast<unsigned>(old_w), static_cast<unsigned>(old_h) };
        sc2.bufferCount = NUM_SLOTS;
        cam_cfg_->validate();
        camera_->configure(cam_cfg_.get());
        stream_ = cam_cfg_->at(0).stream();
        stride_ = cam_cfg_->at(0).stride;
        if (allocate_buffers_and_egl()) start_capture();
        return false;
    }

    if (camera_->configure(cam_cfg_.get())) {
        std::cerr << "[dma] reconfigure: camera_->configure() failed — reverting\n";
        cfg_.width = old_w;  cfg_.height = old_h;  cfg_.fps = old_fps;
        // best-effort revert
        cam_cfg_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        auto& sc2 = cam_cfg_->at(0);
        sc2.pixelFormat = formats::NV12;
        sc2.size = { static_cast<unsigned>(old_w), static_cast<unsigned>(old_h) };
        sc2.bufferCount = NUM_SLOTS;
        cam_cfg_->validate();
        camera_->configure(cam_cfg_.get());
        stream_ = cam_cfg_->at(0).stream();
        stride_ = cam_cfg_->at(0).stride;
        if (allocate_buffers_and_egl()) start_capture();
        return false;
    }

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

    float ev = pending_ev_.exchange(-9999.0f);
    if (ev > -9998.0f)
        ctrls.set(controls::ExposureValue, ev);

    int sh = pending_shutter_us_.exchange(-1);
    if (sh > 0)
        ctrls.set(controls::ExposureTime, sh);

    float lp = pending_lens_pos_.exchange(-1.0f);
    if (lp >= 0.0f)
        ctrls.set(controls::LensPosition, lp);
}

void DmaCamera::start_autofocus() {
    if (!camera_) return;
    if (camera_->controls().count(&controls::AfMode))
        pending_af_mode_.store(controls::AfModeAuto);
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

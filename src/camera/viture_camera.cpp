#include "viture_camera.h"

#include <viture_camera_provider.h>
#include <viture_result.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <GLES2/gl2.h>
#include <iostream>
#include <cstring>

VitureCamera::VitureCamera()  = default;
VitureCamera::~VitureCamera() { stop(); }

bool VitureCamera::start(int glasses_product_id) {
    int cam_vid = xr_camera_provider_get_camera_vid(glasses_product_id);
    int cam_pid = xr_camera_provider_get_camera_pid(glasses_product_id);

    if (cam_vid == 0 || cam_pid == 0) {
        std::cerr << "[vcam] glasses product 0x" << std::hex << glasses_product_id
                  << std::dec << " has no built-in camera\n";
        return false;
    }

    std::cout << "[vcam] camera VID=0x" << std::hex << cam_vid
              << " PID=0x" << cam_pid << std::dec << "\n";

    handle_ = xr_camera_provider_create(cam_vid, cam_pid);
    if (!handle_) {
        std::cerr << "[vcam] xr_camera_provider_create failed\n";
        return false;
    }

    // The SDK delivers XRCameraFrame* on a dedicated thread; forward to our slot.
    int rc = xr_camera_provider_start(
        handle_,
        [](const XRCameraFrame* frame, void* ud) {
            reinterpret_cast<VitureCamera*>(ud)->on_frame(frame);
        },
        this
    );

    if (rc != VITURE_GLASSES_SUCCESS) {
        std::cerr << "[vcam] xr_camera_provider_start failed: " << rc << "\n";
        xr_camera_provider_destroy(handle_);
        handle_ = nullptr;
        return false;
    }

    running_ = true;
    return true;
}

void VitureCamera::stop() {
    running_ = false;
    if (handle_) {
        xr_camera_provider_stop(handle_);
        xr_camera_provider_destroy(handle_);
        handle_ = nullptr;
    }
}

bool VitureCamera::is_streaming() const {
    return handle_ && xr_camera_provider_is_streaming(handle_);
}

// ── Frame callback (SDK thread) ───────────────────────────────────────────────

void VitureCamera::on_frame(const void* raw) {
    if (!raw || !running_) return;
    const XRCameraFrame* f = reinterpret_cast<const XRCameraFrame*>(raw);

    // The Beast camera delivers MJPEG — decode via OpenCV
    if (f->format == XR_CAMERA_FORMAT_MJPEG) {
        std::vector<uint8_t> jpeg(f->data, f->data + f->size);
        cv::Mat decoded = cv::imdecode(jpeg, cv::IMREAD_COLOR);
        if (decoded.empty()) return;

        cv::Mat rgba;
        cv::cvtColor(decoded, rgba, cv::COLOR_BGR2RGBA);

        std::lock_guard<std::mutex> lk(slot_.mtx);
        slot_.w = rgba.cols;
        slot_.h = rgba.rows;
        slot_.buf.resize(static_cast<size_t>(rgba.total()) * 4);
        std::memcpy(slot_.buf.data(), rgba.data, slot_.buf.size());
        slot_.dirty = true;
        frame_w_ = rgba.cols;
        frame_h_ = rgba.rows;

    } else if (f->format == XR_CAMERA_FORMAT_RGB) {
        // Direct RGB→RGBA conversion
        std::lock_guard<std::mutex> lk(slot_.mtx);
        slot_.w = f->width;
        slot_.h = f->height;
        slot_.buf.resize(static_cast<size_t>(f->width) * f->height * 4);
        for (uint32_t i = 0; i < f->width * f->height; i++) {
            slot_.buf[i*4+0] = f->data[i*3+0];
            slot_.buf[i*4+1] = f->data[i*3+1];
            slot_.buf[i*4+2] = f->data[i*3+2];
            slot_.buf[i*4+3] = 255;
        }
        slot_.dirty = true;
        frame_w_ = f->width;
        frame_h_ = f->height;
    }
}

// ── Texture upload (render thread) ────────────────────────────────────────────

bool VitureCamera::get_frame(GLuint& out) {
    std::lock_guard<std::mutex> lk(slot_.mtx);
    if (!slot_.dirty || slot_.buf.empty()) { out = 0; return false; }

    if (out == 0) {
        // Allocate a new GL texture
        glGenTextures(1, &out);
        glBindTexture(GL_TEXTURE_2D, out);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex_w_ = 0;
        tex_h_ = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, out);
    }

    if (tex_w_ != slot_.w || tex_h_ != slot_.h) {
        // Allocate (or reallocate) texture storage
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     slot_.w, slot_.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, slot_.buf.data());
        tex_w_ = slot_.w;
        tex_h_ = slot_.h;
    } else {
        // Fast path: dimensions unchanged, just update pixels
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        slot_.w, slot_.h,
                        GL_RGBA, GL_UNSIGNED_BYTE, slot_.buf.data());
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    slot_.dirty = false;
    return true;
}

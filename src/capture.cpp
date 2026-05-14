// stb_image_write: single-header PNG/JPEG encoder.
// stb_image.h is already defined in nanovg_gl_impl.cpp (via STB_IMAGE_IMPLEMENTATION),
// but stb_image_write is independent — define its implementation here only.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "capture.h"
#include "app_state.h"
#include "gl_utils.h"
#include "vitrue/xr_display.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Read the entire contents of an FBO into a top-down RGBA pixel buffer.
// Must be called on the render (GL) thread.
static std::vector<uint8_t> read_fbo_pixels(gl::Fbo& fbo) {
    const int w = fbo.w, h = fbo.h;
    std::vector<uint8_t> px(static_cast<size_t>(w * h * 4));
    fbo.bind();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    fbo.unbind();
    // OpenGL memory is bottom-up; flip rows to get top-down for PNG
    for (int row = 0; row < h / 2; ++row)
        std::swap_ranges(px.begin() + row * w * 4,
                         px.begin() + (row + 1) * w * 4,
                         px.begin() + (h - 1 - row) * w * 4);
    return px;
}

static std::string build_path(const std::string& dir, const char* tag) {
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tm_buf {};
    localtime_r(&tt, &tm_buf);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);
    return dir + "/protohud_" + ts + "_" + tag + ".png";
}

static void async_write_png(std::vector<uint8_t> px, std::string path, int w, int h) {
    std::thread([px = std::move(px), path = std::move(path), w, h]() {
        stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
    }).detach();
}

static void push_toast(AppState& state, std::string title, std::string body) {
    Notification n;
    n.type          = NotifType::App;
    n.title         = std::move(title);
    n.body          = std::move(body);
    n.auto_dismiss_s = 6.f;
    state.notifs.push(std::move(n));
}

void do_capture(CaptureRequest req, XRDisplay& xr,
                const std::string& dir, AppState& state) {
    fs::create_directories(dir);

    if (req == CaptureRequest::Left) {
        const int w = xr.eye_left().w, h = xr.eye_left().h;
        auto px   = read_fbo_pixels(xr.eye_left());
        auto path = build_path(dir, "left");
        async_write_png(std::move(px), path, w, h);
        std::lock_guard lk(state.mtx);
        push_toast(state, "Photo saved", path);
        state.capture_request = CaptureRequest::None;

    } else if (req == CaptureRequest::Right) {
        const int w = xr.eye_right().w, h = xr.eye_right().h;
        auto px   = read_fbo_pixels(xr.eye_right());
        auto path = build_path(dir, "right");
        async_write_png(std::move(px), path, w, h);
        std::lock_guard lk(state.mtx);
        push_toast(state, "Photo saved", path);
        state.capture_request = CaptureRequest::None;

    } else if (req == CaptureRequest::Stereo) {
        const int w = xr.eye_left().w, h = xr.eye_left().h;
        auto L = read_fbo_pixels(xr.eye_left());
        auto R = read_fbo_pixels(xr.eye_right());
        // Interleave rows: left half then right half per scanline
        std::vector<uint8_t> sbs(static_cast<size_t>(2 * w * h * 4));
        for (int row = 0; row < h; ++row) {
            const uint8_t* l = L.data() + row * w * 4;
            const uint8_t* r = R.data() + row * w * 4;
            uint8_t*      dst = sbs.data() + row * 2 * w * 4;
            std::copy(l, l + w * 4, dst);
            std::copy(r, r + w * 4, dst + w * 4);
        }
        auto path = build_path(dir, "stereo");
        async_write_png(std::move(sbs), path, 2 * w, h);
        std::lock_guard lk(state.mtx);
        push_toast(state, "Stereo photo saved", path);
        state.capture_request = CaptureRequest::None;
    }
}

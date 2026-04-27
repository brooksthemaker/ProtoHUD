#include "xr_display.h"

#include <viture_glasses_provider.h>
#include <viture_protocol_public.h>
#include <viture_device.h>
#include <viture_result.h>

#include <GLES2/gl2.h>
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>

// ── Singleton trampoline ──────────────────────────────────────────────────────
XRDisplay* XRDisplay::s_instance_ = nullptr;

// ── USB scan ──────────────────────────────────────────────────────────────────
static int scan_usb_for_viture() {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return 0;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        std::string base = std::string("/sys/bus/usb/devices/") + e->d_name;
        std::ifstream f(base + "/idProduct");
        if (!f) continue;
        std::string pid_str; f >> pid_str;
        if (pid_str.empty()) continue;
        int pid = 0;
        try { pid = std::stoi(pid_str, nullptr, 16); } catch (...) { continue; }
        if (xr_device_provider_is_product_id_valid(pid)) { closedir(dir); return pid; }
    }
    closedir(dir);
    return 0;
}

static uint8_t fps_to_display_mode(int fps) {
    switch (fps) {
        case 60:  return VITURE_NATIVE_DISPLAY_MODE_3D_SBS_3840_1080_60HZ;
        case 120: return VITURE_NATIVE_DISPLAY_MODE_3D_SBS_3840_1080_120HZ;
        default:  return VITURE_NATIVE_DISPLAY_MODE_3D_SBS_3840_1080_90HZ;
    }
}

// ── SDK callbacks ─────────────────────────────────────────────────────────────
void XRDisplay::s_state_cb(int id, int val) {
    if (!s_instance_) return;
    std::lock_guard<std::mutex> lk(s_instance_->cb_mtx_);
    if (s_instance_->state_cb_) s_instance_->state_cb_(id, val);
}
void XRDisplay::s_imu_cb(float* data, uint64_t) {
    if (!s_instance_ || !data) return;
    float roll = data[0], pitch = data[1], yaw = data[2];
    s_instance_->imu_roll_.store(roll);
    s_instance_->imu_pitch_.store(pitch);
    s_instance_->imu_yaw_.store(yaw);
    std::lock_guard<std::mutex> lk(s_instance_->cb_mtx_);
    if (s_instance_->imu_cb_) s_instance_->imu_cb_(roll, pitch, yaw);
}

// ── Construction ──────────────────────────────────────────────────────────────
XRDisplay::XRDisplay(const XRConfig& cfg) : cfg_(cfg) { s_instance_ = this; }
XRDisplay::~XRDisplay() { shutdown(); if (s_instance_ == this) s_instance_ = nullptr; }

// ── init ──────────────────────────────────────────────────────────────────────
bool XRDisplay::init() {
    if (!glfwInit()) { std::cerr << "[xr] glfwInit failed\n"; return false; }

    bool found = find_and_connect();
    if (!found) std::cerr << "[xr] VITURE glasses not found — desktop fallback\n";

    // Resolve display dimensions from the chosen monitor's current video mode
    // so the window always matches the physical output without hardcoding.
    GLFWmonitor* mon = choose_monitor();
    if (mon) {
        const GLFWvidmode* vm = glfwGetVideoMode(mon);
        disp_w_ = vm->width;
        disp_h_ = vm->height;
    } else {
        // Primary monitor / windowed fallback
        const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
        disp_w_ = vm ? vm->width  : 1920;
        disp_h_ = vm ? vm->height : 1080;
    }
    // Each eye is half the display width (SBS layout)
    eye_w_ = found ? disp_w_ / 2 : disp_w_;
    eye_h_ = disp_h_;
    std::cerr << "[xr] display " << disp_w_ << "x" << disp_h_
              << "  eye " << eye_w_ << "x" << eye_h_ << "\n";

    // GLES 2.0 context via EGL
    glfwWindowHint(GLFW_CLIENT_API,          GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_DOUBLEBUFFER,          GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED,  cfg_.frameless ? GLFW_FALSE : GLFW_TRUE);

    mon = choose_monitor();
    window_ = glfwCreateWindow(disp_w_, disp_h_, "ProtoHUD", mon, nullptr);
    if (!window_) {
        std::cerr << "[xr] glfwCreateWindow failed\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // vsync

    // Build blit program and quad VBO (used in submit_frame)
    if (!create_blit_program()) return false;
    quad_vbo_ = gl::make_quad_vbo();

    // Per-eye FBOs
    rt_left_  = gl::make_fbo(eye_w_, eye_h_);
    rt_right_ = gl::make_fbo(eye_w_, eye_h_);
    if (!rt_left_.valid() || !rt_right_.valid()) {
        std::cerr << "[xr] failed to allocate eye FBOs\n";
        return false;
    }

    return true;
}

// ── Blit program ──────────────────────────────────────────────────────────────
// Blits a texture into a sub-rectangle of the current framebuffer (NDC).
bool XRDisplay::create_blit_program() {
    static const char* vs = R"(
        attribute vec2 a_pos;
        attribute vec2 a_uv;
        uniform vec4   u_rect;   // NDC rect: x0,y0,x1,y1
        varying vec2   v_uv;
        void main() {
            // remap [-1,1] → [x0,x1] / [y0,y1]
            float x = u_rect.x + (a_pos.x * 0.5 + 0.5) * (u_rect.z - u_rect.x);
            float y = u_rect.y + (a_pos.y * 0.5 + 0.5) * (u_rect.w - u_rect.y);
            gl_Position = vec4(x, y, 0.0, 1.0);
            v_uv = a_uv;
        })";

    static const char* fs = R"(
        precision mediump float;
        uniform sampler2D u_tex;
        varying vec2      v_uv;
        void main() {
            gl_FragColor = texture2D(u_tex, v_uv);
        })";

    GLuint vs_s = gl::compile_shader(GL_VERTEX_SHADER,   vs);
    GLuint fs_s = gl::compile_shader(GL_FRAGMENT_SHADER, fs);
    blit_prog_  = gl::link_program(vs_s, fs_s);
    return blit_prog_ != 0;
}

// ── composite ─────────────────────────────────────────────────────────────────
// Blit eye FBOs to the default framebuffer (SBS or desktop). Does NOT swap.

void XRDisplay::composite() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, disp_w_, disp_h_);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(blit_prog_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(blit_prog_, "u_tex"), 0);

    gl::bind_quad(quad_vbo_);

    // SBS layout: left eye → left half, right eye → right half.
    // When glasses are connected (device_) the window is 3840×1080 and each eye
    // fills 1920×1080 naturally.  In desktop fallback the window is 1920×1080 and
    // both cameras are squeezed into 960×1080 each — useful for verifying that
    // both camera feeds are alive without the glasses attached.
    glBindTexture(GL_TEXTURE_2D, rt_left_.tex);
    glUniform4f(glGetUniformLocation(blit_prog_, "u_rect"), -1.f, -1.f, 0.f, 1.f);
    gl::draw_quad();

    glBindTexture(GL_TEXTURE_2D, rt_right_.tex);
    glUniform4f(glGetUniformLocation(blit_prog_, "u_rect"),  0.f, -1.f, 1.f, 1.f);
    gl::draw_quad();

    gl::unbind_quad();
    glUseProgram(0);
}

// ── present ───────────────────────────────────────────────────────────────────
// Swap the GLFW window buffer and poll events.

void XRDisplay::present() {
    glfwSwapBuffers(window_);
    glfwPollEvents();
}

// ── shutdown ──────────────────────────────────────────────────────────────────
void XRDisplay::shutdown() {
    rt_left_.destroy();
    rt_right_.destroy();
    if (blit_prog_) { glDeleteProgram(blit_prog_); blit_prog_ = 0; }
    if (quad_vbo_)  { glDeleteBuffers(1, &quad_vbo_); quad_vbo_ = 0; }

    if (device_) {
        if (cfg_.enable_imu)
            xr_device_provider_close_imu(device_, VITURE_IMU_MODE_POSE);
        xr_device_provider_stop    (device_);
        xr_device_provider_shutdown(device_);
        xr_device_provider_destroy (device_);
        device_ = nullptr;
    }

    if (window_) { glfwDestroyWindow(window_); window_ = nullptr; }
    glfwTerminate();
}

// ── VITURE SDK init ───────────────────────────────────────────────────────────
bool XRDisplay::find_and_connect() {
    product_id_ = (cfg_.product_id != 0) ? cfg_.product_id : scan_usb_for_viture();
    if (product_id_ == 0) return false;

    char market[64] = {}; int mlen = sizeof(market);
    xr_device_provider_get_market_name(product_id_, market, &mlen);
    std::cout << "[xr] Found VITURE " << market
              << " (PID 0x" << std::hex << product_id_ << std::dec << ")\n";

    xr_device_provider_set_log_level(LOG_LEVEL_ERROR);
    device_ = xr_device_provider_create(product_id_);
    if (!device_) { std::cerr << "[xr] create failed\n"; return false; }

    if (xr_device_provider_initialize(device_, nullptr, nullptr) != VITURE_GLASSES_SUCCESS ||
        xr_device_provider_start(device_)                        != VITURE_GLASSES_SUCCESS) {
        xr_device_provider_destroy(device_); device_ = nullptr; return false;
    }

    xr_device_provider_register_state_callback(device_, s_state_cb);
    set_sbs_display_mode();
    if (cfg_.enable_imu) open_imu();
    return true;
}

void XRDisplay::set_sbs_display_mode() {
    if (!device_) return;
    xr_device_provider_native_set_display_mode(device_, fps_to_display_mode(cfg_.target_fps));
    xr_device_provider_native_switch_dimension(device_, 1);
}

void XRDisplay::open_imu() {
    if (!device_) return;
    xr_device_provider_register_imu_pose_callback(device_, s_imu_cb);
    xr_device_provider_open_imu(device_, VITURE_IMU_MODE_POSE,
                                 VITURE_IMU_FREQUENCY_MEDIUM_HIGH);
}

GLFWmonitor* XRDisplay::choose_monitor() const {
    if (cfg_.monitor_index >= 0) {
        int count; GLFWmonitor** mons = glfwGetMonitors(&count);
        if (cfg_.monitor_index < count) return mons[cfg_.monitor_index];
    }
    // Auto: prefer monitor whose mode width is >= 3840 (the SBS output)
    int count; GLFWmonitor** mons = glfwGetMonitors(&count);
    for (int i = 0; i < count; i++) {
        const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
        if (vm && vm->width >= 3840) return mons[i];
    }
    // Fallback: non-primary monitor (glasses are usually secondary)
    return (count > 1) ? mons[1] : nullptr;  // nullptr = windowed on primary
}

// ── Device control ────────────────────────────────────────────────────────────
void XRDisplay::set_brightness(int l)  { if (device_) xr_device_provider_set_brightness_level(device_, l); }
void XRDisplay::set_3d_mode(bool b)    { if (device_) xr_device_provider_native_switch_dimension(device_, b ? 1 : 0); }
// Electrochromic film (lens tint): SDK takes float 0.0–1.0; map from level 0–9
void XRDisplay::set_dimming(int l)     { if (device_) xr_device_provider_set_film_mode(device_, std::max(0.0f, std::min(1.0f, l / 9.0f))); }
void XRDisplay::set_hud_brightness(int l) { if (device_) xr_device_provider_set_brightness_level(device_, std::max(1, std::min(9, l))); }
// Native DOF recenter (Gen2/Beast); no-op on Gen1 devices
void XRDisplay::recenter_tracking()    { if (device_) xr_device_provider_native_recenter_dof(device_); }
void XRDisplay::toggle_gaze_lock()     { /* gaze lock not available in current VITURE SDK */ }

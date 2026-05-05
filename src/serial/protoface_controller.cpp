#include "protoface_controller.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string esc(const std::string& s) {
    // Minimal JSON string escaping (no control chars expected in our values).
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// ── Construction ──────────────────────────────────────────────────────────────

ProtoFaceController::ProtoFaceController(const std::string& path)
    : socket_path_(path) {}

ProtoFaceController::~ProtoFaceController() {
    stop();
}

// ── IFaceController ───────────────────────────────────────────────────────────

bool ProtoFaceController::start() {
    if (running_) return true;
    running_ = true;
    try_connect();
    shm_.open();   // best-effort; Protoface may not be running yet
    reconnect_thread_ = std::thread(&ProtoFaceController::reconnect_loop, this);
    return true;
}

void ProtoFaceController::stop() {
    running_ = false;
    if (reconnect_thread_.joinable()) reconnect_thread_.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        connected_ = false;
    }
    shm_.close();
    if (panel_tex_ != 0) { glDeleteTextures(1, &panel_tex_); panel_tex_ = 0; }
}

bool ProtoFaceController::connected() const {
    return connected_.load();
}

void ProtoFaceController::set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        R"({"cmd":"set_color","r":%u,"g":%u,"b":%u,"layer":%u})",
        r, g, b, layer);
    send(buf);
}

void ProtoFaceController::set_effect(uint8_t effect_id, uint8_t p1, uint8_t p2) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        R"({"cmd":"set_effect","effect_id":%u,"p1":%u,"p2":%u})",
        effect_id, p1, p2);
    send(buf);
}

void ProtoFaceController::play_gif(uint8_t gif_id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), R"({"cmd":"play_gif","gif_id":%u})", gif_id);
    send(buf);
}

void ProtoFaceController::set_brightness(uint8_t value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), R"({"cmd":"set_brightness","value":%u})", value);
    send(buf);
}

void ProtoFaceController::set_palette(uint8_t palette_id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), R"({"cmd":"set_palette","palette_id":%u})", palette_id);
    send(buf);
}

void ProtoFaceController::set_menu_item(uint8_t menu_index, uint8_t value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        R"({"cmd":"set_menu_item","menu_index":%u,"value":%u})",
        menu_index, value);
    send(buf);
}

void ProtoFaceController::request_status() {
    send(R"({"cmd":"request_status"})");
}

void ProtoFaceController::release_control() {
    send(R"({"cmd":"release_control"})");
}

// ── Static helper ─────────────────────────────────────────────────────────────

bool ProtoFaceController::socket_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

// ── Internal ──────────────────────────────────────────────────────────────────

bool ProtoFaceController::try_connect() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
    connected_ = true;
    return true;
}

void ProtoFaceController::reconnect_loop() {
    using namespace std::chrono_literals;
    while (running_) {
        if (!connected_) {
            try_connect();
        }
        // Also retry shm open if it wasn't available at start().
        if (!shm_.is_open()) {
            shm_.open();
        }
        std::this_thread::sleep_for(2s);
    }
}

// ── Panel preview ─────────────────────────────────────────────────────────────

bool ProtoFaceController::open_shm(const char* path) {
    return shm_.open(path);
}

bool ProtoFaceController::get_frame_texture(GLuint& tex) {
    tex = panel_tex_;
    if (shm_.poll(panel_tex_)) {
        tex = panel_tex_;
        return true;
    }
    return false;
}

// ── Internal ──────────────────────────────────────────────────────────────────

bool ProtoFaceController::send(const std::string& json) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ < 0) return false;

    std::string msg = json + "\n";
    ssize_t sent = ::write(fd_, msg.c_str(), msg.size());
    if (sent < 0) {
        ::close(fd_);
        fd_ = -1;
        connected_ = false;
        return false;
    }
    return true;
}

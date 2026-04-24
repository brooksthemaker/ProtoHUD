#include "android_mirror.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>

#ifdef __unix__
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

AndroidMirror::AndroidMirror(const AndroidMirrorConfig& cfg) : cfg_(cfg) {}

AndroidMirror::~AndroidMirror() { stop(); }

bool AndroidMirror::start() {
    if (running_) return true;
    if (!spawn_scrcpy()) return false;

    // Give scrcpy time to negotiate ADB and create the V4L2 device node.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    cap_.open(cfg_.v4l2_sink, cv::CAP_V4L2);
    if (!cap_.isOpened()) {
        fprintf(stderr, "[android] V4L2 sink %s not ready — is v4l2loopback loaded?\n",
                cfg_.v4l2_sink.c_str());
        kill_scrcpy();
        return false;
    }

    running_ = true;
    thread_  = std::thread(&AndroidMirror::capture_loop, this);
    return true;
}

void AndroidMirror::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    cap_.release();
    kill_scrcpy();
    connected_ = false;
}

bool AndroidMirror::get_frame(GLuint& out) {
    std::lock_guard<std::mutex> lk(slot_.mtx);
    out = slot_.tex;
    if (!slot_.dirty || slot_.buf.empty()) return false;
    upload_texture(slot_.tex, slot_.w, slot_.h, slot_.buf.data());
    out       = slot_.tex;
    slot_.dirty = false;
    return true;
}

// ── Capture thread ────────────────────────────────────────────────────────────

void AndroidMirror::capture_loop() {
    cv::Mat frame, rgba;
    while (running_) {
        if (!cap_.read(frame) || frame.empty()) {
            connected_ = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        connected_ = true;
        cv::cvtColor(frame, rgba, cv::COLOR_BGR2RGBA);
        {
            std::lock_guard<std::mutex> lk(slot_.mtx);
            slot_.w     = rgba.cols;
            slot_.h     = rgba.rows;
            slot_.buf.assign(rgba.data, rgba.data + rgba.total() * rgba.elemSize());
            slot_.dirty = true;
        }
    }
}

// ── scrcpy process management ─────────────────────────────────────────────────

bool AndroidMirror::spawn_scrcpy() {
#ifdef __unix__
    // Build scrcpy argv. All char arrays live on the stack until execvp() in the child.
    char max_size_arg[32], v4l2_arg[64], fps_arg[32];
    snprintf(max_size_arg, sizeof(max_size_arg), "--max-size=%d",  cfg_.max_size);
    snprintf(v4l2_arg,    sizeof(v4l2_arg),    "--v4l2-sink=%s",  cfg_.v4l2_sink.c_str());
    snprintf(fps_arg,     sizeof(fps_arg),     "--max-fps=%d",    cfg_.fps);

    std::vector<const char*> args = {
        "scrcpy",
        "--no-audio",      // audio is handled by the spatial audio engine
        "--no-control",    // read-only mirror; disables touch/input injection
        "--video-codec=h264",
        max_size_arg,
        v4l2_arg,
        fps_arg,
    };
    if (!cfg_.adb_serial.empty()) {
        args.push_back("--serial");
        args.push_back(cfg_.adb_serial.c_str());
    }
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { perror("[android] fork"); return false; }

    if (pid == 0) {
        // Child: redirect output to /dev/null, then exec scrcpy.
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execvp("scrcpy", const_cast<char* const*>(args.data()));
        _exit(127); // execvp only returns on failure
    }

    scrcpy_pid_ = pid;
    return true;
#else
    fprintf(stderr, "[android] scrcpy subprocess not supported on this platform\n");
    return false;
#endif
}

void AndroidMirror::kill_scrcpy() {
#ifdef __unix__
    if (scrcpy_pid_ <= 0) return;
    ::kill(scrcpy_pid_, SIGTERM);
    waitpid(scrcpy_pid_, nullptr, 0);
    scrcpy_pid_ = -1;
#endif
}

// ── GL texture upload (render thread only) ────────────────────────────────────

void AndroidMirror::upload_texture(GLuint& tex, int w, int h, const uint8_t* rgba) {
    if (tex == 0) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        slot_.tex_w = w;
        slot_.tex_h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}

#include "android_mirror.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
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

    // scrcpy needs time to push its server, start the phone's encoder, and begin
    // streaming into the loopback. With v4l2loopback exclusive_caps=1 the capture
    // interface doesn't even appear until a producer is streaming — so poll the
    // open (up to ~12 s) instead of a single fixed-delay attempt, which raced
    // scrcpy's startup and failed with "can't be used to capture".
    bool opened = false;
    for (int attempt = 0; attempt < 24; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (cap_.open(cfg_.v4l2_sink, cv::CAP_V4L2) && cap_.isOpened()) {
            opened = true;
            break;
        }
        cap_.release();
    }
    if (!opened) {
        fprintf(stderr, "[android] V4L2 sink %s not ready after ~12s — is scrcpy producing "
                        "frames? check /tmp/protohud-scrcpy.log; is v4l2loopback loaded?\n",
                cfg_.v4l2_sink.c_str());
        kill_scrcpy();
        return false;
    }

    // In virtual-display mode, learn which display id scrcpy created so
    // navigate_to() can target it.
    if (cfg_.new_display) capture_new_display_id();

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

// Both flags below are scrcpy spawn arguments, so a live mirror must be
// restarted to pick them up. start() blocks up to ~12s, so kick it off the
// caller's thread.
void AndroidMirror::set_turn_screen_off(bool v) {
    if (cfg_.turn_screen_off == v) return;
    cfg_.turn_screen_off = v;
    if (running_) {
        stop();
        std::thread([this]{ start(); }).detach();
    }
}

void AndroidMirror::set_new_display(bool v) {
    if (cfg_.new_display == v) return;
    cfg_.new_display = v;
    if (running_) {
        stop();
        std::thread([this]{ start(); }).detach();
    }
}

// ── Preset-destination navigation ─────────────────────────────────────────────

// scrcpy logs the virtual display it creates, e.g.
//   "INFO: New display: 1080x2400/280 (id=2)"
// Parse the last such "...display... id=N" line from the scrcpy log.
void AndroidMirror::capture_new_display_id() {
    display_id_ = -1;
    FILE* f = fopen("/tmp/protohud-scrcpy.log", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char* p = strstr(line, "id=");
        if (p && strstr(line, "display")) {
            int id = atoi(p + 3);
            if (id >= 0) display_id_ = id;
        }
    }
    fclose(f);
    if (display_id_ >= 0)
        fprintf(stderr, "[android] virtual display id=%d\n", display_id_.load());
    else
        fprintf(stderr, "[android] could not determine virtual display id "
                        "(navigation will target the main display)\n");
}

static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
    }
    return out;
}

bool AndroidMirror::navigate_to(const std::string& query) {
#ifdef __unix__
    if (query.empty()) return false;

    // Substitute {q} in the URI template with the URL-encoded destination.
    std::string uri = cfg_.nav_uri_template;
    const std::string enc = url_encode(query);
    for (size_t p; (p = uri.find("{q}")) != std::string::npos; )
        uri.replace(p, 3, enc);

    const int did = display_id_.load();
    const std::string disp = std::to_string(did);
    const bool to_virtual = cfg_.new_display && did >= 0;

    // adb [-s serial] shell am start [--display N] -a VIEW -d <uri>
    std::vector<const char*> args = { "adb" };
    if (!cfg_.adb_serial.empty()) { args.push_back("-s"); args.push_back(cfg_.adb_serial.c_str()); }
    args.push_back("shell"); args.push_back("am"); args.push_back("start");
    if (to_virtual) { args.push_back("--display"); args.push_back(disp.c_str()); }
    args.push_back("-a"); args.push_back("android.intent.action.VIEW");
    args.push_back("-d"); args.push_back(uri.c_str());
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { perror("[android] fork(adb)"); return false; }
    if (pid == 0) {
        freopen("/tmp/protohud-scrcpy.log", "a", stderr);  // append intent output
        dup2(STDERR_FILENO, STDOUT_FILENO);
        execvp("adb", const_cast<char* const*>(args.data()));
        _exit(127);
    }
    waitpid(pid, nullptr, 0);  // adb returns promptly
    fprintf(stderr, "[android] navigate_to '%s' -> %s (display=%d)\n",
            query.c_str(), uri.c_str(), to_virtual ? did : -1);
    return true;
#else
    (void)query; return false;
#endif
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
    int fail_streak = 0;
    while (running_) {
        if (!cap_.read(frame) || frame.empty()) {
            connected_ = false;
            if (++fail_streak > 50) {
                // ~5 s of failures — reopen the v4l2 device in case scrcpy restarted
                cap_.release();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                cap_.open(cfg_.v4l2_sink, cv::CAP_V4L2);
                fail_streak = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        fail_streak = 0;
        connected_  = true;
        if (frame.cols > 0 && frame.rows > 0)
            frame_aspect_.store(static_cast<float>(frame.cols) / static_cast<float>(frame.rows));
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
    char max_size_arg[32], v4l2_arg[64], fps_arg[32], newdisp_arg[64], startapp_arg[128];
    snprintf(max_size_arg, sizeof(max_size_arg), "--max-size=%d",  cfg_.max_size);
    snprintf(v4l2_arg,    sizeof(v4l2_arg),    "--v4l2-sink=%s",  cfg_.v4l2_sink.c_str());
    snprintf(fps_arg,     sizeof(fps_arg),     "--max-fps=%d",    cfg_.fps);

    std::vector<const char*> args = {
        "scrcpy",
        "--no-audio",      // audio is handled by the spatial audio engine
        "--no-playback",   // don't open a preview window (scrcpy 2.x+)
        "--video-codec=h264",
    };

    // Control must be enabled to create a virtual display / launch an app, or to
    // send the screen-off power command. ProtoHUD never injects input, so the
    // mirror stays read-only in practice; --no-control is only safe in plain mode.
    const bool need_control = cfg_.new_display || cfg_.turn_screen_off;
    if (!need_control) {
        // NOTE: with --no-control, --stay-awake/--turn-screen-off are rejected
        // ("...if control is disabled") and scrcpy exits before streaming.
        args.push_back("--no-control");
    }

    if (cfg_.new_display) {
        // Stream a separate virtual display (e.g. a maps app) instead of the
        // phone's own screen, which stays free/lockable. Needs Android 14+.
        if (!cfg_.new_display_size.empty()) {
            snprintf(newdisp_arg, sizeof(newdisp_arg), "--new-display=%s",
                     cfg_.new_display_size.c_str());
            args.push_back(newdisp_arg);
        } else {
            args.push_back("--new-display");
        }
        if (!cfg_.start_app.empty()) {
            snprintf(startapp_arg, sizeof(startapp_arg), "--start-app=%s",
                     cfg_.start_app.c_str());
            args.push_back(startapp_arg);
        }
        args.push_back("--stay-awake");   // keep the virtual display alive
    } else if (cfg_.turn_screen_off) {
        // Black out the phone's screen while still streaming it to the HUD.
        args.push_back("--turn-screen-off");
        args.push_back("--stay-awake");   // don't let the dark device sleep
    }

    args.push_back(max_size_arg);
    args.push_back(v4l2_arg);
    args.push_back(fps_arg);
    if (!cfg_.adb_serial.empty()) {
        args.push_back("--serial");
        args.push_back(cfg_.adb_serial.c_str());
    }
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { perror("[android] fork"); return false; }

    if (pid == 0) {
        // Child: capture scrcpy's output to a log so failures are diagnosable
        // (it was /dev/null, which hid every error). scrcpy logs to stderr; fold
        // stdout into the same file. Inspect with: cat /tmp/protohud-scrcpy.log
        freopen("/tmp/protohud-scrcpy.log", "w", stderr);
        dup2(STDERR_FILENO, STDOUT_FILENO);
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

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
#  include <cerrno>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

AndroidMirror::AndroidMirror(const AndroidMirrorConfig& cfg) : cfg_(cfg) {}

AndroidMirror::~AndroidMirror() { stop(); }

// Verify the v4l2 sink exists and isn't already held before we spawn scrcpy, so
// a missing module or a stale handle yields a precise message instead of the
// vague "not ready after 12s".
bool AndroidMirror::preflight_sink(std::string& reason) const {
#ifdef __unix__
    struct stat st;
    if (stat(cfg_.v4l2_sink.c_str(), &st) != 0) {
        reason = cfg_.v4l2_sink + " does not exist — is v4l2loopback loaded? "
                 "(sudo modprobe v4l2loopback video_nr=N card_label=AndroidMirror "
                 "exclusive_caps=1)";
        return false;
    }
    if (!S_ISCHR(st.st_mode)) {
        reason = cfg_.v4l2_sink + " is not a character (video) device";
        return false;
    }
    // With exclusive_caps=1 a second producer gets EBUSY — that's the stale-handle
    // case. Probe by opening for output, then immediately release it.
    int fd = ::open(cfg_.v4l2_sink.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EBUSY)
            reason = cfg_.v4l2_sink + " is busy — another process holds it (stale "
                     "scrcpy?). Free it with: sudo fuser -k " + cfg_.v4l2_sink;
        else
            reason = "cannot open " + cfg_.v4l2_sink + ": " + strerror(errno);
        return false;
    }
    ::close(fd);
    return true;
#else
    (void)reason; return true;
#endif
}

bool AndroidMirror::start() {
    std::lock_guard<std::mutex> lk(life_mtx_);  // serialise with stop()/restarts
    if (running_) return true;
    kill_scrcpy();   // reap any scrcpy we still own before spawning another

    std::string why;
    if (!preflight_sink(why)) {
        fprintf(stderr, "[android] %s\n", why.c_str());
        return false;
    }
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

    // Force the V4L2 backend to hand us converted BGR. When OpenCV can't
    // negotiate scrcpy's pixel format it leaves CONVERT_RGB off and returns the
    // raw (often YUV) buffer, which cvtColor then mangles into static.
    cap_.set(cv::CAP_PROP_CONVERT_RGB, 1.0);

    // Log the negotiated V4L2 format so a "static / wrong-size" feed can be
    // diagnosed (a square-ish noisy image usually means OpenCV is reading the
    // scrcpy buffer in a pixel format it can't convert).
    {
        const int   fcc = static_cast<int>(cap_.get(cv::CAP_PROP_FOURCC));
        const char  fc[5] = { static_cast<char>(fcc & 0xFF),
                              static_cast<char>((fcc >> 8) & 0xFF),
                              static_cast<char>((fcc >> 16) & 0xFF),
                              static_cast<char>((fcc >> 24) & 0xFF), 0 };
        fprintf(stderr, "[android] v4l2 %s opened: %.0fx%.0f fourcc='%s' convert_rgb=%.0f\n",
                cfg_.v4l2_sink.c_str(), cap_.get(cv::CAP_PROP_FRAME_WIDTH),
                cap_.get(cv::CAP_PROP_FRAME_HEIGHT), fc, cap_.get(cv::CAP_PROP_CONVERT_RGB));
    }

    running_     = true;
    restart_req_ = false;
    thread_      = std::thread(&AndroidMirror::capture_loop, this);

    // Follow device rotation in plain-mirror mode. In new-display mode the phone's
    // physical orientation is irrelevant to the virtual display (and restarting
    // would tear down the launched app), so we don't watch there.
    if (!cfg_.new_display) {
        last_rotation_   = -1;   // re-seeded on first poll
        rotation_thread_ = std::thread(&AndroidMirror::rotation_watch_loop, this);
    }
    return true;
}

void AndroidMirror::stop() {
    std::lock_guard<std::mutex> lk(life_mtx_);  // serialise with start()/restarts
    running_ = false;
    if (rotation_thread_.joinable()) rotation_thread_.join();
    if (thread_.joinable())          thread_.join();
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
    int w = 0, h = 0;
    {
        // Lock covers only the buffer swap — the GL upload happens outside it
        // so the capture thread never blocks on a texture upload. The swapped-
        // out buffer is recycled by the capture thread's next assign().
        std::lock_guard<std::mutex> lk(slot_.mtx);
        out = slot_.tex;
        if (!slot_.dirty || slot_.buf.empty()) return false;
        upload_buf_.swap(slot_.buf);
        w = slot_.w;
        h = slot_.h;
        slot_.dirty = false;
    }
    upload_texture(slot_.tex, w, h, upload_buf_.data());   // tex: render thread only
    out = slot_.tex;
    return true;
}

// ── Capture thread ────────────────────────────────────────────────────────────

void AndroidMirror::capture_loop() {
    cv::Mat frame, rgba;
    int fail_streak = 0;
    bool logged_dims = false;
    while (running_) {
        // Device rotated → renegotiate the v4l2 sink at the new dimensions. Done
        // here (not on the watcher thread) so cap_/scrcpy stay single-threaded.
        if (restart_req_.exchange(false)) {
            restart_pipeline_for_rotation();
            fail_streak = 0;
            logged_dims = false;   // re-log dims at the new orientation
            continue;
        }
        if (!cap_.read(frame) || frame.empty()) {
            connected_ = false;
            if (++fail_streak > 50) {
                // ~5 s of failures — reopen the v4l2 device in case scrcpy restarted
                cap_.release();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (cap_.open(cfg_.v4l2_sink, cv::CAP_V4L2))
                    cap_.set(cv::CAP_PROP_CONVERT_RGB, 1.0);
                fail_streak = 0;
                logged_dims = false;   // re-log dims after a reconnect
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        fail_streak = 0;
        connected_  = true;
        if (!logged_dims) {
            fprintf(stderr, "[android] first frame: %dx%d channels=%d type=%d\n",
                    frame.cols, frame.rows, frame.channels(), frame.type());
            logged_dims = true;
        }
        if (frame.cols > 0 && frame.rows > 0)
            frame_aspect_.store(static_cast<float>(frame.cols) / static_cast<float>(frame.rows));
        switch (frame.channels()) {
            case 4:  cv::cvtColor(frame, rgba, cv::COLOR_BGRA2RGBA); break;
            case 1:  cv::cvtColor(frame, rgba, cv::COLOR_GRAY2RGBA); break;
            default: cv::cvtColor(frame, rgba, cv::COLOR_BGR2RGBA);  break;
        }
        {
            std::lock_guard<std::mutex> lk(slot_.mtx);
            slot_.w     = rgba.cols;
            slot_.h     = rgba.rows;
            slot_.buf.assign(rgba.data, rgba.data + rgba.total() * rgba.elemSize());
            slot_.dirty = true;
        }
    }
}

// ── Rotation following ─────────────────────────────────────────────────────────

// Read the device's display orientation (0/1/2/3) via adb. dumpsys display's
// mCurrentOrientation tracks the *applied* screen rotation (user_rotation and
// the window mRotation proved unreliable on the SM-S928U / Android 16). Returns
// -1 on any error so a transient adb hiccup never looks like a rotation.
int AndroidMirror::read_device_rotation() const {
    std::string cmd = "adb ";
    if (!cfg_.adb_serial.empty()) cmd += "-s " + cfg_.adb_serial + " ";
    cmd += "shell dumpsys display 2>/dev/null | "
           "grep -m1 -oE 'mCurrentOrientation=[0-9]'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    char buf[64] = {0};
    int  rot     = -1;
    if (fgets(buf, sizeof(buf), p)) {
        const char* eq = strrchr(buf, '=');
        if (eq) rot = atoi(eq + 1);
    }
    pclose(p);
    return rot;
}

// Watcher thread (plain-mirror mode): poll the device orientation ~1 Hz and, when
// it changes, ask the capture thread to restart the pipeline. Never touches cap_
// or scrcpy itself — only flips restart_req_ — so there is no teardown race with
// start()/stop(). Sleeps in short chunks so stop() stays responsive.
void AndroidMirror::rotation_watch_loop() {
    while (running_) {
        for (int i = 0; i < 5 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!running_) break;

        const int rot = read_device_rotation();
        if (rot < 0) continue;                    // adb hiccup — ignore
        if (last_rotation_ < 0) { last_rotation_ = rot; continue; }  // seed
        if (rot != last_rotation_) {
            fprintf(stderr, "[android] device rotation %d -> %d, restarting capture\n",
                    last_rotation_.load(), rot);
            last_rotation_ = rot;
            restart_req_   = true;
        }
    }
}

// Capture-thread side of a rotation restart: drop the consumer, respawn scrcpy
// (which re-locks to the now-current orientation and re-creates the v4l2 sink at
// the new WxH), then reopen. Bails early if stop() flips running_ mid-restart so
// teardown can't be stalled for the full reopen window.
void AndroidMirror::restart_pipeline_for_rotation() {
    cap_.release();
    kill_scrcpy();
    if (!spawn_scrcpy()) {
        fprintf(stderr, "[android] rotation restart: scrcpy respawn failed\n");
        connected_ = false;
        return;
    }
    bool opened = false;
    for (int attempt = 0; attempt < 24 && running_; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (cap_.open(cfg_.v4l2_sink, cv::CAP_V4L2) && cap_.isOpened()) {
            opened = true;
            break;
        }
        cap_.release();
    }
    if (!opened) {
        if (running_)
            fprintf(stderr, "[android] rotation restart: sink %s not ready\n",
                    cfg_.v4l2_sink.c_str());
        connected_ = false;
        return;
    }
    cap_.set(cv::CAP_PROP_CONVERT_RGB, 1.0);
    fprintf(stderr, "[android] rotation restart: reopened %.0fx%.0f\n",
            cap_.get(cv::CAP_PROP_FRAME_WIDTH), cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
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
        "--no-playback",   // feed the v4l2 sink without decoding/playing locally.
        "--no-window",     // and don't open scrcpy's own window — otherwise every
                           // (re)spawn pops a window over the HUD and steals input
                           // focus, which the rotation restarts made constant. An
                           // earlier note claimed --no-window stopped v4l2 frames on
                           // 3.3.4; that no longer reproduces with the sink reloaded
                           // exclusive_caps=1 — verified frames still reach OpenCV.
        "--video-codec=h264",
        // Lock the capture orientation. The v4l2loopback sink fixes its WxH when
        // scrcpy starts and a consumer (OpenCV) is reading; it can't be resized
        // mid-stream. Without a lock, rotating the phone makes scrcpy push the new
        // orientation's frames into the old-sized sink, which breaks the feed.
        // '@' = lock to whatever orientation the device is in at *this* start, so
        // mid-stream rotation can't corrupt the feed. To actually follow rotation
        // the watcher thread restarts the pipeline (see rotation_watch_loop), and
        // the fresh scrcpy then locks to the new orientation.
        "--capture-orientation=@",
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
        // Own process group so kill_scrcpy() can signal scrcpy *and* its children
        // (adb server, on-device server) as a unit — a plain SIGTERM to the client
        // left those holding the v4l2 device, which is how stale handles piled up.
        setpgid(0, 0);
        // Child: capture scrcpy's output to a log so failures are diagnosable
        // (it was /dev/null, which hid every error). scrcpy logs to stderr; fold
        // stdout into the same file. Inspect with: cat /tmp/protohud-scrcpy.log
        freopen("/tmp/protohud-scrcpy.log", "w", stderr);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        execvp("scrcpy", const_cast<char* const*>(args.data()));
        _exit(127); // execvp only returns on failure
    }
    setpgid(pid, pid);   // also set in parent to avoid a fork/exec race

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
    // Signal the whole process group (scrcpy + adb/server children), then
    // escalate to SIGKILL if it doesn't exit within ~2s so nothing keeps the
    // v4l2 sink open.
    ::kill(-scrcpy_pid_, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        if (waitpid(scrcpy_pid_, nullptr, WNOHANG) == scrcpy_pid_) {
            scrcpy_pid_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(-scrcpy_pid_, SIGKILL);
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
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    // Reallocate GPU storage only when the mirror resolution changes (device
    // rotation); otherwise sub-image into the existing texture — a full
    // glTexImage2D per frame reallocated ~10 MB of texture storage at up to
    // 60 fps and could stall on the texture's prior use.
    if (w != slot_.tex_w || h != slot_.tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        slot_.tex_w = w;
        slot_.tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

#include "video_recorder.h"

#include "app_state.h"
#include "gl_utils.h"
#include "gl_async_read.h"
#include "vitrue/xr_display.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <GLES2/gl2.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using clock_t_ = std::chrono::steady_clock;

namespace {

std::string now_stamp() {
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tm_buf {};
    localtime_r(&tt, &tm_buf);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);
    return ts;
}

std::string fmt_mmss(double secs) {
    if (secs < 0) secs = 0;
    int total = static_cast<int>(secs);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    return buf;
}

int parse_fourcc(const std::string& s) {
    if (s.size() == 4)
        return cv::VideoWriter::fourcc(s[0], s[1], s[2], s[3]);
    return cv::VideoWriter::fourcc('m', 'p', '4', 'v');
}

constexpr size_t kMaxQueue = 8;  // frames buffered before the encoder; drop oldest if full

} // namespace

struct VideoRecorder::Impl {
    // ── Encode worker ──────────────────────────────────────────────────────────
    // Frames carry bottom-up rows straight from the GPU readback; the worker
    // flips while converting so the render thread never touches the pixels.
    struct Frame { std::vector<uint8_t> rgba; int w = 0, h = 0; int stream = 0; };

    // Async double-buffered FBO readback, one per eye stream. A synchronous
    // glReadPixels here drained the GPU pipeline per eye per capture tick —
    // recording "Both" at 30fps was 60 full stalls a second.
    gl::AsyncFboReader readers[2];

    std::thread             worker;
    std::mutex              qmtx;
    std::condition_variable qcv;          // wakes worker when a frame arrives / on stop
    std::condition_variable drained_cv;   // signalled when the queue empties
    std::deque<Frame>       queue;
    bool                    busy        = false;
    bool                    stop_worker = false;

    std::mutex      writer_mtx;           // guards writers during open/close vs write
    cv::VideoWriter writers[2];           // 0 = left eye, 1 = right eye
    bool            writer_open[2] = {false, false};

    // ── Recording state (render thread only) ────────────────────────────────────
    bool        recording = false;
    bool        paused    = false;
    VideoCamera cam       = VideoCamera::Left;
    int         fps       = 30;
    int         fourcc    = 0;
    std::string dir;
    std::string base_ts;                  // shared timestamp for this recording's files
    int         segment   = 0;            // current segment / run index (1-based)
    clock_t_::time_point seg_start;       // start of the current (un-paused) run
    double      accumulated_s = 0.0;      // recorded time across runs, excluding pauses
    clock_t_::time_point last_grab;
    std::vector<std::string> files;       // every file written this session

    uint32_t toast_id = 0;

    Impl() {
        worker = std::thread([this] { worker_loop(); });
    }
    ~Impl() { stop(); }

    // Idempotent: stop the encode worker (draining queued frames) and finalise
    // any open writers. Safe to call more than once and from the dtor.
    // The PBOs are released only on the first (explicit, render-thread) call —
    // by the time the dtor runs at scope exit the GL context is already gone.
    void stop() {
        bool was_running = false;
        {
            std::lock_guard<std::mutex> lk(qmtx);
            if (!stop_worker) { stop_worker = true; was_running = true; }
        }
        if (was_running) qcv.notify_all();
        if (worker.joinable()) worker.join();
        close_writers();
        if (was_running) { readers[0].release(); readers[1].release(); }
    }

    void worker_loop() {
        for (;;) {
            Frame f;
            {
                std::unique_lock<std::mutex> lk(qmtx);
                qcv.wait(lk, [this] { return stop_worker || !queue.empty(); });
                if (queue.empty()) {
                    if (stop_worker) return;
                    continue;
                }
                f = std::move(queue.front());
                queue.pop_front();
                busy = true;
            }
            {
                cv::Mat rgba(f.h, f.w, CV_8UC4, f.rgba.data());
                cv::Mat bgr;
                cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
                cv::flip(bgr, bgr, 0);   // GL readback is bottom-up
                std::lock_guard<std::mutex> wl(writer_mtx);
                if (f.stream >= 0 && f.stream < 2 && writer_open[f.stream])
                    writers[f.stream].write(bgr);
            }
            {
                std::lock_guard<std::mutex> lk(qmtx);
                busy = false;
                if (queue.empty()) drained_cv.notify_all();
            }
        }
    }

    // Block until every queued frame has been written. Render thread, rare events only.
    void drain() {
        std::unique_lock<std::mutex> lk(qmtx);
        drained_cv.wait(lk, [this] { return queue.empty() && !busy; });
    }

    void close_writers() {
        std::lock_guard<std::mutex> wl(writer_mtx);
        for (int i = 0; i < 2; ++i) {
            if (writer_open[i]) { writers[i].release(); writer_open[i] = false; }
        }
    }

    // Which writer/eye streams are active for the selected camera.
    void active_streams(int out[2], int& n) const {
        n = 0;
        if (cam == VideoCamera::Left  || cam == VideoCamera::Both) out[n++] = 0;
        if (cam == VideoCamera::Right || cam == VideoCamera::Both) out[n++] = 1;
    }

    // Open writers for the current segment. Returns false (and rolls back) on failure.
    bool open_segment(XRDisplay& xr) {
        int streams[2], n;
        active_streams(streams, n);
        std::lock_guard<std::mutex> wl(writer_mtx);
        for (int i = 0; i < n; ++i) {
            int s = streams[i];
            gl::Fbo& fbo = (s == 0) ? xr.eye_left() : xr.eye_right();
            const char* tag = (s == 0) ? "L" : "R";
            std::string path = dir + "/video_" + base_ts + "_" + tag +
                               "_seg" + std::to_string(segment) + ".mp4";
            bool ok = writers[s].open(path, fourcc, static_cast<double>(fps),
                                      cv::Size(fbo.w, fbo.h), /*isColor=*/true);
            if (!ok || !writers[s].isOpened()) {
                for (int j = 0; j < 2; ++j)
                    if (writer_open[j]) { writers[j].release(); writer_open[j] = false; }
                return false;
            }
            writer_open[s] = true;
            files.push_back(path);
        }
        return true;
    }

    void enqueue(Frame f) {
        {
            std::lock_guard<std::mutex> lk(qmtx);
            if (queue.size() >= kMaxQueue) queue.pop_front();  // drop oldest, keep latency bounded
            queue.push_back(std::move(f));
        }
        qcv.notify_one();
    }

    // ── Toast helpers (state.mtx held by caller) ────────────────────────────────
    static uint32_t push_toast(AppState& s, std::string title, std::string body,
                               float auto_dismiss, std::vector<NotifAction> actions) {
        Notification n;
        n.type           = NotifType::App;
        n.title          = std::move(title);
        n.body           = std::move(body);
        n.auto_dismiss_s = auto_dismiss;
        n.actions        = std::move(actions);
        s.notifs.push(std::move(n));
        return s.notifs.items.front().id;
    }

    double elapsed() const {
        double e = accumulated_s;
        if (recording && !paused)
            e += std::chrono::duration<double>(clock_t_::now() - seg_start).count();
        return e;
    }
};

// ── Public API ──────────────────────────────────────────────────────────────

VideoRecorder::VideoRecorder()  : impl_(std::make_unique<Impl>()) {}
VideoRecorder::~VideoRecorder() = default;
void VideoRecorder::stop() { if (impl_) impl_->stop(); }
bool VideoRecorder::recording() const { return impl_->recording; }

void VideoRecorder::tick(XRDisplay& xr, AppState& state, const VideoConfig& cfg) {
    Impl& d = *impl_;

    // 1. Snapshot + clear the pending request.
    VideoRequest req;
    VideoCamera  sel_cam;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        req     = state.video_request;
        sel_cam = state.video_camera;
        state.video_request = VideoRequest::None;
    }

    auto set_action = [](VideoRequest r) {
        return [r](AppState& s) { std::lock_guard<std::mutex> lk(s.mtx); s.video_request = r; };
    };

    // 2. Apply control transitions.
    if (req == VideoRequest::Start) req = d.recording ? VideoRequest::Stop : VideoRequest::Start;

    if (req == VideoRequest::Start && !d.recording) {
        d.cam    = sel_cam;
        d.fps    = std::max(1, cfg.fps);
        d.fourcc = parse_fourcc(cfg.fourcc);
        d.dir    = cfg.dir;
        d.base_ts       = now_stamp();
        d.segment       = 1;
        d.accumulated_s = 0.0;
        d.paused        = false;
        d.files.clear();
        std::error_code ec; fs::create_directories(d.dir, ec);

        if (!d.open_segment(xr)) {
            std::lock_guard<std::mutex> lk(state.mtx);
            Impl::push_toast(state, "Recording failed",
                             "Could not open encoder (" + cfg.fourcc + ")", 6.f, {});
            return;
        }
        d.recording = true;
        d.seg_start = clock_t_::now();
        d.last_grab = d.seg_start - std::chrono::milliseconds(1000 / d.fps);
        std::lock_guard<std::mutex> lk(state.mtx);
        d.toast_id = Impl::push_toast(state, "Recording", "REC  00:00", 0.f,
                                      { {"PAUSE", set_action(VideoRequest::Pause)},
                                        {"STOP",  set_action(VideoRequest::Stop)} });
        state.video_recording = true;
        state.video_paused    = false;
    }
    else if (req == VideoRequest::Pause && d.recording && !d.paused) {
        d.readers[0].cancel(); d.readers[1].cancel();   // drop in-flight readbacks
        d.drain();
        d.close_writers();
        d.accumulated_s += std::chrono::duration<double>(clock_t_::now() - d.seg_start).count();
        d.paused = true;
        std::lock_guard<std::mutex> lk(state.mtx);
        state.notifs.dismiss(d.toast_id);
        d.toast_id = Impl::push_toast(state, "Recording",
                                      "PAUSED  " + fmt_mmss(d.elapsed()), 0.f,
                                      { {"RESUME", set_action(VideoRequest::Resume)},
                                        {"STOP",   set_action(VideoRequest::Stop)} });
        state.video_paused = true;
    }
    else if (req == VideoRequest::Resume && d.recording && d.paused) {
        ++d.segment;
        if (!d.open_segment(xr)) {
            std::lock_guard<std::mutex> lk(state.mtx);
            Impl::push_toast(state, "Recording failed", "Could not reopen encoder", 6.f, {});
            // fall through to a clean stop
            d.recording = false; d.paused = false;
            state.notifs.dismiss(d.toast_id); d.toast_id = 0;
            state.video_recording = false; state.video_paused = false;
            return;
        }
        d.paused    = false;
        d.seg_start = clock_t_::now();
        d.last_grab = d.seg_start - std::chrono::milliseconds(1000 / d.fps);
        std::lock_guard<std::mutex> lk(state.mtx);
        state.notifs.dismiss(d.toast_id);
        d.toast_id = Impl::push_toast(state, "Recording", "REC  " + fmt_mmss(d.elapsed()), 0.f,
                                      { {"PAUSE", set_action(VideoRequest::Pause)},
                                        {"STOP",  set_action(VideoRequest::Stop)} });
        state.video_paused = false;
    }
    else if (req == VideoRequest::Stop && d.recording) {
        if (!d.paused)
            d.accumulated_s += std::chrono::duration<double>(clock_t_::now() - d.seg_start).count();
        d.recording = false;
        d.paused    = false;
        d.readers[0].cancel(); d.readers[1].cancel();   // drop in-flight readbacks
        d.drain();
        d.close_writers();

        uintmax_t bytes = 0;
        for (const auto& p : d.files) {
            std::error_code ec; auto sz = fs::file_size(p, ec);
            if (!ec) bytes += sz;
        }
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        char body[96];
        std::snprintf(body, sizeof(body), "%d seg \xc2\xb7 %s \xc2\xb7 %.1f MB",
                      d.segment, fmt_mmss(d.accumulated_s).c_str(), mb);

        std::lock_guard<std::mutex> lk(state.mtx);
        state.notifs.dismiss(d.toast_id);
        d.toast_id = 0;
        Impl::push_toast(state, "Video saved", body, 6.f, {});
        state.video_recording = false;
        state.video_paused    = false;
    }

    if (!d.recording) return;

    // 3. Grab frames at the configured rate (decoupled from render FPS).
    if (!d.paused) {
        auto now = clock_t_::now();
        const auto interval = std::chrono::milliseconds(1000 / d.fps);
        if (now - d.last_grab >= interval) {
            d.last_grab = now;
            int streams[2], n;
            d.active_streams(streams, n);
            for (int i = 0; i < n; ++i) {
                int s = streams[i];
                gl::Fbo& fbo = (s == 0) ? xr.eye_left() : xr.eye_right();
                Impl::Frame f;
                f.stream = s;
                // Kicks this tick's GPU→PBO copy and delivers the previous
                // one — no pipeline drain, one capture interval of latency.
                if (d.readers[s].read(fbo, f.rgba, f.w, f.h))
                    d.enqueue(std::move(f));
            }
        }
    }

    // 4. Keep the live timer toast current.
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        for (auto& n : state.notifs.items) {
            if (n.id != d.toast_id) continue;
            n.body = (d.paused ? "PAUSED  " : "REC  ") + fmt_mmss(d.elapsed());
            break;
        }
    }
}

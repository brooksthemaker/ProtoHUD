#pragma once
#include <memory>
#include <string>

struct AppState;
class  XRDisplay;

// Encoder configuration, sourced from config.json (see "video" section).
struct VideoConfig {
    std::string dir;             // output directory (created if missing)
    int         fps    = 30;     // capture/playback frame rate
    std::string fourcc = "mp4v"; // 4-char OpenCV FOURCC (mp4v is widely available)
};

// ── VideoRecorder ─────────────────────────────────────────────────────────────
// Records the camera eye FBOs (the same clean, HUD-free source the photo path
// uses) to MP4. Driven entirely from the render thread via tick(); the actual
// H.264/MPEG-4 encoding runs on a background worker so the render loop never
// blocks on it.
//
// Control flow: input handlers / toast actions post a VideoRequest into
// AppState; tick() consumes it. VideoRequest::Start toggles (start when idle,
// stop when recording). Pause/Resume split the output into separate "segment"
// files. Recording status is mirrored back into AppState for the menu/HUD, and a
// live "REC mm:ss" toast (with Pause/Stop actions) is kept updated.
class VideoRecorder {
public:
    VideoRecorder();
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&)            = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // Call once per frame on the render (GL) thread, after the eye FBOs are drawn.
    void tick(XRDisplay& xr, AppState& state, const VideoConfig& cfg);

    // Stop the encode worker and finalise any open MP4 writers. Idempotent.
    // Call during shutdown so teardown is deterministic and happens before the
    // GL context is destroyed, rather than in the destructor at program exit.
    void stop();

    bool recording() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

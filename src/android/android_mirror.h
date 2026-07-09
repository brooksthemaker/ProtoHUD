#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include <GLES2/gl2.h>
#include <opencv2/videoio.hpp>

struct AndroidMirrorConfig {
    bool        enabled         = false;
    std::string v4l2_sink       = "/dev/video4"; // v4l2loopback device scrcpy writes into
    std::string adb_serial;                       // empty = first connected USB device
    int         max_size        = 1080;           // scrcpy --max-size (longest dimension, px)
    int         fps             = 30;             // scrcpy --max-fps
    // Black out the phone's own display while mirroring (scrcpy --turn-screen-off).
    // Requires scrcpy control to be enabled, so when this is on the mirror runs
    // *with* control (ProtoHUD never injects input, so it's still read-only in
    // practice); when off, the mirror runs --no-control and the phone stays lit.
    bool        turn_screen_off = false;
    // Stream a separate *virtual* display (scrcpy --new-display) instead of the
    // phone's own screen — launch a dedicated app (e.g. a maps app) on it and
    // mirror that, leaving the phone's real screen free/lockable. Needs
    // Android 14+ on the device. Implies control on (to create the display and
    // launch the app); ProtoHUD still injects no input.
    bool        new_display      = false;
    std::string new_display_size;            // e.g. "1080x2400" or "1920x1080/280"; empty = device default
    std::string start_app;                   // app to launch, e.g. "org.organicmaps" or "?maps" (fuzzy)

    // Preset-destination navigation: selecting one fires an intent at the
    // mirrored display so the maps app jumps to turn-by-turn — no HUD touch
    // needed. {q} in the template is replaced with the URL-encoded query.
    struct NavDestination { std::string name; std::string query; };
    std::vector<NavDestination> destinations;
    std::string nav_uri_template = "google.navigation:q={q}";  // Organic Maps: "geo:0,0?q={q}"
};

// Mirrors an Android device into a GL texture via scrcpy → V4L2 loopback → OpenCV.
//
// Prerequisites on the CM5:
//   sudo apt install scrcpy v4l2loopback-dkms
//   sudo modprobe v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1
//   # Add to /etc/modules + /etc/modprobe.d/v4l2loopback.conf for persistence.
//   # Connect Android device via USB with USB debugging enabled.
//
// Threading:
//   start() / stop()  — call from any non-render thread (blocks up to ~2 s)
//   get_frame()       — call from the render thread only (uploads to GL)

class AndroidMirror {
public:
    explicit AndroidMirror(const AndroidMirrorConfig& cfg);
    ~AndroidMirror();

    // Spawns scrcpy, waits for V4L2 sink, starts capture thread.
    // Must NOT be called from the render thread.
    bool start();

    // Stops capture thread and kills scrcpy. Idempotent.
    void stop();

    bool  is_running()    const { return running_;   }
    bool  is_connected()  const { return connected_; }
    // Aspect ratio (w/h) of the live frame; 9/16 until first frame arrives.
    float frame_aspect()  const { return frame_aspect_.load(); }

    // Blacking out the phone display. Changing it while running restarts scrcpy
    // (the flag is a spawn argument). Callable from any non-render thread.
    bool  turn_screen_off() const { return cfg_.turn_screen_off; }
    void  set_turn_screen_off(bool v);

    // Virtual-display ("maps display") mode — streams cfg_.start_app on its own
    // display instead of the phone's screen. Restarts scrcpy when toggled.
    bool  new_display_enabled() const { return cfg_.new_display; }
    void  set_new_display(bool v);

    // Preset-destination navigation. navigate_to() fires a maps intent at the
    // mirrored display (the virtual display in new-display mode, else the phone's
    // main display). Run off the render thread. destinations() feeds the menu.
    bool  navigate_to(const std::string& query);
    const std::vector<AndroidMirrorConfig::NavDestination>& destinations() const {
        return cfg_.destinations;
    }

    // Render-thread only: upload latest frame to a GL texture.
    // Returns true if a new frame was uploaded. out is always set to
    // the current texture ID (0 until the first frame arrives).
    bool get_frame(GLuint& out);

    const AndroidMirrorConfig& config() const { return cfg_; }

private:
    void capture_loop();
    bool spawn_scrcpy();
    void kill_scrcpy();
    void upload_texture(GLuint& tex, int w, int h, const uint8_t* rgba);
    void capture_new_display_id();   // parse scrcpy log for the virtual display id
    bool preflight_sink(std::string& reason) const;  // v4l2 sink exists & not busy

    // Rotation following (plain-mirror mode only). The watcher thread polls the
    // device's display orientation and, on a change, asks the capture thread to
    // restart the scrcpy→v4l2 pipeline so the sink renegotiates the new WxH (it
    // can't be resized mid-stream). All cap_/scrcpy mutation stays on the capture
    // thread, so the watcher only ever flips an atomic — no cross-thread teardown.
    void rotation_watch_loop();
    void restart_pipeline_for_rotation();   // capture-thread side of the restart
    int  read_device_rotation() const;       // adb display orientation, -1 on error

    AndroidMirrorConfig cfg_;
    pid_t               scrcpy_pid_ = -1;
    std::atomic<int>    display_id_ { -1 };  // scrcpy virtual display id (new-display mode)

    // Shared between capture thread (writer) and render thread (reader).
    struct TexSlot {
        GLuint               tex   = 0;   // render thread only
        int                  tex_w = 0, tex_h = 0;   // render thread only
        std::mutex           mtx;         // guards buf/w/h/dirty
        std::vector<uint8_t> buf;
        int                  w = 0, h = 0;
        bool                 dirty = false;
    } slot_;
    // Render-thread staging buffer: get_frame() swaps slot_.buf into this under
    // the lock, then does the GL upload outside it, so the capture thread never
    // blocks on a texture upload (and the freed buffer is recycled by assign).
    std::vector<uint8_t> upload_buf_;

    cv::VideoCapture  cap_;
    std::thread       thread_;
    std::thread        rotation_thread_;  // polls device orientation (plain mode)
    std::mutex         life_mtx_;    // serialises start()/stop() so rapid toggles
                                     // never spawn overlapping scrcpy instances
    std::atomic<bool>  running_      { false };
    std::atomic<bool>  connected_   { false };
    std::atomic<bool>  restart_req_ { false };  // watcher → capture thread: re-spawn
    std::atomic<int>   last_rotation_{ -1 };     // last seen device orientation
    std::atomic<float> frame_aspect_{ 9.f / 16.f };
};

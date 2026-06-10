#pragma once
// ── gif_player.h ───────────────────────────────────────────────────────────────
// C++ port of protoface/gif_player.py. Decodes all frames + per-frame delays of
// a GIF (via stb_image) into panel-sized RGBA cv::Mats and advances playback on
// update(). get_frame() returns the current frame (CV_8UC4) or an empty Mat when
// idle, so the controller can use it as a full-colour face replacement.

#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace face {

class GifPlayer {
public:
    GifPlayer(int width, int height) : w_(width), h_(height) {}

    void load(const std::string& path, bool loop = true);  // decode + start
    void stop();
    void update(double dt);
    cv::Mat get_frame() const;          // empty Mat when not playing
    bool playing() const { return playing_; }
    int  width()   const { return w_; } // target frame size frames are decoded to
    int  height()  const { return h_; }

    // Sorted list of *.gif paths in folder.
    static std::vector<std::string> scan_folder(const std::string& folder);

private:
    int w_, h_;
    std::vector<cv::Mat> frames_;       // CV_8UC4 RGBA, panel-sized
    std::vector<double>  durations_;    // seconds per frame
    int    frame_idx_ = 0;
    double elapsed_   = 0.0;
    bool   playing_   = false;
    bool   loop_      = true;
};

} // namespace face

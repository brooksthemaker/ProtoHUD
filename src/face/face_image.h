#pragma once
// ── face_image.h ───────────────────────────────────────────────────────────────
// Small OpenCV image helpers for the native face renderer.
//
// Channel convention: all cv::Mat buffers in the face renderer are RGB / RGBA
// ordered (channel 0 = R), matching the Protoface numpy code — NOT OpenCV's
// native BGR. PNGs loaded via cv::imread (BGR/BGRA) are converted on load. The
// final canvas is therefore RGB, which is what the shm preview format and the
// (later) Piomatter output expect.

#include <string>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace face {

// Load a PNG as an (h, w) RGBA cv::Mat (CV_8UC4), NEAREST-resized to panel size.
// Returns an empty Mat if the file can't be read.
inline cv::Mat load_png_rgba(const std::string& path, int w, int h) {
    cv::Mat raw = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (raw.empty()) return cv::Mat();
    cv::Mat rgba;
    if (raw.channels() == 4)      cv::cvtColor(raw, rgba, cv::COLOR_BGRA2RGBA);
    else if (raw.channels() == 3) cv::cvtColor(raw, rgba, cv::COLOR_BGR2RGBA);
    else if (raw.channels() == 1) cv::cvtColor(raw, rgba, cv::COLOR_GRAY2RGBA);
    else                          return cv::Mat();
    cv::Mat out;
    cv::resize(rgba, out, cv::Size(w, h), 0, 0, cv::INTER_NEAREST);
    return out;
}

// Load a PNG as an (h, w) RGB cv::Mat (CV_8UC3) at native size (no resize).
inline cv::Mat load_png_rgb_native(const std::string& path) {
    cv::Mat raw = cv::imread(path, cv::IMREAD_COLOR);   // always 3-channel BGR
    if (raw.empty()) return cv::Mat();
    cv::Mat rgb;
    cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
    return rgb;
}

// Equivalent of np.roll on a 2D image: shift columns by dx and rows by dy with
// wrap-around. Positive dx shifts content to the right.
inline cv::Mat roll(const cv::Mat& m, int dx, int dy) {
    const int w = m.cols, h = m.rows;
    cv::Mat out = m;
    if (w > 0 && (dx % w) != 0) {
        int s = ((dx % w) + w) % w;
        cv::Mat tmp(h, w, m.type());
        out.colRange(w - s, w).copyTo(tmp.colRange(0, s));
        out.colRange(0, w - s).copyTo(tmp.colRange(s, w));
        out = tmp;
    }
    if (h > 0 && (dy % h) != 0) {
        int s = ((dy % h) + h) % h;
        cv::Mat tmp(h, w, m.type());
        out.rowRange(h - s, h).copyTo(tmp.rowRange(0, s));
        out.rowRange(0, h - s).copyTo(tmp.rowRange(s, h));
        out = tmp;
    }
    return out;
}

} // namespace face

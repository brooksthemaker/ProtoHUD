#include "gif_player.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <opencv2/imgproc.hpp>

// Own static stb_image implementation (STB_IMAGE_STATIC keeps the symbols
// file-local so they don't collide with splash.cpp's / nanovg's copies).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

namespace fs = std::filesystem;

namespace face {

void GifPlayer::load(const std::string& path, bool loop) {
    frames_.clear();
    durations_.clear();
    playing_ = false;

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "[gif] cannot open '%s'\n", path.c_str()); return; }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (buf.empty()) { std::fprintf(stderr, "[gif] empty file '%s'\n", path.c_str()); return; }

    int x = 0, y = 0, z = 0, comp = 0;
    int* delays = nullptr;
    // req_comp = 4 → RGBA frames stacked z deep; delays[i] in milliseconds.
    unsigned char* data = stbi_load_gif_from_memory(
        buf.data(), static_cast<int>(buf.size()), &delays, &x, &y, &z, &comp, 4);
    if (!data || z <= 0) {
        std::fprintf(stderr, "[gif] no frames decoded from '%s'\n", path.c_str());
        if (data) stbi_image_free(data);
        if (delays) std::free(delays);
        return;
    }

    const size_t frame_bytes = static_cast<size_t>(x) * y * 4;
    for (int i = 0; i < z; ++i) {
        cv::Mat src(y, x, CV_8UC4, data + i * frame_bytes);   // RGBA, references data
        cv::Mat dst;
        cv::resize(src, dst, cv::Size(w_, h_), 0, 0, cv::INTER_NEAREST);
        frames_.push_back(dst);                               // owns its own buffer
        double dur = (delays ? delays[i] : 100) / 1000.0;
        durations_.push_back(std::max(0.016, dur));           // floor ~60fps
    }
    stbi_image_free(data);
    if (delays) std::free(delays);

    loop_ = loop;
    frame_idx_ = 0;
    elapsed_   = 0.0;
    playing_   = true;
}

void GifPlayer::stop() {
    playing_ = false;
    frames_.clear();
}

void GifPlayer::update(double dt) {
    if (!playing_ || frames_.empty()) return;
    elapsed_ += dt;
    while (elapsed_ >= durations_[frame_idx_]) {
        elapsed_ -= durations_[frame_idx_];
        frame_idx_ += 1;
        if (frame_idx_ >= static_cast<int>(frames_.size())) {
            if (loop_) {
                frame_idx_ = 0;
            } else {
                frame_idx_ = static_cast<int>(frames_.size()) - 1;
                playing_   = false;
                return;
            }
        }
    }
}

cv::Mat GifPlayer::get_frame() const {
    if (!playing_ || frames_.empty()) return cv::Mat();
    return frames_[frame_idx_];
}

std::vector<std::string> GifPlayer::scan_folder(const std::string& folder) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(folder, ec)) return out;
    for (auto& e : fs::directory_iterator(folder, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".gif") out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace face

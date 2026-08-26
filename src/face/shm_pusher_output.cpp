#include "shm_pusher_output.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>

namespace face {

ShmPusherOutput::ShmPusherOutput(int width, int height,
                                 std::vector<Panel> panels,
                                 std::vector<Half> halves, std::string path)
    : w_(width), h_(height), panels_(std::move(panels)),
      halves_(std::move(halves)), path_(std::move(path)) {
    size_ = static_cast<size_t>(1) + static_cast<size_t>(w_) * h_ * 3;
}

std::vector<cv::Rect> ShmPusherOutput::covered_regions() const {
    std::lock_guard<std::mutex> lk(panels_mtx_);
    std::vector<cv::Rect> out;
    out.reserve(panels_.size());
    for (const auto& p : panels_) out.push_back(p.rect);
    return out;
}

std::vector<NamedRegion> ShmPusherOutput::covered_named_regions() const {
    std::lock_guard<std::mutex> lk(panels_mtx_);
    std::vector<NamedRegion> out;
    out.reserve(panels_.size());
    for (const auto& p : panels_) out.push_back({p.name, p.rect});
    return out;
}

void ShmPusherOutput::set_panel_angles(const std::vector<double>& angles) {
    std::lock_guard<std::mutex> lk(panels_mtx_);
    for (size_t i = 0; i < panels_.size() && i < angles.size(); ++i)
        panels_[i].angle = angles[i];
}

void ShmPusherOutput::set_panel_flips(
        const std::vector<std::array<bool, 2>>& flips) {
    std::lock_guard<std::mutex> lk(panels_mtx_);
    for (size_t i = 0; i < panels_.size() && i < flips.size(); ++i) {
        panels_[i].flip_x = flips[i][0];
        panels_[i].flip_y = flips[i][1];
    }
}

void ShmPusherOutput::set_half_flips(
        const std::vector<std::array<bool, 2>>& halves) {
    std::lock_guard<std::mutex> lk(panels_mtx_);
    for (size_t i = 0; i < halves_.size() && i < halves.size(); ++i) {
        halves_[i].flip_x = halves[i][0];
        halves_[i].flip_y = halves[i][1];
    }
}

ShmPusherOutput::~ShmPusherOutput() { close(); }

bool ShmPusherOutput::open() {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0660);
    if (fd_ < 0) return false;
    if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        ::close(fd_); fd_ = -1; return false;
    }
    void* m = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) { ::close(fd_); fd_ = -1; return false; }
    map_ = static_cast<uint8_t*>(m);
    return true;
}

// Caller holds panels_mtx_.
bool ShmPusherOutput::has_panel_map() const {
    if (panels_.empty()) return false;
    for (const auto& p : panels_)
        if (p.dst.width <= 0 || p.dst.height <= 0) return false;
    return true;
}

void ShmPusherOutput::show(const cv::Mat& rgb) {
    if (!map_) return;

    cv::Mat canvas = rgb;
    if (canvas.type() != CV_8UC3)
        canvas.convertTo(canvas, CV_8UC3);

    cv::Mat out;
    std::unique_lock<std::mutex> panels_lk(panels_mtx_);
    if (has_panel_map()) {
        // Gather: the canvas spans the panels where they physically sit (with
        // whatever gaps the nudges opened up), so copy each panel's slice into
        // its slot in the chain-order framebuffer. Anything the canvas covers
        // that no panel sits on is simply not wired to an LED and drops out.
        if (fb_.rows != h_ || fb_.cols != w_ || fb_.type() != CV_8UC3)
            fb_.create(h_, w_, CV_8UC3);
        fb_.setTo(cv::Scalar(0, 0, 0));
        const cv::Rect canvas_box(0, 0, canvas.cols, canvas.rows);
        const cv::Rect fb_box(0, 0, w_, h_);
        // Mirror this panel's sampled tile. Done on the tile, after the canvas
        // has been read, so a flip can never disturb what a neighbouring
        // rotated panel reads out of the canvas next to it.
        auto flip_tile = [](cv::Mat& tile, bool fx, bool fy) {
            if (!fx && !fy) return;
            cv::Mat f;
            cv::flip(tile, f, (fx && fy) ? -1 : (fx ? 1 : 0));
            tile = f;
        };
        for (const auto& p : panels_) {
            if (p.angle != 0.0) {
                // Mounting rotation: resample the canvas along a rect tilted
                // about the panel's centre, straight into a panel-sized tile.
                // Bilinear rather than nearest — at a few degrees, nearest
                // turns every near-horizontal face edge into a visible
                // staircase, which is exactly what this is meant to fix.
                const cv::Point2f c(
                    static_cast<float>(p.rect.x) + p.rect.width  * 0.5f,
                    static_cast<float>(p.rect.y) + p.rect.height * 0.5f);
                cv::Mat M = cv::getRotationMatrix2D(c, p.angle, 1.0);
                // Retarget the rotation's output from the canvas centre to the
                // tile's own centre, so the warp crops as it rotates.
                M.at<double>(0, 2) += p.rect.width  * 0.5 - c.x;
                M.at<double>(1, 2) += p.rect.height * 0.5 - c.y;
                cv::Mat tile;
                cv::warpAffine(canvas, tile, M,
                               cv::Size(p.rect.width, p.rect.height),
                               sharp_rot_.load() ? cv::INTER_NEAREST
                                                 : cv::INTER_LINEAR,
                               cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
                flip_tile(tile, p.flip_x, p.flip_y);
                const cv::Rect dst_clipped = p.dst & fb_box;
                if (dst_clipped.width <= 0 || dst_clipped.height <= 0) continue;
                tile(cv::Rect(dst_clipped.x - p.dst.x, dst_clipped.y - p.dst.y,
                              dst_clipped.width, dst_clipped.height))
                    .copyTo(fb_(dst_clipped));
                continue;
            }
            // Clip both ends and keep them aligned: a panel nudged partly off
            // the canvas (or a slot partly off the framebuffer) copies only the
            // overlap rather than throwing.
            const cv::Rect src = p.rect & canvas_box;
            if (src.width <= 0 || src.height <= 0) continue;
            if (p.flip_x || p.flip_y) {
                // Flipped: build the whole tile so the mirror is about the
                // panel's own centre even when the canvas clipped one edge.
                cv::Mat tile(p.rect.height, p.rect.width, CV_8UC3,
                             cv::Scalar(0, 0, 0));
                canvas(src).copyTo(tile(cv::Rect(src.x - p.rect.x, src.y - p.rect.y,
                                                 src.width, src.height)));
                flip_tile(tile, p.flip_x, p.flip_y);
                const cv::Rect dc = p.dst & fb_box;
                if (dc.width <= 0 || dc.height <= 0) continue;
                tile(cv::Rect(dc.x - p.dst.x, dc.y - p.dst.y, dc.width, dc.height))
                    .copyTo(fb_(dc));
                continue;
            }
            cv::Rect dst(p.dst.x + (src.x - p.rect.x),
                         p.dst.y + (src.y - p.rect.y),
                         src.width, src.height);
            const cv::Rect dst_clipped = dst & fb_box;
            if (dst_clipped.width <= 0 || dst_clipped.height <= 0) continue;
            const cv::Rect src_clipped(src.x + (dst_clipped.x - dst.x),
                                       src.y + (dst_clipped.y - dst.y),
                                       dst_clipped.width, dst_clipped.height);
            canvas(src_clipped).copyTo(fb_(dst_clipped));
        }
        // Half strips, in framebuffer space: a row of panels mounted rotated as
        // one assembly. Flipping the strip here swaps which panel shows which
        // end of it — the same thing rotating the physical row does — without
        // ever touching the canvas the panels sampled from.
        for (const auto& hf : halves_) {
            if (!hf.flip_x && !hf.flip_y) continue;
            const cv::Rect roi = hf.rect & fb_box;
            if (roi != hf.rect || roi.width <= 0 || roi.height <= 0) continue;
            cv::Mat region = fb_(roi), flipped;
            cv::flip(region, flipped, (hf.flip_x && hf.flip_y) ? -1
                                                               : (hf.flip_x ? 1 : 0));
            flipped.copyTo(region);
        }
        out = fb_;
    } else {
        // No panel map (daemon mode / legacy): the canvas IS the framebuffer.
        if (canvas.cols != w_ || canvas.rows != h_)
            cv::resize(canvas, canvas, cv::Size(w_, h_), 0, 0, cv::INTER_NEAREST);
        out = canvas;
    }
    panels_lk.unlock();

    // Whole-output mirror, applied to the assembled framebuffer so a 180° mount
    // swaps whole panels rather than sliding the face across the canvas.
    const bool fx = flip_x_.load(), fy = flip_y_.load();
    if (fx || fy) {
        cv::Mat flipped;
        cv::flip(out, flipped, (fx && fy) ? -1 : (fx ? 1 : 0));
        out = flipped;
    }
    if (!out.isContinuous())
        out = out.clone();

    std::memcpy(map_ + 1, out.data, static_cast<size_t>(w_) * h_ * 3);
    map_[0] = ++seq_;   // publish: bump the sequence counter last
}

void ShmPusherOutput::close() {
    if (map_) {
        std::memset(map_ + 1, 0, static_cast<size_t>(w_) * h_ * 3);   // blank
        map_[0] = ++seq_;
        ::munmap(map_, size_);
        map_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

} // namespace face

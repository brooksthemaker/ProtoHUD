#pragma once
// Outer-rim fade for the panel set: content dissolves toward black as it nears
// a physical panel edge, instead of being chopped off mid-image.
//
// ⚠ THE MASK IS THE DISTANCE TO THE BOUNDARY OF THE **UNION** OF THE PANEL
// RECTS — deliberately NOT a per-panel vignette. Panels that touch have no
// boundary between them, so a contiguous row stays contiguous. On the VANESSA2
// build the bottom two panels share a full 32px edge and each top panel shares
// ~50px with the one below it; fading every panel's own edges would paint dark
// bands straight down those seams, through a continuous image. Only the true
// outer rim (including the two edges facing the 30px gap between the top
// panels, which really do cut off) is a boundary, and it falls out of the
// geometry automatically — no per-edge settings to configure or keep in sync.
//
// Applied to the composited canvas, so the face, particles, effects and glitch
// all fade together. Runs before the scrolling banner so a readout stays fully
// legible, and before the test patterns so alignment work is never softened.

#include "dither.h"

#include <opencv2/opencv.hpp>
#include <algorithm>
#include <vector>

namespace face {

class EdgeFade {
public:
    // width_px: ramp width in canvas pixels; <= 0 disables (true no-op).
    // dither: break the ramp with an ordered stipple — see apply().
    // `panels` are the panel rects in canvas space; empty falls back to the
    // canvas rectangle, which is right for a single-panel or preview build.
    void apply(cv::Mat& canvas, const std::vector<cv::Rect>& panels,
               int width_px, bool dither) {
        if (canvas.empty() || canvas.type() != CV_8UC3 || width_px <= 0) return;
        rebuild_if_needed(canvas.size(), panels, width_px);
        if (mask_.empty()) return;

        // Ordered dither offsets come from face/dither.h — the same table the
        // face-layer depth reduction uses, so the two never disagree.
        for (int y = 0; y < canvas.rows; ++y) {
            const float*  m = mask_.ptr<float>(y);
            cv::Vec3b*    p = canvas.ptr<cv::Vec3b>(y);
            for (int x = 0; x < canvas.cols; ++x) {
                const float a = m[x];
                if (a >= 1.f) continue;
                if (a <= 0.f) { p[x] = cv::Vec3b(0, 0, 0); continue; }
                // Dither offset is applied at the QUANTISATION step, not to the
                // alpha: a shallow ramp across 8px of a 32px panel lands on only
                // a handful of output levels, and rounding them cleanly is what
                // produces visible steps. Spreading the rounding error over the
                // matrix trades a step for a stipple.
                const float d = dither ? bayer8_offset(x, y) : 0.f;
                for (int c = 0; c < 3; ++c) {
                    const float v = p[x][c] * a + d;
                    p[x][c] = static_cast<uchar>(
                        std::clamp(static_cast<int>(v + 0.5f), 0, 255));
                }
            }
        }
    }

    void invalidate() { cached_w_ = -1; }

private:
    void rebuild_if_needed(cv::Size sz, const std::vector<cv::Rect>& panels,
                           int width_px) {
        if (cached_w_ == width_px && cached_sz_ == sz && cached_panels_ == panels)
            return;
        cached_w_ = width_px; cached_sz_ = sz; cached_panels_ = panels;

        // Coverage: 255 where a panel actually shows pixels. Padded by the ramp
        // width so the CANVAS border counts as outside too — without the pad, a
        // panel flush against the canvas edge has no zero pixel to measure
        // against and distanceTransform would report it as deep interior, so
        // the one edge most in need of a fade would get none.
        const int pad = width_px + 2;
        cv::Mat cover(sz.height + pad * 2, sz.width + pad * 2, CV_8U,
                      cv::Scalar(0));
        const cv::Rect bounds(pad, pad, sz.width, sz.height);
        if (panels.empty()) {
            cover(bounds).setTo(255);
        } else {
            for (const auto& r : panels) {
                const cv::Rect s = (r + cv::Point(pad, pad)) & bounds;
                if (s.width > 0 && s.height > 0) cover(s).setTo(255);
            }
        }

        cv::Mat dist;
        cv::distanceTransform(cover, dist, cv::DIST_L2, 3);

        mask_.create(sz, CV_32F);
        const float w = static_cast<float>(width_px);
        for (int y = 0; y < sz.height; ++y) {
            const float* d = dist.ptr<float>(y + pad);
            float*       m = mask_.ptr<float>(y);
            for (int x = 0; x < sz.width; ++x) {
                // Smoothstep rather than a linear ramp: a straight ramp leaves a
                // visible crease where it meets full brightness, which on a
                // 32px-tall panel is as obvious as the hard edge it replaced.
                const float t = std::clamp(d[x + pad] / w, 0.f, 1.f);
                m[x] = t * t * (3.f - 2.f * t);
            }
        }
    }

    cv::Mat               mask_;          // CV_32F, 0..1, canvas-sized
    cv::Size              cached_sz_{0, 0};
    int                   cached_w_ = -1;
    std::vector<cv::Rect> cached_panels_;
};

}  // namespace face

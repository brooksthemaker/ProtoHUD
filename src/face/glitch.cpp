#include "glitch.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace face {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Shift a horizontal band [y0,y1) of an RGB image sideways by dx, filling the
// vacated edge with black (signal-loss feel). Self-overlap safe (clones first).
void shift_rows(cv::Mat& img, int y0, int y1, int dx) {
    y0 = std::max(0, y0);
    y1 = std::min(img.rows, y1);
    if (y1 <= y0 || dx == 0) return;
    const int W = img.cols;
    if (std::abs(dx) >= W) { img.rowRange(y0, y1).setTo(cv::Scalar(0, 0, 0)); return; }
    cv::Mat band = img.rowRange(y0, y1).clone();
    cv::Mat dst  = img.rowRange(y0, y1);
    dst.setTo(cv::Scalar(0, 0, 0));
    if (dx > 0) band.colRange(0, W - dx).copyTo(dst.colRange(dx, W));
    else        band.colRange(-dx, W).copyTo(dst.colRange(0, W + dx));
}

// Horizontally shift a single-channel image by dx into a fresh black image.
cv::Mat hshift(const cv::Mat& src, int dx) {
    if (dx == 0) return src.clone();
    cv::Mat out = cv::Mat::zeros(src.size(), src.type());
    const int W = src.cols;
    if (std::abs(dx) >= W) return out;
    if (dx > 0) src.colRange(0, W - dx).copyTo(out.colRange(dx, W));
    else        src.colRange(-dx, W).copyTo(out.colRange(0, W + dx));
    return out;
}

} // namespace

nlohmann::json GlitchConfig::to_json() const {
    return nlohmann::json{
        {"enabled", enabled}, {"intensity", intensity},
        {"burst_rate", burst_rate}, {"burst_min", burst_min}, {"burst_max", burst_max},
        {"chromatic", chromatic}, {"tearing", tearing}, {"blocks", blocks},
        {"bitcrush", bitcrush}, {"dropout", dropout}, {"datamosh", datamosh},
        {"region_desync", region_desync}, {"expr_flicker", expr_flicker},
    };
}

GlitchConfig GlitchConfig::from_json(const nlohmann::json& j) {
    GlitchConfig c;
    if (!j.is_object()) return c;
    c.enabled       = j.value("enabled",       c.enabled);
    c.intensity     = j.value("intensity",     c.intensity);
    c.burst_rate    = j.value("burst_rate",    c.burst_rate);
    c.burst_min     = j.value("burst_min",     c.burst_min);
    c.burst_max     = j.value("burst_max",     c.burst_max);
    c.chromatic     = j.value("chromatic",     c.chromatic);
    c.tearing       = j.value("tearing",       c.tearing);
    c.blocks        = j.value("blocks",        c.blocks);
    c.bitcrush      = j.value("bitcrush",      c.bitcrush);
    c.dropout       = j.value("dropout",       c.dropout);
    c.datamosh      = j.value("datamosh",      c.datamosh);
    c.region_desync = j.value("region_desync", c.region_desync);
    c.expr_flicker  = j.value("expr_flicker",  c.expr_flicker);
    return c;
}

GlitchEffect::GlitchEffect()
    : rng_(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())) {}

double GlitchEffect::rnd(double lo, double hi) {
    std::uniform_real_distribution<double> d(lo, hi);
    return d(rng_);
}

void GlitchEffect::tick(double dt, const GlitchConfig& cfg) {
    flicker_expr_ = false;
    if (!cfg.enabled || cfg.intensity <= 0.0) { env_ = 0.0; return; }

    if (cfg.burst_rate <= 0.0) {
        env_ = 1.0;                                  // constant-on
    } else if (burst_left_ > 0.0) {
        burst_left_ -= dt;
        // Smooth 0→1→0 over the burst so corruption ramps in and out.
        const double t = 1.0 - std::clamp(burst_left_ / std::max(1e-3, burst_len_), 0.0, 1.0);
        env_ = std::sin(t * kPi);
        if (burst_left_ <= 0.0) { env_ = 0.0; burst_left_ = 0.0; }
    } else {
        env_ = 0.0;
        next_in_ -= dt;
        if (next_in_ <= 0.0) {
            burst_len_  = rnd(cfg.burst_min, std::max(cfg.burst_min, cfg.burst_max));
            burst_left_ = burst_len_;
            // Poisson-style gap: -ln(u)/rate, plus the burst's own length.
            const double u = std::max(1e-4, rnd(0.0, 1.0));
            next_in_ = burst_len_ - std::log(u) / std::max(1e-3, cfg.burst_rate);
        }
    }

    // Expression flicker: sample only well inside an active burst.
    if (env_ > 0.2 && cfg.expr_flicker > 0.0 &&
        rnd(0.0, 1.0) < cfg.expr_flicker * env_ * 0.4) {
        flicker_expr_ = true;
        flicker_pick_ = rnd(0.0, 1.0);
    }
}

void GlitchEffect::apply(cv::Mat& rgb, const GlitchConfig& cfg) {
    if (rgb.empty() || rgb.type() != CV_8UC3) return;
    const double s = env_ * cfg.intensity;
    if (s <= 1e-3) { rgb.copyTo(prev_); return; }

    const int W = rgb.cols, H = rgb.rows;

    // datamosh smears the *clean* previous frame, so stash a clean copy first.
    cv::Mat clean;
    if (cfg.datamosh > 0.0) rgb.copyTo(clean);

    // 1. Region desync — split into eyes(top) / mouth(bottom) and slip each
    //    band independently. Eyes sit in the upper ~60% of these faces.
    if (cfg.region_desync > 0.0) {
        const int amp = static_cast<int>(std::round(cfg.region_desync * s * W * 0.12));
        if (amp > 0) {
            const int split = std::max(1, static_cast<int>(std::round(H * 0.62)));
            shift_rows(rgb, 0, split, static_cast<int>(std::round(rnd(-amp, amp))));
            shift_rows(rgb, split, H, static_cast<int>(std::round(rnd(-amp, amp))));
        }
    }

    // 2. Tearing — a handful of horizontal bands shoved sideways.
    if (cfg.tearing > 0.0) {
        const int bands = 1 + static_cast<int>(cfg.tearing * s * 6.0);
        const int amp   = std::max(1, static_cast<int>(std::round(cfg.tearing * s * W * 0.18)));
        for (int i = 0; i < bands; ++i) {
            const int y0 = static_cast<int>(rnd(0, H - 1));
            const int bh = 1 + static_cast<int>(rnd(1, std::max(2.0, H * 0.18)));
            shift_rows(rgb, y0, y0 + bh, static_cast<int>(std::round(rnd(-amp, amp))));
        }
    }

    // 3. Block shuffle — copy random source blocks over destination blocks.
    if (cfg.blocks > 0.0) {
        const int n = 1 + static_cast<int>(cfg.blocks * s * 8.0);
        const int jit = std::max(1, static_cast<int>(std::round(cfg.blocks * s * W * 0.15)));
        for (int i = 0; i < n; ++i) {
            const int bw = std::max(2, static_cast<int>(rnd(3, std::max(4.0, W * 0.2))));
            const int bh = std::max(2, static_cast<int>(rnd(2, std::max(3.0, H * 0.5))));
            const int dx = static_cast<int>(rnd(0, std::max(1, W - bw)));
            const int dy = static_cast<int>(rnd(0, std::max(1, H - bh)));
            const int sx = std::clamp(dx + static_cast<int>(std::round(rnd(-jit, jit))),
                                      0, std::max(0, W - bw));
            const int sy = std::clamp(dy + static_cast<int>(std::round(rnd(-2.0, 2.0))),
                                      0, std::max(0, H - bh));
            cv::Mat blk = rgb(cv::Rect(sx, sy, bw, bh)).clone();
            blk.copyTo(rgb(cv::Rect(dx, dy, bw, bh)));
        }
    }

    // 4. Chromatic split — shove the R and B planes apart (canvas is RGB, so
    //    channel 0 = R, channel 2 = B).
    if (cfg.chromatic > 0.0) {
        const int off = std::max(1, static_cast<int>(std::round(cfg.chromatic * s * 4.0)));
        std::vector<cv::Mat> ch;
        cv::split(rgb, ch);
        ch[0] = hshift(ch[0],  off);
        ch[2] = hshift(ch[2], -off);
        cv::merge(ch, rgb);
    }

    // 5. Bitcrush — posterise colour depth (8 levels → 2 as the amount rises).
    if (cfg.bitcrush > 0.0) {
        const int levels = std::clamp(static_cast<int>(std::round(8.0 - cfg.bitcrush * s * 6.0)), 2, 8);
        const double q = 255.0 / (levels - 1);
        rgb.forEach<cv::Vec3b>([q](cv::Vec3b& p, const int*) {
            for (int k = 0; k < 3; ++k)
                p[k] = static_cast<uchar>(std::clamp(
                    static_cast<int>(std::round(std::round(p[k] / q) * q)), 0, 255));
        });
    }

    // 6. Dropout — random horizontal bars go black or full static.
    if (cfg.dropout > 0.0) {
        const int n = static_cast<int>(cfg.dropout * s * 4.0);
        for (int i = 0; i < n; ++i) {
            const int y0 = static_cast<int>(rnd(0, H - 1));
            const int bh = 1 + static_cast<int>(rnd(1, std::max(2.0, H * 0.12)));
            cv::Mat band = rgb.rowRange(y0, std::min(H, y0 + bh));
            if (rnd(0.0, 1.0) < 0.5) {
                band.setTo(cv::Scalar(0, 0, 0));
            } else {
                cv::Mat noise(band.size(), CV_8UC3);
                cv::randu(noise, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
                noise.copyTo(band);
            }
        }
    }

    // 7. Datamosh — blend in a ghost of the previous clean frame.
    if (cfg.datamosh > 0.0 && !prev_.empty() &&
        prev_.size() == rgb.size() && prev_.type() == rgb.type()) {
        const double a = std::clamp(cfg.datamosh * s * 0.8, 0.0, 0.95);
        cv::addWeighted(prev_, a, rgb, 1.0 - a, 0.0, rgb);
    }
    if (cfg.datamosh > 0.0) clean.copyTo(prev_);
    else                    rgb.copyTo(prev_);
}

} // namespace face

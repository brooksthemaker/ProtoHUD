#include "eye_animations.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <opencv2/imgproc.hpp>

namespace face {

namespace {

inline uint8_t cb(double v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

inline double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// Cheap deterministic hash → [0,1). Used by the Glitch animation.
inline double hash01(int x, int y, int s) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u
               + static_cast<uint32_t>(y) * 668265263u
               + static_cast<uint32_t>(s) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFFu) / static_cast<double>(0x1000000u);
}

// Primary colour scaled by intensity, with a white core as intensity → 1.
inline cv::Vec3b paint(const EyeAnimParams& p, double inten) {
    inten = clamp01(inten);
    const double white = clamp01((inten - 0.82) / 0.18) * 150.0;
    return cv::Vec3b(cb(p.r * inten + white),
                     cb(p.g * inten + white),
                     cb(p.b * inten + white));
}

} // namespace

static cv::Mat render_rgb(const EyeAnimParams& p, double t, int w, int h) {
    cv::Mat out(std::max(1, h), std::max(1, w), CV_8UC3, cv::Scalar(0, 0, 0));
    const double cx = (w - 1) * clamp01(p.cx);
    const double cy = (h - 1) * clamp01(p.cy);
    const double scale = std::max(1.0, std::min(w, h) * 0.5);  // radius → ~1 at edge
    const double sz = std::max(0.1, p.size);
    const double sp = p.speed;

    if (p.type == EyeAnim::Glitch) {
        const int block = std::max(2, static_cast<int>(std::round(6.0 * sz)));
        const int step  = static_cast<int>(std::floor(t * std::max(0.1, sp) * 12.0));
        for (int y = 0; y < h; ++y) {
            cv::Vec3b* row = out.ptr<cv::Vec3b>(y);
            for (int x = 0; x < w; ++x) {
                const int bx = x / block, by = y / block;
                const double r = hash01(bx, by, step);
                // ~45% of blocks lit; brightness + occasional white flash.
                double inten = (r > 0.55) ? (0.35 + (r - 0.55) * 1.6) : 0.0;
                if (hash01(bx, by, step * 7 + 3) > 0.93) inten = 1.0;
                row[x] = paint(p, inten);
            }
        }
        return out;
    }

    for (int y = 0; y < h; ++y) {
        cv::Vec3b* row = out.ptr<cv::Vec3b>(y);
        for (int x = 0; x < w; ++x) {
            const double dx = (x - cx) / scale;
            const double dy = (y - cy) / scale;
            const double rr = std::sqrt(dx * dx + dy * dy);   // 0 at centre
            const double a  = std::atan2(dy, dx);
            double inten = 0.0;

            switch (p.type) {
            case EyeAnim::Spiral: {
                const double v = std::sin(2.0 * a + rr * (6.0 / sz) - t * 4.0 * sp);
                inten = clamp01(v * 1.3) * clamp01(1.15 - rr * 0.25);
                break;
            }
            case EyeAnim::Rings: {
                const double v = std::sin(rr * (10.0 / sz) - t * 5.0 * sp);
                inten = clamp01(v * 1.4);
                break;
            }
            case EyeAnim::Hearts: {
                const double pulse = 0.82 + 0.18 * std::sin(t * 4.0 * sp);
                const double k  = 1.0 / (1.25 * sz * pulse);
                const double hx = dx * k;
                const double hy = -dy * k;                    // y up
                const double q  = hx * hx + hy * hy - 1.0;
                const double f  = q * q * q - hx * hx * hy * hy * hy;
                inten = (f <= 0.0) ? (0.65 + 0.35 * pulse) : 0.0;
                break;
            }
            case EyeAnim::Swirl: {
                const double v = std::sin(3.0 * a + rr * (8.0 / sz) - t * 3.0 * sp);
                inten = (0.5 + 0.5 * v) * clamp01(1.2 - rr * 0.3);
                break;
            }
            case EyeAnim::Starburst: {
                const double v = std::cos(12.0 * a - t * 2.5 * sp);
                inten = clamp01(v) * clamp01(1.1 - rr * 0.55)
                        * (0.6 + 0.4 * std::sin(rr * (6.0 / sz) - t * 3.0 * sp));
                break;
            }
            case EyeAnim::XEyes: {
                // Cartoon K.O. cross: two diagonal strokes with soft edges
                // and a light pulse. 0.7071 = 1/√2 (point-to-line distance).
                const double pulse = 0.88 + 0.12 * std::sin(t * 5.0 * sp);
                const double thick = 0.15 * sz * pulse;
                const double d = std::min(std::fabs(dx - dy),
                                          std::fabs(dx + dy)) * 0.7071;
                if (rr < 0.85 * sz)
                    inten = clamp01(1.0 - d / thick) * (0.75 + 0.25 * pulse);
                break;
            }
            case EyeAnim::Radar: {
                // Sweep beam with an exponential afterglow trail, over faint
                // range rings, clipped to the scope radius.
                constexpr double kTau2 = 6.283185307179586;
                if (rr < 1.05 * sz) {
                    const double sweep = std::fmod(t * 2.0 * sp, kTau2);
                    double da = sweep - a;
                    da -= kTau2 * std::floor(da / kTau2);       // 0 at the beam
                    const double trail = std::exp(-da * 2.4);
                    const double rings = 0.22 * clamp01(
                        std::sin(rr * (12.0 / sz)) * 6.0 - 5.0); // thin circles
                    inten = clamp01(trail + rings) * clamp01(1.15 - rr * 0.4);
                }
                break;
            }
            case EyeAnim::Fire: {
                // Column flame: per-column flickering height, bright at the
                // panel base, fading toward the tip. Fills the whole panel
                // (like Glitch), so the centre setting is ignored.
                const double yy = (h - 1 - y) / static_cast<double>(std::max(1, h - 1));
                const double flick = 0.5 + 0.5 * std::sin(
                    x * 0.35 / sz + t * 9.0 * sp +
                    2.0 * std::sin(x * 0.13 - t * 5.0 * sp));
                const double hgt = 0.45 + 0.5 * flick;          // flame height
                const double v = clamp01((hgt - yy) / hgt);
                inten = v * v * (0.7 + 0.3 * flick);
                break;
            }
            case EyeAnim::Rain: {
                // Falling streaks: hash-phased columns, bright head + fading
                // tail, two interleaved layers for density. Fills the panel.
                const int cw = std::max(2, static_cast<int>(std::round(2.5 * sz)));
                const double yy = y / static_cast<double>(std::max(1, h - 1));
                for (int layer = 0; layer < 2; ++layer) {
                    const int col = x / cw + layer * 131;
                    const double ph = hash01(col, layer, 17);
                    const double fall = std::fmod(
                        t * (1.2 + ph) * sp * 0.8 + ph * 7.0, 1.4);
                    const double d = fall - yy;                 // head at 0
                    if (d >= 0.0 && d < 0.4)
                        inten = std::max(inten, clamp01(1.0 - d / 0.4) *
                                                (layer ? 0.6 : 1.0));
                }
                break;
            }
            case EyeAnim::Sparkle: {
                // Twinkling star field: sparse hash-picked cells, each with
                // its own twinkle rate/phase, bright at the cell centre.
                // Fills the panel.
                const int cell = std::max(3, static_cast<int>(std::round(5.0 * sz)));
                const int bx = x / cell, by = y / cell;
                if (hash01(bx, by, 3) > 0.7) {
                    const double ph = hash01(bx, by, 5) * 6.283185307179586;
                    const double tw = std::sin(
                        t * (2.0 + 3.0 * hash01(bx, by, 9)) * sp + ph);
                    const double lx = (x % cell - cell * 0.5) / (cell * 0.5);
                    const double ly = (y % cell - cell * 0.5) / (cell * 0.5);
                    const double rd = std::sqrt(lx * lx + ly * ly);
                    inten = clamp01(tw) * clamp01(1.0 - rd * 1.4);
                }
                break;
            }
            case EyeAnim::Heartbeat: {
                // Scrolling ECG trace on the cy baseline: P wave, QRS spike,
                // T wave as gaussian bumps along the scroll phase.
                auto bump = [](double uu, double c, double wd, double amp) {
                    const double d = (uu - c) / wd;
                    return amp * std::exp(-d * d * 4.0);
                };
                auto wave = [&](int px) {
                    const double u = std::fmod(
                        px / static_cast<double>(std::max(1, w)) -
                        t * 0.55 * sp + 16.0, 1.0);
                    return bump(u, 0.18, 0.030, 0.18)             // P
                         - bump(u, 0.28, 0.014, 0.16)             // Q
                         + bump(u, 0.31, 0.016, 0.95)             // R
                         - bump(u, 0.345, 0.016, 0.28)            // S
                         + bump(u, 0.50, 0.050, 0.22);            // T
                };
                // Span this column's and the next column's trace heights so
                // steep segments (the QRS spike) stay a connected line
                // instead of aliasing into detached dots.
                const double ty0 = cy - wave(x)     * scale * 0.9 * sz;
                const double ty1 = cy - wave(x + 1) * scale * 0.9 * sz;
                const double lo  = std::min(ty0, ty1);
                const double hi  = std::max(ty0, ty1);
                const double d   = (y < lo ? lo - y : y > hi ? y - hi : 0.0)
                                   / (scale * 0.06 * sz);
                inten = clamp01(1.0 - d);
                break;
            }
            default: break;
            }
            row[x] = paint(p, inten);
        }
    }
    return out;
}

cv::Mat render_eye_animation(const EyeAnimParams& p, double t, int w, int h) {
    // The compositor consumes RGBA face layers (composite() splits out the
    // alpha channel); the animation owns the whole panel, so it converts to
    // fully opaque RGBA here rather than teaching every draw loop about alpha.
    // (p.mirror is handled by the CALLER — it renders one half-width copy via
    // this function and composites left + mirrored right.)
    cv::Mat rgba;
    cv::cvtColor(render_rgb(p, t, w, h), rgba, cv::COLOR_RGB2RGBA);
    return rgba;
}

const char* eye_anim_name(EyeAnim a) {
    switch (a) {
    case EyeAnim::Spiral:    return "Spiral";
    case EyeAnim::Rings:     return "Rings";
    case EyeAnim::Hearts:    return "Hearts";
    case EyeAnim::Swirl:     return "Swirl";
    case EyeAnim::Starburst: return "Starburst";
    case EyeAnim::Glitch:    return "Glitch";
    case EyeAnim::XEyes:     return "X Eyes";
    case EyeAnim::Radar:     return "Radar";
    case EyeAnim::Fire:      return "Fire";
    case EyeAnim::Rain:      return "Rain";
    case EyeAnim::Sparkle:   return "Sparkle";
    case EyeAnim::Heartbeat: return "Heartbeat";
    default:                 return "?";
    }
}

int eye_anim_count() { return static_cast<int>(EyeAnim::Count); }

} // namespace face

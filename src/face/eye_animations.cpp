#include "eye_animations.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

cv::Mat render_eye_animation(const EyeAnimParams& p, double t, int w, int h) {
    cv::Mat out(std::max(1, h), std::max(1, w), CV_8UC3, cv::Scalar(0, 0, 0));
    const double cx = (w - 1) * 0.5;
    const double cy = (h - 1) * 0.5;
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
            default: break;
            }
            row[x] = paint(p, inten);
        }
    }
    return out;
}

const char* eye_anim_name(EyeAnim a) {
    switch (a) {
    case EyeAnim::Spiral:    return "Spiral";
    case EyeAnim::Rings:     return "Rings";
    case EyeAnim::Hearts:    return "Hearts";
    case EyeAnim::Swirl:     return "Swirl";
    case EyeAnim::Starburst: return "Starburst";
    case EyeAnim::Glitch:    return "Glitch";
    default:                 return "?";
    }
}

int eye_anim_count() { return static_cast<int>(EyeAnim::Count); }

} // namespace face

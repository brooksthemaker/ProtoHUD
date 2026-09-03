#include "eye_animations.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

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

static cv::Mat render_rgb(const EyeAnimParams& p, double t, int w, int h,
                         const EyeLidLine* lid, int lid_x0, float gravity_deg) {
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

    // ── Crying ────────────────────────────────────────────────────────────────
    // Tears welling on the eye's closed-lid line, then falling away. Like Glitch
    // this gets its own block: it needs w-sized precompute and never uses the
    // polar dx/dy/rr/a setup below, so it shouldn't pay sqrt+atan2 per pixel.
    //
    // Droplets are SPLATTED into a float accumulator over each drop's small bbox
    // rather than evaluated per-pixel — with ~40 streams a per-pixel test would
    // cost thousands of sqrt calls a frame, while the splats touch a few thousand
    // floats total. Still a pure function of (p, t, lid): every bit of randomness
    // comes from the stateless hash01, so the menu preview and the panel agree.
    // Waterfall shares all of this — the lid derivation, the spans and the wet
    // lid stroke are identical; only what happens BELOW the lid differs (discrete
    // droplets vs a continuous sheet), so they branch at the bottom.
    if (p.type == EyeAnim::Crying || p.type == EyeAnim::Waterfall) {
        const double spc = std::max(0.05, sp);
        cv::Mat1f acc(h, w, 0.f);

        // Gravity direction from head roll. Upright = straight down; clamped to
        // ±75° so there is always a downward component and a hard tilt can't send
        // tears running purely sideways. Everything below travels along this ray
        // rather than blindly down the panel, so tears run downhill when the head
        // leans — the whole point of feeding the IMU in.
        constexpr double kDeg2Rad = 0.017453292519943295;
        const double groll = std::clamp(static_cast<double>(gravity_deg), -75.0, 75.0)
                             * kDeg2Rad;
        // NOTE the minus on x. attitude roll is "+ = head tilted RIGHT"
        // (expression_director.h). At roll +90° the wearer's right is the
        // OBSERVER's left, so the face image has rotated such that world-down
        // expressed in image coords is (-1, 0) — tears run toward image -x.
        // ⚠️ This rig also applies panel mirror/flip transforms downstream, which
        // can invert x again. If tears run UPHILL on hardware, flip this one sign.
        const double gxd = -std::sin(groll), gyd = std::cos(groll);

        // The lid line, one row per column. -1 = no eye in that column.
        const bool have_lid = lid && !lid->empty() && lid->height == h &&
                              lid_x0 >= 0 && lid_x0 + w <= lid->width;
        std::vector<float> lidy(static_cast<size_t>(w), -1.f);
        if (have_lid) {
            // With a real lid, Position Y NUDGES the authored edge (±17.5% of the
            // panel) — the polygon can sit a pixel or two off the drawn art.
            const float nud = static_cast<float>((clamp01(p.cy) - 0.5) * h * 0.35);
            for (int x = 0; x < w; ++x) {
                const int b = lid->bottom[static_cast<size_t>(lid_x0 + x)];
                if (b >= 0)
                    lidy[x] = std::clamp(static_cast<float>(b) + nud, 0.f,
                                         static_cast<float>(h - 1));
            }
        } else {
            // No face context (menu preview, or a face with no eye regions):
            // a flat lid at Position Y so the animation still reads as crying.
            std::fill(lidy.begin(), lidy.end(), static_cast<float>(cy));
        }

        // Contiguous runs of eye columns — normally one per eye. Streams are
        // placed WITHIN a span so tears sit inside the eye instead of landing on
        // a global lattice that would straddle the bridge of the nose.
        std::vector<std::pair<int, int>> spans;
        for (int x = 0; x < w; ++x) {
            if (lidy[x] < 0.f) continue;
            if (!spans.empty() && spans.back().second == x - 1) spans.back().second = x;
            else                                                spans.emplace_back(x, x);
        }

        // The lid stroke itself, with a travelling highlight so it reads as WET.
        // Drawn first so brighter droplets win over it.
        for (int x = 0; x < w; ++x) {
            if (lidy[x] < 0.f) continue;
            const double ly    = lidy[x];
            const double thick = 0.55 + 0.35 * sz;
            const double glint = 0.70 + 0.30 * std::sin(x * 0.5 + t * 1.8 * spc);
            const int y_lo = std::max(0, static_cast<int>(std::floor(ly - 3.0)));
            const int y_hi = std::min(h - 1, static_cast<int>(std::ceil(ly + thick + 1.0)));
            for (int y = y_lo; y <= y_hi; ++y) {
                const double d = std::fabs(y - ly);
                // 0.95 ceiling so the line sits just under paint()'s 0.82 white
                // threshold at glint's trough and crosses it only at the peak —
                // a highlight that travels along the lid rather than a
                // permanently blown-out white bar.
                double v = clamp01(thick + 0.5 - d) * glint * 0.95;
                // Faint shading ABOVE the line: with blackout_eyes the eye box is
                // solid black, and this gives it closed-lid volume instead of
                // reading as a hole punched in the face.
                if (y < ly - 0.5)
                    v = std::max(v, 0.16 * clamp01(1.0 - (ly - y) / 3.0));
                acc(y, x) = std::max(acc(y, x), static_cast<float>(v));
            }
        }

        if (p.type == EyeAnim::Waterfall) {
            // A continuous SHEET rather than separate drops: every eye column
            // pours at once, with vertical streaks scrolling down it to read as
            // flow, a slow undulation along the lid so parts gush and parts
            // trickle, a bright lip where the water leaves the lid, and
            // turbulence pooling in the bottom rows.
            const double flow = 14.0 * spc;                 // streak scroll, rows/sec
            const double strw = std::max(1.0, 1.6 * sz);    // streak column width
            const double maxs = std::hypot(static_cast<double>(w),
                                           static_cast<double>(h)) + 4.0;
            // Perpendicular to gravity — used for spread, spray and splash-back.
            const double pxd = -gyd, pyd = gxd;
            for (int x = 0; x < w; ++x) {
                if (lidy[x] < 0.f) continue;
                const double y0   = lidy[x];
                const double span = std::max(1.0, (h - 1) - y0);
                const double wob  = 0.55 + 0.45 * std::sin(x * 0.55 + t * 1.1 * spc);
                const int    col  = static_cast<int>(x / strw);
                // Flow SURGES: the whole fall swells and eases like a real spout
                // rather than pouring at one dead-constant rate.
                const double surge = 0.78 + 0.22 * std::sin(t * 0.9 * spc +
                                                            hash01(col, 0, 29) * 6.2831853);
                // Step along the gravity ray at half-pixel intervals (so a leaning
                // sheet has no gaps) instead of straight down a column.
                for (double d = 0.0; d < maxs; d += 0.5) {
                    // The sheet SPREADS as it descends — a fall widens and frays
                    // instead of staying a clean ribbon. Offset perpendicular to
                    // gravity, hashed per depth band so the edge looks ragged.
                    const double sprd = 0.35 * sz * d / std::max(1.0, span);
                    const double jit  = (hash01(col, static_cast<int>(d * 0.5), 37) - 0.5)
                                        * sprd * 2.0;
                    const int px = static_cast<int>(std::lround(x  + gxd * d + pxd * jit));
                    const int py = static_cast<int>(std::lround(y0 + gyd * d + pyd * jit));
                    if (py < 0 || py >= h) break;
                    if (px < 0 || px >= w) continue;
                    // Quantised scrolling phase, hashed per column so neighbouring
                    // columns run at their own rate instead of in lockstep.
                    // MINUS t: a constant-phase feature then satisfies
                    // d = const + t*flow, i.e. it travels AWAY from the lid. With a
                    // plus the texture crawled back up the sheet and the whole fall
                    // read as flowing upward.
                    const double ph =
                        (d - t * flow * (0.75 + 0.5 * hash01(col, 0, 83))) / 3.0;
                    const double st = hash01(col, static_cast<int>(std::floor(ph)), 91);
                    // Brightest at the lip, easing with depth but never going dry —
                    // it's a fall, not a drip.
                    const double depth = 0.55 + 0.45 * clamp01(1.0 - d / span);
                    double v = depth * (0.45 + 0.55 * st) * wob * surge;
                    if (d < 1.5) v = std::max(v, 0.85);     // the lip
                    if (py >= h - 2)                         // splash pooling
                        v = std::max(v, 0.5 + 0.5 * hash01(
                                px, static_cast<int>(t * 18.0 * spc), 97));
                    acc(py, px) = std::max(acc(py, px), static_cast<float>(v));
                }

                // SPRAY: droplets that break off the sheet and fly their own
                // little ballistic arc — sideways kick plus gravity — instead of
                // the sheet being one solid slab. A few per column, each a
                // stateless function of (column, cycle) like the Crying drops.
                for (int q = 0; q < 2; ++q) {
                    const int sd = col * 71 + q * 17;
                    const double u  = t * (0.9 * spc) + hash01(sd, 0, 43) * 4.0;
                    const int    k  = static_cast<int>(std::floor(u));
                    const double f  = u - k;
                    if (hash01(sd, k, 47) < 0.55) continue;   // most cycles: no spray
                    // Detach part-way down the fall, then arc away.
                    const double d0   = span * (0.25 + 0.55 * hash01(sd, k, 51));
                    const double kick = (hash01(sd, k, 53) - 0.5) * 4.2 * sz;
                    const double along = d0 + span * 1.4 * f * f;      // gravity
                    const double across = kick * f;                     // sideways
                    const int px = static_cast<int>(std::lround(
                        x + gxd * along + pxd * across));
                    const int py = static_cast<int>(std::lround(
                        y0 + gyd * along + pyd * across));
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;
                    const double amp = 0.75 * (1.0 - f) * surge;
                    acc(py, px) = std::max(acc(py, px), static_cast<float>(amp));
                }

                // SPLASH-BACK: water hitting the bottom throws spray back UP
                // against gravity, which then falls again. Anchored at the impact
                // point so it tracks the lean.
                const double s_floor = ((h - 1.0) - y0) / std::max(0.25, gyd);
                if (s_floor > 1.0) {
                    for (int q = 0; q < 3; ++q) {
                        const int sd = col * 97 + q * 31;
                        const double u = t * (1.6 * spc) + hash01(sd, 0, 59) * 3.0;
                        const int    k = static_cast<int>(std::floor(u));
                        const double f = u - k;
                        if (hash01(sd, k, 61) < 0.5) continue;
                        // Up then down: height peaks mid-life (a lobbed arc).
                        const double up  = (1.6 + 2.4 * sz) * 4.0 * f * (1.0 - f);
                        const double out = (hash01(sd, k, 67) - 0.5) * 6.0 * sz * f;
                        const int px = static_cast<int>(std::lround(
                            x + gxd * s_floor - gxd * up + pxd * out));
                        const int py = static_cast<int>(std::lround(
                            y0 + gyd * s_floor - gyd * up + pyd * out));
                        if (px < 0 || px >= w || py < 0 || py >= h) continue;
                        acc(py, px) = std::max(acc(py, px),
                                               static_cast<float>(0.7 * (1.0 - f)));
                    }
                }
            }
        } else {
        const double gap = std::max(3.0, 7.0 * sz);   // target stream spacing
        for (const auto& sn : spans) {
            const int span_w = sn.second - sn.first + 1;
            const int n = std::max(1, static_cast<int>(std::lround(span_w / gap)));
            for (int i = 0; i < n; ++i) {
                const double base = sn.first + (i + 0.5) * span_w / static_cast<double>(n);
                // Seeded with the span's start so the two eyes drip INDEPENDENTLY
                // rather than in lockstep.
                const int sj = sn.first * 131 + i;

                const double rate = 0.42 * spc * (0.7 + 0.6 * hash01(sj, 0, 23));
                const double u    = t * rate + hash01(sj, 0, 29) * 5.0;
                const int    k    = static_cast<int>(std::floor(u));
                const double f    = u - k;
                if (hash01(sj, k, 41) < 0.28) continue;   // skip ~28% → irregular

                // Well on the lid, then fall under gravity. This two-phase motion
                // is what makes it a TEAR rather than rain.
                const double hold  = 0.28;
                const double swell = clamp01(f / hold);
                const double fall  = clamp01((f - hold) / (1.0 - hold));
                // Accelerate, then ease toward a terminal speed rather than
                // growing without bound. Normalised so gy still reaches 1 at f=1.
                const double gy    = (fall * fall / (1.0 + 0.6 * fall)) / 0.625;

                const double ds  = 0.75 + 0.5 * hash01(sj, k, 59);   // per-drop size
                const double jx  = (hash01(sj, k, 53) - 0.5) * gap * 0.35;
                const double sx  = base + jx;
                const int    sxi = std::clamp(static_cast<int>(std::lround(sx)), 0, w - 1);
                if (lidy[sxi] < 0.f) continue;
                const double y0 = lidy[sxi];
                if (h - 1 - y0 < 2.0) continue;      // lid at the panel floor: nowhere to fall

                const double rx  = (0.55 + 0.85 * sz) * ds;
                const double ry  = (0.85 + 1.15 * sz) * ds;
                const double srx = rx * (0.5 + 0.5 * swell);
                const double sry = ry * (0.5 + 0.5 * swell) * (1.0 + 0.5 * gy);
                // How far the drop travels along the gravity ray before it leaves
                // the panel, and how far along that ray it is right now.
                const double reach = (h + ry * 2.0 - y0) / std::max(0.25, gyd);
                const double trav  = reach * gy;
                // Lateral wobble PERPENDICULAR to gravity — a running drop shivers
                // rather than tracking a plumb line. Scaled by how far it has
                // fallen so it leaves the lid cleanly.
                const double wob = std::sin(f * 11.0 + hash01(sj, k, 67) * 6.2831853)
                                   * 0.6 * sz * fall;
                const double xc = sx + gxd * trav - gyd * wob;
                const double yc = y0 + ry * 0.6 + gyd * trav + gxd * wob;
                const double tail = (1.5 + 5.0 * sz) * gy;   // trail grows with speed

                const int bx0 = std::max(0, static_cast<int>(
                    std::floor(std::min(sx, xc) - srx - tail - 1.0)));
                const int bx1 = std::min(w - 1, static_cast<int>(
                    std::ceil(std::max(sx, xc) + srx + tail + 1.0)));
                const int by0 = std::max(0, static_cast<int>(
                    std::floor(std::min(y0, yc) - 1.0)));
                const int by1 = std::min(h - 1, static_cast<int>(
                    std::ceil(std::max(y0, yc) + sry + 1.0)));
                for (int y = by0; y <= by1; ++y) {
                    for (int x = bx0; x <= bx1; ++x) {
                        const double ex = (x - xc) / std::max(0.35, srx);
                        const double ey = (y - yc) / std::max(0.35, sry);
                        double v = clamp01((1.15 - std::sqrt(ex * ex + ey * ey)) * 1.8);
                        // Tail: behind the head ALONG the gravity ray (projected,
                        // so it trails correctly at any tilt) rather than straight up.
                        if (tail > 0.5) {
                            const double bxr = x - xc, byr = y - yc;
                            const double along = -(bxr * gxd + byr * gyd);
                            const double perp  = std::fabs(-bxr * gyd + byr * gxd);
                            if (along > 0.0 && along < tail)
                                v = std::max(v, 0.55 * clamp01(1.0 - along / tail) *
                                    clamp01(1.25 - perp / std::max(0.5, srx * 0.6)));
                        }
                        acc(y, x) = std::max(acc(y, x), static_cast<float>(v));
                    }
                }

                // Splash: once the ray has carried the drop past the bottom row,
                // burst outward from the impact point and fade. Stateless — the
                // "age" is just how far beyond the floor the drop has travelled.
                const double s_hit = ((h - 1.0) - y0) / std::max(0.25, gyd);
                if (s_hit > 0.0 && trav > s_hit) {
                    const double age = clamp01((trav - s_hit) / std::max(1.5, 3.0 * sz));
                    const double ix  = sx + gxd * s_hit;
                    const double rad = (0.8 + 3.2 * sz) * age;
                    const double amp = 0.8 * (1.0 - age);
                    const int px0 = std::max(0, static_cast<int>(std::floor(ix - rad - 1.0)));
                    const int px1 = std::min(w - 1, static_cast<int>(std::ceil(ix + rad + 1.0)));
                    const int py0 = std::max(0, h - 1 - static_cast<int>(
                        std::ceil(rad * 0.6)) - 1);
                    for (int y = py0; y < h; ++y)
                        for (int x = px0; x <= px1; ++x) {
                            const double dxs = (x - ix) / std::max(0.5, rad);
                            const double dys = ((h - 1.0) - y) / std::max(0.5, rad * 0.6);
                            const double rd  = std::sqrt(dxs * dxs + dys * dys);
                            if (rd > 1.0) continue;
                            acc(y, x) = std::max(acc(y, x),
                                                 static_cast<float>(amp * (1.0 - rd)));
                        }
                }
            }
        }
        }   // end Crying droplet branch

        for (int y = 0; y < h; ++y) {
            cv::Vec3b*   row = out.ptr<cv::Vec3b>(y);
            const float* a   = acc.ptr<float>(y);
            for (int x = 0; x < w; ++x) row[x] = paint(p, a[x]);
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
                // Monitor-style ECG: a sweep head redraws the trace left →
                // right each pass, hospital-EKG style — bright pen tip, an
                // erase gap just ahead of the head, and the older trace
                // dimming until the next pass overwrites it. ~2.5 complexes
                // fit across the panel at Size 1.0; Size trades amplitude
                // against density (smaller = more, smaller beats).
                const double beats = std::clamp(4.0 / sz, 1.0, 10.0);
                const double tau   = t * 0.45 * sp;      // sweeps elapsed
                auto bump = [](double uu, double c, double wd, double amp) {
                    const double d = (uu - c) / wd;
                    return amp * std::exp(-d * d * 4.0);
                };
                // Trace height at a column, sampled at the sweep-time when
                // the head last drew it (each pass shows the signal's NEXT
                // stretch, like a real monitor). False when the column
                // hasn't been drawn yet (first pass) or sits in the erase
                // gap ahead of the head.
                auto trace = [&](int px, double& ty, double& age) -> bool {
                    const double fx   = px / static_cast<double>(std::max(1, w));
                    const double taux = std::floor(tau - fx) + fx;
                    if (taux < 0.0) return false;        // not drawn yet
                    age = tau - taux;                    // sweeps since drawn
                    if (age > 0.94) return false;        // erase gap
                    const double ph = beats * taux;
                    const double u  = ph - std::floor(ph);
                    const double wv = bump(u, 0.18, 0.030, 0.18)     // P
                                    - bump(u, 0.28, 0.014, 0.16)     // Q
                                    + bump(u, 0.31, 0.016, 0.95)     // R
                                    - bump(u, 0.345, 0.016, 0.28)    // S
                                    + bump(u, 0.50, 0.050, 0.22);    // T
                    ty = cy - wv * scale * 0.9 * sz;
                    return true;
                };
                // Span this column's and the next column's trace heights so
                // steep segments (the QRS spike) stay a connected line
                // instead of aliasing into detached dots.
                double ty0 = 0.0, ty1 = 0.0, age0 = 0.0, age1 = 0.0;
                if (!trace(x, ty0, age0)) break;
                if (!trace(x + 1, ty1, age1)) ty1 = ty0;
                const double lo = std::min(ty0, ty1);
                const double hi = std::max(ty0, ty1);
                // Hard single-pixel stroke: snap the trace span to pixel
                // rows and light exactly those, full intensity — no soft
                // edges to smear the line across two rows (the default
                // centre puts the baseline exactly between rows, which a
                // soft stroke renders 2 px thick forever). The trail only
                // ages to ~70% before the erase gap catches up to it.
                const int lo_r = static_cast<int>(std::lround(lo));
                const int hi_r = static_cast<int>(std::lround(hi));
                if (y >= lo_r && y <= hi_r)
                    inten = (age0 < 0.02) ? 1.0 : 0.99 - 0.31 * age0;
                break;
            }
            default: break;
            }
            row[x] = paint(p, inten);
        }
    }
    return out;
}

cv::Mat render_eye_animation(const EyeAnimParams& p, double t, int w, int h,
                             const EyeLidLine* lid, int lid_x0, float gravity_deg) {
    // The compositor consumes RGBA face layers (composite() splits out the
    // alpha channel); the animation owns the whole panel, so it converts to
    // fully opaque RGBA here rather than teaching every draw loop about alpha.
    // (p.mirror is handled by the CALLER — it renders one half-width copy via
    // this function and composites left + mirrored right.)
    cv::Mat rgba;
    cv::cvtColor(render_rgb(p, t, w, h, lid, lid_x0, gravity_deg), rgba,
                 cv::COLOR_RGB2RGBA);
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
    case EyeAnim::Crying:    return "Crying";
    case EyeAnim::Waterfall: return "Waterfall";
    default:                 return "?";
    }
}

int eye_anim_count() { return static_cast<int>(EyeAnim::Count); }

} // namespace face

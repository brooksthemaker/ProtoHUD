#include "particles.h"

#include <opencv2/imgproc.hpp>   // cv::line / cv::circle for line-based effects

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace face {
namespace {

using json = nlohmann::json;

constexpr double kTau = 6.283185307179586;
constexpr double kPi  = 3.14159265358979323846;

// ── json/random helpers ──────────────────────────────────────────────────────

double jnum(const json& j, const char* k, double d) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<double>() : d;
}
int jint(const json& j, const char* k, int d) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? static_cast<int>(it->get<double>()) : d;
}
bool has_colors(const json& cfg) { return cfg.contains("colors") || cfg.contains("color"); }

double frand(std::mt19937& rng, double lo, double hi) {
    std::uniform_real_distribution<double> d(lo, hi);
    return d(rng);
}
int irand(std::mt19937& rng, int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    std::uniform_int_distribution<int> d(lo, hi);
    return d(rng);
}

struct Color { int r = 255, g = 255, b = 255; };

// Linear multi-stop gradient across a color list, f in [0,1] (water depth
// shading, frost density shading).
Color sample_grad(const std::vector<Color>& v, double f) {
    if (v.size() == 1) return v[0];
    f = std::clamp(f, 0.0, 1.0);
    const double pos = f * (v.size() - 1);
    const int i = (int)pos;
    if (i >= (int)v.size() - 1) return v.back();
    const double t = pos - i;
    return { (int)std::lround(v[i].r + (v[i+1].r - v[i].r) * t),
             (int)std::lround(v[i].g + (v[i+1].g - v[i].g) * t),
             (int)std::lround(v[i].b + (v[i+1].b - v[i].b) * t) };
}

Color pick_color(const json& cfg, std::mt19937& rng) {
    auto it = cfg.find("colors");
    if (it != cfg.end() && it->is_array() && !it->empty()) {
        const json& c = (*it)[irand(rng, 0, static_cast<int>(it->size()) - 1)];
        return { c[0].get<int>(), c[1].get<int>(), c[2].get<int>() };
    }
    auto c = cfg.find("color");
    if (c != cfg.end() && c->is_array() && c->size() >= 3)
        return { (*c)[0].get<int>(), (*c)[1].get<int>(), (*c)[2].get<int>() };
    return { 255, 255, 255 };
}

double pick_speed(const json& cfg, double dmin, double dmax, std::mt19937& rng) {
    double lo = jnum(cfg, "speed_min", jnum(cfg, "speed", dmin));
    double hi = jnum(cfg, "speed_max", jnum(cfg, "speed", dmax));
    return frand(rng, lo, hi);
}
double pick_life(const json& cfg, double dmin, double dmax, std::mt19937& rng) {
    return frand(rng, jnum(cfg, "life_min", dmin), jnum(cfg, "life_max", dmax));
}
int pick_size(const json& cfg, int dmin, int dmax, std::mt19937& rng) {
    return irand(rng, jint(cfg, "size_min", dmin), jint(cfg, "size_max", dmax));
}

// emit position helper (mirror _emit_pos)
void emit_pos(const json& cfg, int w, int h, const char* def,
              std::mt19937& rng, double& ox, double& oy) {
    std::string where = cfg.value("emit_from", std::string(def));
    if (where == "bottom")      { ox = frand(rng, 0, w - 1); oy = h; }
    else if (where == "top")    { ox = frand(rng, 0, w - 1); oy = -2.0; }
    else if (where == "edges") {
        int side = irand(rng, 0, 3);
        if (side == 0)      { ox = frand(rng, 0, w - 1); oy = -2.0; }
        else if (side == 1) { ox = frand(rng, 0, w - 1); oy = h; }
        else if (side == 2) { ox = -2.0; oy = frand(rng, 0, h - 1); }
        else                { ox = w;    oy = frand(rng, 0, h - 1); }
    } else { ox = frand(rng, 0, w - 1); oy = frand(rng, 0, h - 1); }
}

// ── Particle + drawing ───────────────────────────────────────────────────────

struct Particle {
    double x = 0, y = 0, vx = 0, vy = 0;
    double life = 1.0, max_life = 1.0;
    double r = 255, g = 255, b = 255;
    int    size = 1;
    double extra = 0.0;
};

inline void draw_pixel(cv::Mat& c, int x, int y, int r, int g, int b, int a) {
    if (x < 0 || x >= c.cols || y < 0 || y >= c.rows) return;
    cv::Vec4b& px = c.at<cv::Vec4b>(y, x);
    px[0] = cv::saturate_cast<uchar>(r);
    px[1] = cv::saturate_cast<uchar>(g);
    px[2] = cv::saturate_cast<uchar>(b);
    px[3] = cv::saturate_cast<uchar>(a);
}
inline void draw_dot(cv::Mat& c, double x, double y, int r, int g, int b,
                     double alpha, int size) {
    int a = static_cast<int>(std::clamp(alpha * 255.0, 0.0, 255.0));
    if (size <= 1) { draw_pixel(c, (int)x, (int)y, r, g, b, a); return; }
    int ix = (int)x, iy = (int)y;
    for (int dy = -size + 1; dy < size; ++dy)
        for (int dx = -size + 1; dx < size; ++dx)
            draw_pixel(c, ix + dx, iy + dy, r, g, b, a);
}
inline void draw_rect(cv::Mat& c, double x, double y, int r, int g, int b,
                      double alpha, int size) {
    int a = static_cast<int>(std::clamp(alpha * 255.0, 0.0, 255.0));
    int ix = (int)x, iy = (int)y;
    draw_pixel(c, ix, iy, r, g, b, a);
    draw_pixel(c, ix, iy + 1, r, g, b, a);
    if (size > 1) {
        draw_pixel(c, ix + 1, iy, r, g, b, a);
        draw_pixel(c, ix + 1, iy + 1, r, g, b, a);
    }
}

// Tapered anti-aliased streak: from the head at (x,y) back along -(ux,uy) for
// len px, fading to nothing at the tail. Used by shooting stars (and any
// future streaking effect) so tails match the vortex's streamline look.
inline void draw_streak(cv::Mat& c, double x, double y, double ux, double uy,
                        double len, double r, double g, double b,
                        double a_head) {
    const int N = std::max(3, (int)std::lround(len / 2.0));
    double px = x, py = y;
    for (int k = 1; k <= N; ++k) {
        // Midpoint taper: alpha for the segment's middle, not its far end —
        // an end-based taper zeroed the last segment and left short tails
        // nearly invisible.
        const double t  = (static_cast<double>(k) - 0.5) / N;
        const double qx = x - ux * len * t, qy = y - uy * len * t;
        const int A = (int)std::clamp(a_head * std::pow(1.0 - t, 1.3) * 255.0,
                                      0.0, 255.0);
        if (A > 2)
            cv::line(c, {(int)std::lround(px), (int)std::lround(py)},
                        {(int)std::lround(qx), (int)std::lround(qy)},
                     cv::Scalar(r, g, b, A), 1, cv::LINE_AA);
        px = qx; py = qy;
    }
}

// A six-armed snowflake: three spokes through the centre plus short side
// branches, at radius R and rotation `rot`. Sharp (per-pixel) so it keeps the
// blocky pixel look. `a` is 0..1 coverage.
inline void draw_snowflake(cv::Mat& c, double cx, double cy, double R,
                           double rot, int r, int g, int b, double a) {
    const int col_a = (int)std::clamp(a * 255.0, 0.0, 255.0);
    if (col_a <= 0) return;
    draw_pixel(c, (int)std::lround(cx), (int)std::lround(cy), r, g, b, col_a);
    for (int arm = 0; arm < 6; ++arm) {
        const double ang = rot + arm * (kTau / 6.0);
        const double ux = std::cos(ang), uy = std::sin(ang);
        for (double d = 1.0; d <= R; d += 1.0) {
            const double px = cx + ux * d, py = cy + uy * d;
            const double fade = a * (1.0 - 0.4 * (d / std::max(1.0, R)));
            draw_pixel(c, (int)std::lround(px), (int)std::lround(py),
                       r, g, b, (int)std::clamp(fade * 255.0, 0.0, 255.0));
            // Little branch pips two-thirds out, giving the dendritic look.
            if (d >= R * 0.6 && d <= R * 0.7) {
                const double bx = -uy, by = ux;   // perpendicular
                for (int s = -1; s <= 1; s += 2)
                    draw_pixel(c, (int)std::lround(px + bx * s),
                               (int)std::lround(py + by * s), r, g, b,
                               (int)std::clamp(fade * 0.8 * 255.0, 0.0, 255.0));
            }
        }
    }
}

// ── Base effect ──────────────────────────────────────────────────────────────

class BaseEffect {
public:
    BaseEffect(int w, int h, json cfg)
        : w_(w), h_(h), cw_(w), ch_(h), cfg_(std::move(cfg)),
          intensity_(jnum(cfg_, "intensity", 1.0)),
          rng_(static_cast<uint32_t>(
              std::chrono::steady_clock::now().time_since_epoch().count())) {
        shape_rect_ = cfg_.value("shape", std::string("dot")) == "rect";
    }
    virtual ~BaseEffect() = default;
    virtual void update(double dt) = 0;
    virtual cv::Mat render() = 0;   // CV_8UC4
    // "Refraction" hint: how strongly the backdrop face should glow back through
    // this layer (water overrides). 0 = opaque overlay like every other effect.
    virtual double face_glow() const { return 0.0; }
    // Face-tint request (frost overrides): a panel-local CV_8UC1 mask filled
    // during render(), or nullptr when the effect doesn't tint. r/g/b is the
    // tint colour. The mask shares a scratch buffer consumed the same tick.
    virtual const cv::Mat* face_tint(uint8_t&, uint8_t&, uint8_t&) const {
        return nullptr;
    }

    // Latest IMU / audio state, pushed each frame by the owning ParticleSystem.
    // Effects read it through count()/direction_unit() when the cfg opts in.
    void set_motion(const MotionInput& m) { motion_ = m; }
    void set_audio(double level) { audio_ = level; }
    // Relative humidity 0..1 (or <0 = no reading), pushed each frame. The water
    // effect reads it when the layer opts in with "level_from":"humidity".
    void set_humidity(double humidity01) { humidity_ = humidity01; }
    // Ambient signals for density reactivity. lux < 0 and temp <= kNoTemp mean
    // "no sensor" and contribute nothing rather than reading as zero/cold.
    void set_ambient(double lux, double temp_c, double day_frac) {
        lux_ = lux; temp_c_ = temp_c; day_frac_ = day_frac;
    }
    static constexpr double kNoTemp = -1000.0;
    // Global default for direction coupling (see direction_unit): layers with
    // no explicit "direction_from" behave as "gravity" while this is on.
    void set_motion_reactive(bool on) { motion_reactive_ = on; }
    // Swap in new params without recreating the effect — keeps the live particle
    // sim (and motion/audio/canvas state) so menu edits preview without resetting.
    void set_cfg(json cfg) {
        cfg_ = std::move(cfg);
        intensity_  = jnum(cfg_, "intensity", 1.0);
        shape_rect_ = cfg_.value("shape", std::string("dot")) == "rect";
    }
    void set_canvas_geometry(int cw, int ch, int ox, int oy) {
        cw_ = cw; ch_ = ch; ox_ = ox; oy_ = oy;
    }

protected:
    int count(int def) const {
        double scale = intensity_;
        const std::string from = cfg_.value("intensity_from", std::string("none"));
        if (from != "none") {
            const double gain = jnum(cfg_, "motion_gain", 1.0);
            if (from == "yaw_rate")
                scale *= 1.0 + std::min(std::fabs(motion_.yaw_rate) / 90.0, 3.0) * gain;
            else if (from == "accel")
                scale *= 1.0 + std::min(std::fabs(motion_.accel_g - 1.0) * 4.0, 3.0) * gain;
            else if (from == "audio")
                scale *= 1.0 + std::min(audio_, 1.0) * 3.0 * gain;
            // Ambient sources. Each is normalised to 0..1 where 1 = densest,
            // and offered in BOTH directions so a layer can thicken in the
            // dark or in the light without needing a separate invert control.
            else if (from == "light" || from == "dark") {
                if (lux_ >= 0.0) {
                    // 800 lx ~ bright indoors; the same reference the trigger
                    // recipes use for their light conditions.
                    const double lit = std::clamp(lux_ / 800.0, 0.0, 1.0);
                    const double v = (from == "light") ? lit : (1.0 - lit);
                    scale *= 1.0 + v * 3.0 * gain;
                }
            } else if (from == "warm" || from == "cold") {
                if (temp_c_ > kNoTemp) {
                    // 18 °C is the neutral point; ±15 °C reaches full effect.
                    const double warm = std::clamp((temp_c_ - 18.0) / 15.0, 0.0, 1.0);
                    const double cold = std::clamp((18.0 - temp_c_) / 15.0, 0.0, 1.0);
                    scale *= 1.0 + ((from == "warm") ? warm : cold) * 3.0 * gain;
                }
            } else if (from == "night" || from == "day") {
                // Smooth cosine over the day rather than a hard switch, so an
                // effect thickens toward its hour instead of popping at one.
                const double night = 0.5 * (1.0 + std::cos(2.0 * M_PI * day_frac_));
                const double v = (from == "night") ? night : (1.0 - night);
                scale *= 1.0 + v * 3.0 * gain;
            }
        }
        return std::max(1, static_cast<int>(jnum(cfg_, "count", def) * scale));
    }
    void draw_particle(cv::Mat& c, const Particle& p, double alpha) {
        if (shape_rect_) draw_rect(c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, alpha, p.size);
        else             draw_dot (c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, alpha, p.size);
    }
    // Reusable per-effect canvas: render() consumes the returned frame within
    // the same tick, so handing out a shallow header over one persistent
    // buffer avoids an alloc + zero-fill per layer per frame.
    cv::Mat blank() {
        if (scratch_.rows != h_ || scratch_.cols != w_ || scratch_.type() != CV_8UC4)
            scratch_.create(h_, w_, CV_8UC4);
        scratch_.setTo(cv::Scalar(0, 0, 0, 0));
        return scratch_;
    }
    // Direction unit vector for directional effects (snow / rain / embers /
    // confetti / clouds). Convention: 0° = right, 90° = down, 180° = left,
    // 270° = up (screen-space, +Y down). default_deg lets each effect keep
    // its historical motion direction when no "direction_deg" override is set.
    void direction_unit(double& dx, double& dy, double default_deg) const {
        double deg = jnum(cfg_, "direction_deg", default_deg);
        // Motion coupling (opt-in): "heading" locks the angle to the compass,
        // "yaw" drifts it as you turn, "tilt" skews it like gravity when you
        // roll your head, "gravity" combines lean + turn-sweep (and is the
        // default for every directional layer while the global Motion Reactive
        // toggle is on). "motion_gain" scales the response.
        const std::string from = cfg_.value("direction_from",
            std::string(motion_reactive_ ? "gravity" : "none"));
        if (from != "none") {
            const double gain = jnum(cfg_, "motion_gain", 1.0);
            if (from == "heading")   deg  = motion_.heading_deg;
            else if (from == "yaw")  deg += motion_.yaw_rate * 0.5 * gain;
            else if (from == "tilt") deg += motion_.roll_deg * 2.0 * gain;
            else if (from == "gravity") {
                // Real-gravity feel: rain/snow lean with head roll and get
                // swept sideways by fast turns, clamped so precipitation
                // never flows uphill.
                const double delta = (motion_.roll_deg * 1.6 +
                                      motion_.yaw_rate * 0.35) * gain;
                deg += std::clamp(delta, -75.0, 75.0);
            }
        }
        deg += jnum(cfg_, "direction_offset_deg", 0.0);
        const double rad = deg * kPi / 180.0;
        dx = std::cos(rad);
        dy = std::sin(rad);
    }
    // Spawn a particle at the trailing edge for the layer's direction. With
    // historical defaults (down for snow/rain/confetti, up for embers) this
    // yields the original top/bottom spawn lines; for a sideways or diagonal
    // angle it slides the spawn point onto the leading-edge mirror so the
    // particle enters the canvas instead of starting mid-frame. `margin`
    // controls the random distance offscreen along the motion axis (gives
    // the original snow/rain/confetti staggered entry).
    void direction_spawn_point(double margin, double default_deg,
                               double& sx, double& sy) {
        double dx, dy;
        direction_unit(dx, dy, default_deg);
        if (std::fabs(dx) >= std::fabs(dy)) {
            sx = (dx >= 0) ? frand(rng_, -margin, 0.0)
                           : frand(rng_, static_cast<double>(w_),
                                   static_cast<double>(w_) + margin);
            sy = frand(rng_, 0.0, static_cast<double>(h_ - 1));
        } else {
            sy = (dy >= 0) ? frand(rng_, -margin, 0.0)
                           : frand(rng_, static_cast<double>(h_),
                                   static_cast<double>(h_) + margin);
            sx = frand(rng_, 0.0, static_cast<double>(w_ - 1));
        }
    }

    int w_, h_;
    int cw_, ch_, ox_ = 0, oy_ = 0;   // full-canvas size + this panel's offset
    json cfg_;
    double intensity_;
    std::mt19937 rng_;
    std::vector<Particle> particles_;
    MotionInput motion_{};
    double audio_ = 0.0;
    double lux_      = -1.0;        // <0 = no light sensor
    double temp_c_   = kNoTemp;     // ambient °C
    double day_frac_ = 0.5;         // 0 = midnight, 0.5 = noon
    double humidity_ = -1.0;          // rel humidity 0..1; <0 = no sensor reading
    bool motion_reactive_ = false;
    bool shape_rect_ = false;         // "shape" resolved once per cfg change
    cv::Mat scratch_;                 // blank() backing buffer, reused each frame
};

// ── Sparkle ──────────────────────────────────────────────────────────────────

class SparkleEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        std::vector<Particle> alive;
        for (auto& p : particles_) { p.life -= dt / p.max_life; if (p.life > 0) alive.push_back(p); }
        particles_ = std::move(alive);
        while ((int)particles_.size() < count(40)) {
            double ml = pick_life(cfg_, 0.08, 0.35, rng_);
            Color col = pick_color(cfg_, rng_);
            double x, y; emit_pos(cfg_, w_, h_, "random", rng_, x, y);
            Particle p; p.x = x; p.y = y; p.life = 1; p.max_life = ml;
            p.r = col.r; p.g = col.g; p.b = col.b; p.size = pick_size(cfg_, 1, 1, rng_);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) {
            const double env = std::sin(p.life * kPi);
            draw_particle(c, p, env);
            // 4-point glint: short cross arms flash for the brightest couple
            // of frames of each blink — the classic sparkle read.
            if (env > 0.90) {
                const double gl  = (env - 0.90) / 0.10;
                const int    arm = (p.size > 1) ? 2 : 1;
                for (int i = 1; i <= arm; ++i) {
                    const int A = (int)(gl * 220.0) / i;
                    draw_pixel(c, (int)p.x + i, (int)p.y, (int)p.r, (int)p.g, (int)p.b, A);
                    draw_pixel(c, (int)p.x - i, (int)p.y, (int)p.r, (int)p.g, (int)p.b, A);
                    draw_pixel(c, (int)p.x, (int)p.y + i, (int)p.r, (int)p.g, (int)p.b, A);
                    draw_pixel(c, (int)p.x, (int)p.y - i, (int)p.r, (int)p.g, (int)p.b, A);
                }
            }
        }
        return c;
    }
};

// ── Snow ─────────────────────────────────────────────────────────────────────

class SnowEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    // Depth 0 (far) … 1 (near), inferred from where the flake's speed sits in
    // the configured range — no extra storage, and speed/brightness/wobble
    // stay coupled the way real parallax couples them.
    double depth_of(double spd) const {
        const double lo = jnum(cfg_, "speed_min", 6.0);
        const double hi = jnum(cfg_, "speed_max", 12.0);
        return (hi > lo) ? std::clamp((spd - lo) / (hi - lo), 0.0, 1.0) : 1.0;
    }
    void update(double dt) override {
        double drift_x = jnum(cfg_, "drift_x", 1.5);
        double dx, dy; direction_unit(dx, dy, 90.0);   // default down
        // Flakes landing on the bottom edge sit there this long (±40%) before
        // melting away. 0 turns settling off. Downward snow only.
        const double settle_s = jnum(cfg_, "settle_s", 2.0);
        const bool   downish  = dy > 0.5;
        for (auto& p : particles_) {
            if (p.extra < 0) { p.life -= dt / p.max_life; continue; }   // melting
            const double spd = (p.extra == 0) ? pick_speed(cfg_, 6, 12, rng_) : p.extra;
            p.extra = spd;
            const double d = depth_of(spd);
            // Near flakes wobble wider than far ones — same parallax coupling.
            p.vx = drift_x * (0.55 + 0.9 * d) * std::sin(p.vx + p.y * 0.3);
            p.x += (spd * dx + p.vx) * dt;
            const double py0 = p.y;
            p.y += spd * dy * dt;
            p.life -= dt / p.max_life;
            if (settle_s > 0 && downish && py0 < h_ - 1 && p.y >= h_ - 1) {
                p.extra = -1.0; p.y = h_ - 1;          // settle, then melt
                p.life = 1.0;
                p.max_life = frand(rng_, settle_s * 0.6, settle_s * 1.4);
            }
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return !(p.x > -4 && p.x < w_ + 4 && p.y > -4 && p.y < h_ + 4 && p.life > 0);
            }), particles_.end());
        // Cold start (effect just switched on): scatter the first fill across
        // the whole panel so it's already snowing, instead of one cohort
        // marching down from the top edge in a band.
        const bool cold = particles_.empty();
        while ((int)particles_.size() < count(30)) {
            double spd = pick_speed(cfg_, 6, 12, rng_);
            // Life defaults to the crossing time (the old fixed 1.5-4 s killed
            // most flakes mid-air); explicit cfg life_min/max still override.
            const double cross = (h_ + 8.0) / std::max(1.0, spd);
            double ml  = pick_life(cfg_, cross * 1.1, cross * 1.5, rng_);
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{200, 220, 255};
            Particle p;
            direction_spawn_point(2.0, 90.0, p.x, p.y);   // 90° = historical down
            if (cold) { p.x = frand(rng_, 0, w_ - 1.0); p.y = frand(rng_, 0, h_ - 2.0); }
            p.vx = frand(rng_, 0, kTau); p.max_life = ml; p.life = 1;
            p.r = col.r; p.g = col.g; p.b = col.b; p.size = pick_size(cfg_, 1, 1, rng_); p.extra = spd;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) {
            if (p.extra < 0) {                          // settled: melt in place
                draw_particle(c, p, std::clamp(p.life, 0.0, 1.0) * 0.85);
                continue;
            }
            // Depth-graded brightness — far flakes dim, near flakes bright —
            // instead of the old flat 0.9 across the board.
            draw_particle(c, p, 0.40 + 0.55 * depth_of(p.extra));
        }
        return c;
    }
};

// ── Embers ───────────────────────────────────────────────────────────────────

class EmbersEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double spread  = jnum(cfg_, "spread", 0.4);
        double drift_x = jnum(cfg_, "drift_x", 0.0);
        double dx, dy; direction_unit(dx, dy, 270.0);  // default up
        for (auto& p : particles_) {
            p.extra += dt * 3.0;
            p.vx = spread * std::sin(p.extra + p.y * 0.2) * p.vy + drift_x;
            p.x += (p.vy * dx + p.vx) * dt;
            p.y += p.vy * dy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return !(p.x > -4 && p.x < w_ + 4 && p.y > -4 && p.y < h_ + 4 && p.life > 0);
            }), particles_.end());
        while ((int)particles_.size() < count(25)) {
            double spd = pick_speed(cfg_, 8, 22, rng_);
            double ml  = pick_life(cfg_, 0.8, 2.5, rng_);
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{255, 120, 20};
            Particle p;
            direction_spawn_point(0.0, 270.0, p.x, p.y);  // 270° = historical up (spawn at bottom)
            p.vy = spd;
            p.max_life = ml; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        // Embers render as little angled diamonds (45°) with a flicker spark
        // above, which reads as natural fire far better than axis-aligned blocks.
        for (auto& p : particles_) {
            const double alpha = p.life;
            const int gr = (int)std::clamp(p.r, 0.0, 255.0);
            const int gg = (int)std::clamp(p.g * p.life, 0.0, 255.0);
            const int gb = (int)std::clamp(p.b * p.life * p.life, 0.0, 255.0);
            const int A  = (int)std::clamp(alpha * 255.0, 0.0, 255.0);
            const int rad = std::max(0, p.size - 1);          // size 1 = single pixel
            const int ix = (int)p.x, iy = (int)p.y;
            for (int dy = -rad; dy <= rad; ++dy)
                for (int dx = -rad; dx <= rad; ++dx)
                    if (std::abs(dx) + std::abs(dy) <= rad)    // diamond, not square
                        draw_pixel(c, ix + dx, iy + dy, gr, gg, gb, A);
            // flickering spark just above the ember (wobbles with its phase)
            const int fx = (int)std::lround(std::sin(p.extra * 3.0) * 1.5);
            draw_pixel(c, ix + fx, iy - rad - 1, 255, 230, 180, (int)(alpha * 200.0));
        }
        return c;
    }
};

// ── Flames ───────────────────────────────────────────────────────────────────
// A real flame body, not particles: the classic heat-field fire. Two fuel rows
// below the panel burn with a smoothed per-column flicker; each 30 Hz substep
// every cell pulls heat from the row beneath (with random sideways jitter, so
// tongues lick and split) minus a random cooling term sized so flames die out
// at ~"height" of the panel. Heat maps through a black → deep red → orange →
// yellow → white palette; cfg "colors" replaces the mid-stops for green/blue
// fire. Transparent above the flame body, so the face shows through.
//   height (0..1, default 0.5) · turbulence (jitter chance, default 0.55)
//   · colors (optional palette override, cool → hot)
class FlamesEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        acc_ += std::min(dt, 0.1);
        age_ += dt;
        // Burn-in: ignition runs at full 30 Hz while the flames first climb
        // (one row per substep -> h_/30 s per full rise), then the burn eases
        // down to "settle_hz" over a second - a fire that flares up and
        // settles into a calm steady flicker.
        const double ignite_s = (h_ / 30.0) * 1.2;
        const double settle   = std::clamp(jnum(cfg_, "settle_hz", 16.0), 4.0, 30.0);
        const double k        = std::clamp((age_ - ignite_s) / 1.0, 0.0, 1.0);
        const double step     = 1.0 / (30.0 + (settle - 30.0) * k);
        for (int n = 0; acc_ >= step && n < 3; ++n) { acc_ -= step; tick(); }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        if (heat_.empty()) return c;
        ensure_palette();
        for (int y = 0; y < h_; ++y) {
            const float* hr = heat_.ptr<float>(y);
            cv::Vec4b*   px = c.ptr<cv::Vec4b>(y);
            for (int x = 0; x < w_; ++x) {
                const float v = std::clamp(hr[x], 0.f, 1.f);
                if (v <= 0.06f) continue;
                // Exposure shaping: LEDs clip the hot end hard, so bias the
                // body into the red/orange range — white stays a thin core.
                // The 0.18 floor keeps detached TIP points in the vivid-red
                // band so they read as distinct glowing licks, not the ramp's
                // dimmest shades.
                const float t = 0.18f +
                    0.82f * std::pow((v - 0.06f) / 0.94f, 1.25f);
                const int   i = std::min(int(t * (kPalN - 1)), kPalN - 2);
                const float f = t * (kPalN - 1) - i;
                px[x][0] = (uchar)(pal_[i][0] + (pal_[i + 1][0] - pal_[i][0]) * f);
                px[x][1] = (uchar)(pal_[i][1] + (pal_[i + 1][1] - pal_[i][1]) * f);
                px[x][2] = (uchar)(pal_[i][2] + (pal_[i + 1][2] - pal_[i][2]) * f);
                px[x][3] = (uchar)(std::clamp(v * 3.f, 0.f, 1.f) * 255.f);
            }
        }
        return c;
    }

private:
    void tick() {
        if (heat_.empty()) {
            heat_ = cv::Mat::zeros(h_ + 2, w_, CV_32F);   // +2 fuel rows below
            fuel_.assign(static_cast<size_t>(w_), 1.f);
        }
        const double height = std::clamp(jnum(cfg_, "height", 0.5), 0.1, 1.0);
        const double turb   = std::clamp(jnum(cfg_, "turbulence", 0.55), 0.0, 1.0);
        // Average cooling per row such that full heat (1.0) burns down to the
        // visibility floor across height*h_ rows of rise.
        const float cool = static_cast<float>(0.80 / (height * h_));
        // Fuel flicker: smoothed random per column, with occasional dips so
        // the base isn't a solid bar and tongues form above the hot spots.
        for (int x = 0; x < w_; ++x) {
            float target = frand(rng_, 0.35f, 1.25f);
            if (frand(rng_, 0.f, 1.f) < 0.05) target = 0.05f;   // brief gap
            fuel_[x] += (target - fuel_[x]) * 0.25f;
        }
        // Spatial smoothing: hotspots span several columns, so the base burns
        // as distinct tongues instead of per-pixel noise.
        for (int x = 1; x < w_ - 1; ++x)
            fuel_[x] = (fuel_[x - 1] + 2.f * fuel_[x] + fuel_[x + 1]) * 0.25f;
        for (int y = h_; y < h_ + 2; ++y) {
            float* row = heat_.ptr<float>(y);
            for (int x = 0; x < w_; ++x)
                row[x] = fuel_[x] * static_cast<float>(intensity_);
        }
        // Rise: each cell pulls from the row beneath with sideways jitter and
        // random cooling — the jitter is what makes tongues lick and split.
        for (int y = 0; y < h_; ++y) {
            float*       dst = heat_.ptr<float>(y);
            const float* src = heat_.ptr<float>(y + 1);
            for (int x = 0; x < w_; ++x) {
                int sx = x;
                if (frand(rng_, 0.f, 1.f) < turb)
                    sx = std::clamp(x + irand(rng_, -1, 1), 0, w_ - 1);
                // Body: 3-tap pull from below keeps the hot core coherent.
                // Tips (cool cells): single-source pull with a WIDER waver, so
                // detached points stay crisp and flicker sideways instead of
                // melting into mist.
                const int   l = std::max(0, sx - 1), r = std::min(w_ - 1, sx + 1);
                float pulled = src[sx];
                if (pulled > 0.45f)
                    pulled = (src[l] + 6.f * src[sx] + src[r]) * 0.125f;
                else if (frand(rng_, 0.f, 1.f) < turb)
                    pulled = src[std::clamp(x + irand(rng_, -2, 2), 0, w_ - 1)];
                // Cooling: a smooth burn plus occasional LARGE bites — the
                // bites carve notches that pinch tongues off into free-rising
                // points, which is what makes tips break like real flames.
                float cbite = static_cast<float>(frand(rng_, 0.0, 2.0 * cool));
                if (frand(rng_, 0.f, 1.f) < 0.10f)
                    cbite += static_cast<float>(frand(rng_, 3.0, 9.0)) * cool;
                dst[x] = std::max(0.f, pulled - cbite);
            }
        }
    }
    void ensure_palette() {
        // Rebuild whenever the layer's colours change — set_cfg swaps params
        // in live (keeping the running sim), so a build-once palette made the
        // builder's colour edits do nothing.
        const json cur = cfg_.contains("colors") ? cfg_["colors"] : json();
        if (pal_built_ && cur == pal_src_) return;
        pal_built_ = true;
        pal_src_   = cur;
        // Default: classic blackbody ramp. cfg colors (cool → hot) replace the
        // mid-stops; the ends stay near-black / near-white so it still reads
        // as burning rather than a flat tint.
        // LED palette: saturated hues at high brightness — the hue ramp
        // (red → orange → yellow) carries the heat read. Dark "realistic"
        // shades quantise to nothing on dimmed 6-bit panels (they showed as
        // white-only wisps under Auto Dim).
        std::vector<std::array<int, 3>> stops = {
            {150, 0, 0},   {220, 20, 0},   {255, 60, 0},
            {255, 120, 0}, {255, 180, 20}, {255, 235, 120} };
        if (has_colors(cfg_) && cfg_["colors"].is_array()) {
            std::vector<std::array<int, 3>> mid;
            for (const auto& jc : cfg_["colors"])
                if (jc.is_array() && jc.size() == 3)
                    mid.push_back({ jc[0].get<int>(), jc[1].get<int>(),
                                    jc[2].get<int>() });
            if (!mid.empty()) {
                stops.clear();
                // Bottom stop = a dimmed cut of the coolest colour, not
                // near-black — dark shades vanish on dimmed panels.
                stops.push_back({ mid[0][0] * 11 / 20, mid[0][1] * 11 / 20,
                                  mid[0][2] * 11 / 20 });
                for (auto& m : mid) stops.push_back(m);
                const auto& lastc = mid.back();
                stops.push_back({ std::min(255, lastc[0] / 2 + 150),
                                  std::min(255, lastc[1] / 2 + 150),
                                  std::min(255, lastc[2] / 2 + 150) });
            }
        }
        for (int i = 0; i < kPalN; ++i) {
            const double t = static_cast<double>(i) / (kPalN - 1) *
                             (static_cast<double>(stops.size()) - 1.0);
            const int    s = std::min(static_cast<int>(t),
                                      static_cast<int>(stops.size()) - 2);
            const double f = t - s;
            for (int k = 0; k < 3; ++k)
                pal_[i][k] = static_cast<int>(std::lround(
                    stops[s][k] + (stops[s + 1][k] - stops[s][k]) * f));
        }
    }

    static constexpr int kPalN = 48;
    cv::Mat            heat_;
    std::vector<float> fuel_;
    double             acc_ = 0.0;
    double             age_ = 0.0;
    int                pal_[kPalN][3] = {};
    bool               pal_built_ = false;
    json               pal_src_;
};

// ── Confetti ─────────────────────────────────────────────────────────────────

class ConfettiEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double drift_x = jnum(cfg_, "drift_x", 0.0);
        double dx, dy; direction_unit(dx, dy, 90.0);  // default down
        const double tumble = jnum(cfg_, "tumble", 4.0);
        for (auto& p : particles_) {
            // Per-piece fall speed lives in vy — the old code rolled ONE random
            // speed per frame and applied it to every piece, so the whole
            // flock jittered in lockstep.
            p.vx = std::sin(p.extra) * 2.0 + drift_x;
            p.extra += dt * tumble;
            p.x += (p.vy * dx + p.vx) * dt;
            p.y += p.vy * dy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return !(p.x > -4 && p.x < w_ + 4 && p.y > -4 && p.y < h_ + 4 && p.life > 0);
            }), particles_.end());
        static const int kDef[][3] = {
            {255,50,50},{255,180,30},{50,220,50},{50,150,255},{220,50,220},{255,255,50}};
        const bool cold = particles_.empty();   // scatter the first fill (see snow)
        while ((int)particles_.size() < count(20)) {
            const double spd = pick_speed(cfg_, 4, 10, rng_);
            // Life defaults to the actual crossing time (the old fixed 2-5 s
            // killed most pieces mid-air on a 64 px panel); explicit cfg
            // life_min/max still override via pick_life.
            const double cross = (h_ + 8.0) / std::max(1.0, spd);
            double ml = pick_life(cfg_, cross * 1.1, cross * 1.5, rng_);
            Color col;
            if (has_colors(cfg_)) col = pick_color(cfg_, rng_);
            else { const int* d = kDef[irand(rng_, 0, 5)]; col = {d[0], d[1], d[2]}; }
            Particle p;
            direction_spawn_point(4.0, 90.0, p.x, p.y);   // 90° = historical down
            if (cold) { p.x = frand(rng_, 0, w_ - 1.0); p.y = frand(rng_, 0, h_ - 2.0); }
            p.vy = spd;
            p.max_life = ml; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        // Tumble flutter: each piece flips between face-on (a wide bright
        // pair) and edge-on (a thin dim sliver) as its phase turns — the flip
        // and the brightness dip together are what read as fluttering paper.
        for (auto& p : particles_) {
            const double life = std::clamp(p.life, 0.0, 1.0);
            const double edge = std::cos(p.extra);            // ±1 face-on … 0 edge-on
            const double a = std::min(1.0, life * 4.0) *
                             std::min(1.0, (1.0 - life) * 8.0 + 0.2) *
                             (0.30 + 0.70 * std::fabs(edge));
            const int A  = (int)std::clamp(a * 255.0, 0.0, 255.0);
            if (A <= 2) continue;
            const int ix = (int)p.x, iy = (int)p.y;
            draw_pixel(c, ix, iy, (int)p.r, (int)p.g, (int)p.b, A);
            if (std::fabs(edge) > 0.45) {                      // face-on: wide
                draw_pixel(c, ix + 1, iy, (int)p.r, (int)p.g, (int)p.b, A * 4 / 5);
                if (p.size > 1)
                    draw_pixel(c, ix - 1, iy, (int)p.r, (int)p.g, (int)p.b, A * 4 / 5);
            } else {                                           // edge-on: sliver
                draw_pixel(c, ix, iy + 1, (int)p.r, (int)p.g, (int)p.b, A * 3 / 5);
            }
        }
        return c;
    }
};

// ── Rings ────────────────────────────────────────────────────────────────────

class RingsEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double max_r = jnum(cfg_, "max_radius", 20);
        // Per-ring expansion speed (in vx) — one speed used to be rolled per
        // FRAME and applied to every ring, so they all jittered in lockstep
        // and each ring's fade drifted out of step with its radius.
        for (auto& p : particles_) { p.extra += p.vx * dt; p.life = std::max(0.0, 1.0 - p.extra / max_r); }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){ return !(p.life > 0); }), particles_.end());
        while ((int)particles_.size() < count(3)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{0, 200, 255};
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, 0, h_ - 1);
            p.vx = pick_speed(cfg_, 15, 25, rng_);
            p.max_life = max_r / p.vx; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b; p.extra = 0;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_)
            draw_circle(c, (int)p.x, (int)p.y, (int)p.extra, (int)p.r, (int)p.g, (int)p.b, p.life);
        return c;
    }
private:
    void draw_circle(cv::Mat& c, int cx, int cy, int radius, int r, int g, int b, double alpha) {
        if (radius <= 0) return;
        int a = (int)(alpha * 200);
        int x = radius, y = 0, err = 0;
        while (x >= y) {
            const int pts[8][2] = {{cx+x,cy+y},{cx-x,cy+y},{cx+x,cy-y},{cx-x,cy-y},
                                   {cx+y,cy+x},{cx-y,cy+x},{cx+y,cy-x},{cx-y,cy-x}};
            for (auto& pt : pts) draw_pixel(c, pt[0], pt[1], r, g, b, a);
            y += 1;
            if (err <= 0) err += 2 * y + 1;
            else { x -= 1; err += 2 * (y - x) + 1; }
        }
    }
};

// ── Rain ─────────────────────────────────────────────────────────────────────
// Velocity-aligned streaks with depth parallax: near drops are faster, longer
// and brighter than far ones. A drop reaching the bottom edge while the rain
// falls mostly downward flashes a one-tick splash where it lands.
// Particle.extra: depth 0 (far) … 1 (near); negative = a live splash.
class RainEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        int    length  = jint(cfg_, "length", 4);
        double drift_x = jnum(cfg_, "drift_x", 0.0);
        double dx, dy; direction_unit(dx, dy, 90.0);  // default down
        const bool downish = dy > 0.5;
        for (auto& p : particles_) {
            if (p.extra < 0) { p.life -= dt / p.max_life; continue; }   // splash
            p.x += (p.vy * dx + drift_x) * dt;
            const double py0 = p.y;
            p.y += p.vy * dy * dt;
            p.life -= dt / p.max_life;
            if (downish && py0 < h_ - 1 && p.y >= h_ - 1) {
                p.extra = -1.0; p.y = h_ - 1; p.vy = 0;
                p.life = 1.0; p.max_life = 0.14;
            }
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return p.life <= 0 ||
                       !(p.x > -length - 2 && p.x < w_ + length + 2 &&
                         p.y > -length - 2 && p.y < h_ + length + 2);
            }), particles_.end());
        const double lo = jnum(cfg_, "speed_min", 70.0);
        const double hi = jnum(cfg_, "speed_max", 120.0);
        const bool cold = particles_.empty();   // scatter the first fill (see snow)
        while ((int)particles_.size() < count(15)) {
            const double d   = frand(rng_, 0.0, 1.0);       // depth: 0 far … 1 near
            const double spd = lo + (hi - lo) * (0.25 + 0.75 * d);
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{100, 150, 255};
            Particle p;
            direction_spawn_point(static_cast<double>(length + 2), 90.0,
                                  p.x, p.y);              // 90° = historical down
            if (cold) { p.x = frand(rng_, 0, w_ - 1.0); p.y = frand(rng_, 0, h_ - 2.0); }
            p.vy = spd; p.max_life = (h_ + length) / spd * 2.0;
            p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = d;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        const int    length  = jint(cfg_, "length", 4);
        const double drift_x = jnum(cfg_, "drift_x", 0.0);
        double dx, dy; direction_unit(dx, dy, 90.0);
        for (auto& p : particles_) {
            if (p.extra < 0) {                            // splash: crown tick
                const int a = (int)std::clamp(p.life * 255.0, 0.0, 255.0);
                draw_pixel(c, (int)p.x - 1, (int)p.y,     (int)p.r, (int)p.g, (int)p.b, a * 3 / 5);
                draw_pixel(c, (int)p.x + 1, (int)p.y,     (int)p.r, (int)p.g, (int)p.b, a * 3 / 5);
                draw_pixel(c, (int)p.x,     (int)p.y - 1, 255, 255, 255, a * 3 / 4);
                continue;
            }
            // Streak along the drop's ACTUAL velocity (dir · speed + drift) so
            // angled rain slants with its motion — the old tail was hard-coded
            // straight down and ignored the direction slider entirely.
            const double vx = p.vy * dx + drift_x, vy = p.vy * dy;
            const double sp = std::hypot(vx, vy);
            if (sp < 1e-3) continue;
            const double d   = std::clamp(p.extra, 0.0, 1.0);
            const double len = length * (0.5 + 0.9 * d);
            const double a   = 0.35 + 0.6 * d;
            const double ux = vx / sp, uy = vy / sp;
            cv::line(c,
                     {(int)std::lround(p.x - ux * len), (int)std::lround(p.y - uy * len)},
                     {(int)std::lround(p.x), (int)std::lround(p.y)},
                     cv::Scalar(p.r, p.g, p.b, (int)(a * 235.0)),
                     std::max(1, p.size), cv::LINE_AA);
            draw_dot(c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b,
                     std::min(1.0, a + 0.25), p.size);
        }
        return c;
    }
};

// ── Steam ────────────────────────────────────────────────────────────────────
// Wisps rising from the snout (bottom-centre of the panel): spawned in a
// narrow band, drifting upward with a sideways waver, growing and fading as
// they cool. With motion-reactive gravity on, the plume leans as the head
// tilts. cfg: count, speed_min/max, spread_frac (emitter width as a fraction
// of the panel), wander (sideways waver px/s), colors.

class SteamEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const double wander = jnum(cfg_, "wander", 6.0);
        double dx, dy; direction_unit(dx, dy, 270.0);   // default straight up
        for (auto& p : particles_) {
            p.extra += dt;
            p.x += (p.vy * dx + std::sin(p.extra * 2.2 + p.vx) * wander) * dt;
            p.y += p.vy * dy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return p.life <= 0 || p.y < -4 || p.y > h_ + 4 ||
                       p.x < -4 || p.x > w_ + 4;
            }), particles_.end());
        while ((int)particles_.size() < count(14)) {
            const double spread = jnum(cfg_, "spread_frac", 0.20) * w_;
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_)
                                         : Color{205, 208, 215};
            Particle p;
            p.x = w_ * 0.5 + frand(rng_, -spread, spread);
            p.y = frand(rng_, h_ - 2.0, h_ + 2.0);        // the snout line
            p.vx = frand(rng_, 0, kTau);                  // waver phase
            p.vy = pick_speed(cfg_, 7.0, 13.0, rng_);
            p.max_life = frand(rng_, 1.6, 3.0);   // rise ~1/3-2/3 panel before thinning out
            p.life = 1.0;
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 2, rng_);
            p.extra = 0.0;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) {
            const double life = std::clamp(p.life, 0.0, 1.0);
            const double age  = 1.0 - life;                   // 0 fresh → 1 spent
            // Ease in so wisps condense out of nothing at the snout instead of
            // popping in, then thin out as they cool.
            const double a = std::min(1.0, age * 6.0 + 0.1) * life * 0.55;
            const int size = p.size + (age > 0.5 ? 1 : 0);
            // A small puff cluster that shears apart as it rises: centre dot
            // plus two satellites drifting outward with age (diffusion), on a
            // per-puff axis so no two wisps tear the same way.
            const double spread = 0.6 + age * 2.4;
            const double ca = std::cos(p.vx), sa = std::sin(p.vx);
            draw_dot(c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, a, size);
            draw_dot(c, p.x + ca * spread, p.y - spread * 0.8,
                     (int)p.r, (int)p.g, (int)p.b, a * 0.70, std::max(1, size - 1));
            draw_dot(c, p.x - sa * spread, p.y + spread * 0.4,
                     (int)p.r, (int)p.g, (int)p.b, a * 0.55, 1);
        }
        return c;
    }
};

// ── Waveform ─────────────────────────────────────────────────────────────────
// A scrolling audio waveform mirrored about the panel's horizontal centreline —
// the whole face visibly reacts to speech/music, not just the mouth. Feeds off
// the mic level the controller already pushes (set_audio); with no signal it
// settles to a faint idle ripple. cfg: gain, scroll_hz (samples/s), colors.

class WaveformEffect : public BaseEffect {
public:
    WaveformEffect(int w, int h, json cfg) : BaseEffect(w, h, std::move(cfg)) {
        hist_.assign(std::max(2, w_), 0.0);
    }
    void update(double dt) override {
        t_accum_ += dt;
        idle_ += dt;
        const double period = 1.0 / std::max(5.0, jnum(cfg_, "scroll_hz", 45.0));
        while (t_accum_ >= period) {          // push one column per sample tick
            t_accum_ -= period;
            const double idle = 0.06 + 0.04 * std::sin(idle_ * 2.1);
            hist_[head_] = std::max(audio_ * jnum(cfg_, "gain", 1.6), idle);
            head_ = (head_ + 1) % hist_.size();
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{0, 220, 255};
        const double mid = h_ / 2.0;
        for (int x = 0; x < w_; ++x) {
            const double v = hist_[(head_ + x) % hist_.size()];
            const int span = std::max(1, (int)std::lround(
                std::clamp(v, 0.0, 1.0) * (h_ / 2.0 - 1.0) * intensity_));
            for (int dy = 0; dy < span; ++dy) {
                const double a = 0.85 * (1.0 - (double)dy / span) + 0.10;
                draw_pixel(c, x, (int)(mid - 1 - dy), (int)col.r, (int)col.g, (int)col.b,
                           (int)(a * 255));
                draw_pixel(c, x, (int)(mid + dy),     (int)col.r, (int)col.g, (int)col.b,
                           (int)(a * 255));
            }
        }
        return c;
    }
private:
    std::vector<double> hist_;
    size_t head_ = 0;
    double t_accum_ = 0.0, idle_ = 0.0;
};

// ── Matrix ───────────────────────────────────────────────────────────────────
// Digital "code rain": bright heads racing down with fading trails, one runner
// per column slot. Direction follows the shared coupling, so with Motion
// Reactive the code leans with gravity too. cfg: count (runners), speed_min/max,
// trail, colors (default green-on-white heads).

class MatrixEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const int trail = jint(cfg_, "trail", 7);
        double dx, dy; direction_unit(dx, dy, 90.0);   // classic: straight down
        for (auto& p : particles_) {
            p.x += p.vy * dx * dt;
            p.y += p.vy * dy * dt;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return p.x < -trail - 2 || p.x > w_ + trail + 2 ||
                       p.y < -trail - 2 || p.y > h_ + trail + 2;
            }), particles_.end());
        while ((int)particles_.size() < count(std::max(4, w_ / 4))) {
            Particle p;
            direction_spawn_point(static_cast<double>(trail + 2), 90.0, p.x, p.y);
            p.x = std::floor(p.x);                     // lock runners to columns
            p.vy = pick_speed(cfg_, 10.0, 22.0, rng_);
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{60, 255, 120};
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = 1; p.life = 1; p.max_life = 9999; p.extra = trail;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        double dx, dy; direction_unit(dx, dy, 90.0);
        for (auto& p : particles_) {
            const int trail = (int)p.extra;
            for (int i = 0; i < trail; ++i) {          // trail fades behind the head
                const double a = (i == 0) ? 1.0 : 0.75 * (1.0 - (double)i / trail);
                const int r = (i == 0) ? 210 : (int)p.r;   // whitened head
                const int g = (i == 0) ? 255 : (int)p.g;
                const int b = (i == 0) ? 210 : (int)p.b;
                draw_pixel(c, (int)std::lround(p.x - dx * i),
                              (int)std::lround(p.y - dy * i), r, g, b, (int)(a * 255));
            }
        }
        return c;
    }
};

// ── Circuit ──────────────────────────────────────────────────────────────────
// Glowing circuit-board traces: dim orthogonal paths etched across the panel
// with bright pulses travelling along them. Paths regenerate every ~20 s so the
// board slowly rewires itself. cfg: count (traces), speed (px/s), colors.

class CircuitEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        rebuild_in_ -= dt;
        if (paths_.empty() || rebuild_in_ <= 0.0) build_paths();
        for (size_t i = 0; i < pulse_.size(); ++i) {
            // Per-path speed — one speed per FRAME made every pulse jitter
            // in lockstep.
            pulse_[i] += path_spd_[i] * dt;
            if (pulse_[i] >= (double)paths_[i].size())
                pulse_[i] = 0.0;                       // wrap to the trace start
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (size_t i = 0; i < paths_.size(); ++i) {
            // Per-path colour, picked at build — re-rolling ONE colour every
            // rendered frame made a multi-colour board strobe at 60 Hz.
            const Color& col = path_col_[i];
            const auto& path = paths_[i];
            for (const auto& pt : path)                // dim etched trace
                draw_pixel(c, pt.first, pt.second, (int)col.r, (int)col.g, (int)col.b, 34);
            const int head = (int)pulse_[i];           // bright pulse + short tail
            for (int t = 0; t < 4; ++t) {
                const int idx = head - t;
                if (idx < 0 || idx >= (int)path.size()) continue;
                const double a = (t == 0) ? 1.0 : 0.6 * (1.0 - t / 4.0);
                draw_pixel(c, path[idx].first, path[idx].second,
                           (int)col.r, (int)col.g, (int)col.b, (int)(a * 255));
            }
        }
        return c;
    }
private:
    void build_paths() {
        paths_.clear();
        pulse_.clear();
        path_col_.clear();
        path_spd_.clear();
        const int n = count(5);
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<int,int>> path;
            int x = (int)frand(rng_, 1, w_ - 2), y = (int)frand(rng_, 1, h_ - 2);
            bool horiz = frand(rng_, 0, 1) < 0.5;
            const int segs = 3 + (int)frand(rng_, 0, 3);
            for (int sIdx = 0; sIdx < segs; ++sIdx) {
                const int len  = 4 + (int)frand(rng_, 0, 9);
                const int step = frand(rng_, 0, 1) < 0.5 ? -1 : 1;
                for (int k = 0; k < len; ++k) {
                    if (horiz) x += step; else y += step;
                    if (x < 0 || x >= w_ || y < 0 || y >= h_) break;
                    path.push_back({x, y});
                }
                horiz = !horiz;                        // orthogonal corners
            }
            if (path.size() >= 8) {
                paths_.push_back(std::move(path));
                pulse_.push_back(frand(rng_, 0, 8));
                path_col_.push_back(has_colors(cfg_) ? pick_color(cfg_, rng_)
                                                     : Color{0, 210, 190});
                path_spd_.push_back(pick_speed(cfg_, 14.0, 22.0, rng_));
            }
        }
        rebuild_in_ = 20.0;
    }
    std::vector<std::vector<std::pair<int,int>>> paths_;
    std::vector<double> pulse_;
    std::vector<Color>  path_col_;   // per path, refreshed each rebuild (~20 s)
    std::vector<double> path_spd_;
    double rebuild_in_ = 0.0;
};

// ── Frost ────────────────────────────────────────────────────────────────────
// Ice creeping in from the panel edges: pale crystals thickest at the rim,
// thinning toward the centre, each twinkling slowly as it forms and melts.
// The temp-reactive ambient sync applies this when it's freezing out. cfg:
// count, depth_frac (how far in the frost reaches), colors.

class FrostEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;

    void update(double dt) override {
        fractal_ = cfg_.value("fractal", false);
        ensure_field();
        age_ += dt;
        // Fixed-step growth with a FIXED-SEED rng: every panel instance of
        // the same canvas computes an identical field, so the sheet stays
        // continuous across panel seams.
        // "speed" (default 1.0) is the user-facing knob: >1 forms/creeps
        // faster, <1 slower. It scales the growth rate and the drifting
        // snowflakes together so the whole effect speeds up coherently.
        const double speed  = std::clamp(jnum(cfg_, "speed", 1.0), 0.1, 4.0);
        const double form_s = std::max(0.1, jnum(cfg_, "form_s", 5.0));
        const float  rate   = static_cast<float>(
            kStep * (5.0 / form_s) * speed * std::max(0.05, intensity_));
        acc_ += dt;
        int guard = 0;
        while (acc_ >= kStep && guard++ < 8) { acc_ -= kStep; step_field(rate); }
        if (acc_ >= kStep) acc_ = 0.0;               // stalled frame: drop debt

        // Sparkle glints riding the frozen area (panel-local, cosmetic).
        for (auto& p : particles_) { p.extra += dt; p.life -= dt / p.max_life; }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [](const Particle& p){ return p.life <= 0; }), particles_.end());
        const int want = count(8);
        int tries = 0;
        while ((int)particles_.size() < want && tries++ < want * 4) {
            const int lx = irand(rng_, 0, w_ - 1), ly = irand(rng_, 0, h_ - 1);
            if (cell(lx + ox_, ly + oy_) < 0.45f) continue;    // only on solid frost
            Particle p; p.x = lx; p.y = ly;
            p.life = 1.0; p.max_life = frand(rng_, 0.5, 1.4);
            p.r = 255; p.g = 255; p.b = 255; p.size = 1;
            p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }

        // Fractal mode: large snowflakes drift down and settle, appearing
        // gradually as the sheet develops. Panel-local + cosmetic.
        if (fractal_) {
            const double drift = jnum(cfg_, "flake_drift", 6.0);
            for (auto& fp : flakes_) {
                fp.extra += dt;                             // rotation phase
                fp.y += fp.vy * dt * speed;                 // slow fall (scaled)
                fp.x += std::sin(fp.extra * 0.9 + fp.vx) * drift * dt * speed;
                fp.life -= dt / fp.max_life;
            }
            flakes_.erase(std::remove_if(flakes_.begin(), flakes_.end(),
                [&](const Particle& p){ return p.life <= 0 || p.y > h_ + p.size + 2; }),
                flakes_.end());
            const int want_f = count(5);                    // a handful at a time
            flake_acc_ += dt;
            const double spawn_every = std::max(0.3, jnum(cfg_, "flake_every", 0.9));
            while ((int)flakes_.size() < want_f && flake_acc_ >= spawn_every) {
                flake_acc_ -= spawn_every;
                Particle fp;
                fp.x = frand(rng_, 0, w_ - 1);
                fp.y = frand(rng_, -4.0, h_ * 0.35);        // enter from the top
                fp.vx = frand(rng_, 0, kTau);               // sway phase
                fp.vy = pick_speed(cfg_, 5.0, 11.0, rng_);  // fall speed
                fp.size = irand(rng_, 3, 5);                // LARGE flakes
                fp.max_life = frand(rng_, 2.5, 4.5);
                fp.life = 1.0;
                fp.r = 235; fp.g = 245; fp.b = 255;
                fp.extra = frand(rng_, 0, kTau);
                flakes_.push_back(fp);
            }
        } else if (!flakes_.empty()) {
            flakes_.clear();
        }
    }

    cv::Mat render() override {
        cv::Mat c = blank();
        // Translucent over the face (~50% by default) so the expression stays
        // readable underneath; "opacity" in the layer cfg adjusts it.
        const double opacity = std::clamp(jnum(cfg_, "opacity", 0.5), 0.0, 1.0);
        // Density drives a double gradient: color runs white feathery tips →
        // pale ice → deep blue solid rim, and alpha fades with the same
        // density, so the sheet dissolves smoothly into the clear center.
        // A "colors" list in the layer cfg overrides the ramp (first entry =
        // thin inner ice, last = dense rim).
        std::vector<Color> grad;
        if (has_colors(cfg_)) {
            const auto it = cfg_.find("colors");
            if (it != cfg_.end() && it->is_array())
                for (const auto& jc : *it)
                    if (jc.is_array() && jc.size() >= 3)
                        grad.push_back({jc[0].get<int>(), jc[1].get<int>(),
                                        jc[2].get<int>()});
        }
        if (grad.empty())
            grad = { {255, 255, 255}, {200, 230, 255}, {120, 180, 255} };
        // Face tint: the face material "freezes" toward the frost's rim colour
        // in a gradient that LEADS the crystal front. The ramp saturates fast
        // (f*2) and the mask is then blurred, spreading the blue several px
        // INSIDE the visible crystal edge — without this the strong tint sat
        // only where the add-blended crystals already wash the face out, so
        // it was invisible in practice. Default ON for fractal frost;
        // "face_tint" (0..1) in the layer cfg overrides.
        const double tint_k = std::clamp(
            jnum(cfg_, "face_tint", fractal_ ? 0.8 : 0.0), 0.0, 1.0);
        tint_active_ = tint_k > 0.0;
        if (tint_active_) {
            if (tint_mask_.rows != h_ || tint_mask_.cols != w_ ||
                tint_mask_.type() != CV_8UC1)
                tint_mask_.create(h_, w_, CV_8UC1);
            tint_mask_.setTo(0);
            tint_col_ = grad.back();   // dense rim colour = the frost blue
        }
        const double tsh = age_ * 1.4;
        for (int ly = 0; ly < h_; ++ly) {
            uchar* tm = tint_active_ ? tint_mask_.ptr<uchar>(ly) : nullptr;
            for (int lx = 0; lx < w_; ++lx) {
                const int gx = lx + ox_, gy = ly + oy_;
                const float f = cell(gx, gy);
                if (f < 0.04f) continue;
                if (tm) tm[lx] = (uchar)std::lround(
                    255.0 * tint_k * std::min(1.0, (double)f * 2.0));
                const float h1 = hash01(gx, gy);
                const Color col = sample_grad(grad, f);
                // Per-cell grain brightness keeps crystal texture in the sheet
                // so the gradient doesn't read airbrushed-flat.
                const float vb = 0.88f + 0.24f * h1;
                const int r = std::min(255, (int)(col.r * vb));
                const int g = std::min(255, (int)(col.g * vb));
                const int b = std::min(255, (int)(col.b * vb));
                const double shimmer =
                    0.85 + 0.15 * std::sin(tsh + h1 * kTau * 4.0);
                const double a = opacity * std::pow((double)f, 0.9) * shimmer;
                draw_pixel(c, lx, ly, r, g, b, (int)std::lround(a * 255.0));
            }
        }
        // Spread the tint past the crystal front: blur a copy inward and take
        // the per-pixel MAX with the raw mask — the raw term keeps the rim at
        // full strength while the blurred term pushes the blue several pixels
        // deeper into the face than the ice itself, so the freeze-over reads
        // AHEAD of the crystals instead of hidden underneath them.
        if (tint_active_) {
            const int ks = std::max(5, (std::min(w_, h_) / 3) | 1);
            cv::GaussianBlur(tint_mask_, tint_blur_, cv::Size(ks, ks), ks * 0.5);
            cv::max(tint_mask_, tint_blur_, tint_mask_);
        }
        // Glints on top: brief white twinkles on the solid sheet.
        for (auto& p : particles_) {
            const double env = std::clamp(std::min((1.0 - p.life) * 6.0,
                                                   p.life * 6.0), 0.0, 1.0);
            draw_particle(c, p, env * (0.4 + 0.6 * (0.5 + 0.5 * std::sin(p.extra * 9.0)))
                              * opacity);
        }
        // Large snowflakes (fractal mode): fade in, hold, fade out.
        for (auto& fp : flakes_) {
            const double env = std::clamp(std::min((1.0 - fp.life) * 4.0,
                                                   fp.life * 3.0), 0.0, 1.0);
            draw_snowflake(c, fp.x, fp.y, fp.size, fp.extra * 0.5,
                           (int)fp.r, (int)fp.g, (int)fp.b,
                           env * std::min(1.0, opacity + 0.35));
        }
        return c;
    }

    const cv::Mat* face_tint(uint8_t& r, uint8_t& g, uint8_t& b) const override {
        if (!tint_active_ || tint_mask_.empty()) return nullptr;
        r = (uint8_t)tint_col_.r;
        g = (uint8_t)tint_col_.g;
        b = (uint8_t)tint_col_.b;
        return &tint_mask_;
    }

private:
    static constexpr double kStep = 1.0 / 30.0;   // fixed growth timestep

    float cell(int gx, int gy) const {
        if (gx < 0 || gx >= cw_ || gy < 0 || gy >= ch_ || field_.empty()) return 0.f;
        return field_[(size_t)gy * cw_ + gx];
    }
    // Deterministic per-cell hash — shared "crystal grain" across panels.
    static float hash01(int x, int y) {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h >> 16) & 0xFFFF) / 65535.f;
    }

    // (Re)build the field + reach mask when geometry or shaping cfg changes.
    void ensure_field() {
        const bool   top   = cfg_.value("include_top", false);
        // Fractal fingers reach deeper in than the smooth front.
        const double reach = std::clamp(
            jnum(cfg_, "reach", jnum(cfg_, "depth_frac", 0.32)) *
            (fractal_ ? 1.4 : 1.0), 0.05, 0.95);
        if ((int)field_.size() == cw_ * ch_ && top == mask_top_ &&
            std::fabs(reach - mask_reach_) < 1e-6 && fractal_ == mask_fractal_)
            return;
        mask_top_ = top; mask_reach_ = reach; mask_fractal_ = fractal_;
        const int W = cw_, H = ch_;
        field_.assign((size_t)W * H, 0.f);
        mask_.assign((size_t)W * H, 0.f);
        grain_.assign((size_t)W * H, 0.f);
        det_rng_.seed(0x1CEF7057u);
        acc_ = 0.0; age_ = 0.0;
        const float reach_px = (float)(reach * std::min(W, H));
        // Corners where two enabled edges meet: bottom pair always; the top
        // pair only when the top rim is on.
        const int ncorner = top ? 4 : 2;
        const float cxs[4] = { 0.f, (float)(W - 1), 0.f, (float)(W - 1) };
        const float cys[4] = { (float)(H - 1), (float)(H - 1), 0.f, 0.f };
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float de = std::min({ (float)x, (float)(W - 1 - x),
                                      (float)(H - 1 - y),
                                      top ? (float)y : 1e9f });
                float dc = 1e9f;
                for (int i = 0; i < ncorner; ++i)
                    dc = std::min(dc, std::hypot(x - cxs[i], y - cys[i]));
                // Frost bites ~reach_px in from the rim, nearly twice that in
                // the corners — the classic curved corner wedge.
                const float rl = reach_px *
                    (0.75f + 1.15f * std::exp(-dc / (reach_px * 1.2f)));
                const float m = std::clamp(1.f - de / std::max(1.f, rl), 0.f, 1.f);
                mask_[(size_t)y * W + x] = std::pow(m, 0.7f);
                // Static per-cell heterogeneity: fast lanes become the
                // fingers that race ahead of the front, slow cells the gaps.
                // Fractal mode sharpens this hard (g^4, near-zero floor) so a
                // few lanes race far ahead and branch — dendritic ferns —
                // while the gaps between them stay clear.
                const float g = hash01(x * 3 + 1, y * 5 + 2);
                grain_[(size_t)y * W + x] = fractal_
                    ? 0.03f + 0.97f * g * g * g * g
                    : 0.15f + 0.85f * g * g;
            }
        }
    }

    // One growth step: rim cells self-seed; interior cells accrete only next
    // to existing frost, at a rate shaped by the reach mask and the grain.
    void step_field(float rate) {
        const int W = cw_, H = ch_;
        // Alternate scan direction so in-place neighbor reads don't give the
        // front a fixed directional bias.
        const bool rev = (step_n_++ & 1) != 0;
        for (int yy = 0; yy < H; ++yy) {
            const int y = rev ? H - 1 - yy : yy;
            for (int xx = 0; xx < W; ++xx) {
                const int x = rev ? W - 1 - xx : xx;
                const size_t i = (size_t)y * W + x;
                const float m = mask_[i];
                if (m <= 0.f) continue;
                float& f = field_[i];
                // The mask is the density CEILING, not just a rate: cells can
                // only fill to it, so the finished sheet is a gradient —
                // solid ice at the rim thinning continuously to nothing at
                // the inner reach limit (thin inner ice also renders in the
                // whiter tip tint, like real feathered frost).
                if (f >= m) continue;
                float sup = (m >= 0.999f) ? 0.6f : 0.f;   // rim self-seeds
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                        sup = std::max(sup, field_[(size_t)ny * W + nx]);
                    }
                // Fractal mode lets thinner fingers thread inward (lower
                // supply threshold) and race with more variance.
                if (sup <= (mask_fractal_ ? 0.02f : 0.05f)) continue;
                const double vlo = mask_fractal_ ? 0.4 : 0.6;
                const double vhi = mask_fractal_ ? 1.7 : 1.4;
                f = std::min(m, f + rate * m * sup * grain_[i] *
                                     (float)frand(det_rng_, vlo, vhi));
            }
        }
    }

    double age_ = 0.0, acc_ = 0.0;
    uint32_t step_n_ = 0;
    bool   mask_top_     = false;
    bool   fractal_      = false;   // dendritic growth + large snowflakes
    bool   mask_fractal_ = false;   // fractal value the field was built for
    double mask_reach_   = -1.0;
    std::vector<float> field_, mask_, grain_;   // canvas-space growth state
    std::vector<Particle> flakes_;              // large drifting snowflakes
    double flake_acc_ = 0.0;                     // snowflake spawn timer
    std::mt19937 det_rng_{0x1CEF7057u};         // shared fixed seed (see update)
    cv::Mat tint_mask_, tint_blur_;              // face-tint mask + blur scratch
    bool    tint_active_ = false;
    Color   tint_col_{120, 180, 255};
};

// ── Heatwave ─────────────────────────────────────────────────────────────────
// Heat shimmer: translucent warm streaks rising off the whole face, wavering
// as they climb — reads as hot air coming off the panels. The temp-reactive
// ambient sync applies this when it's scorching out. Follows the gravity
// coupling like steam. cfg: count, speed_min/max, wander, colors.

class HeatwaveEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        heartbeat_ = cfg_.value("heartbeat", false);
        // "speed" (default 1.0) is the user-facing knob: >1 the shimmer rises
        // and wavers faster and the heartbeat quickens, <1 slower. Life still
        // ticks in real time so the streak lengths stay natural.
        const double speed = std::clamp(jnum(cfg_, "speed", 1.0), 0.1, 4.0);
        hb_t_ += dt * speed;
        const double wander = jnum(cfg_, "wander", 8.0);
        double dx, dy; direction_unit(dx, dy, 270.0);   // rises by default
        for (auto& p : particles_) {
            p.extra += dt * speed;
            p.x += (p.vy * dx + std::sin(p.extra * 3.1 + p.vx) * wander) * dt * speed;
            p.y += p.vy * dy * dt * speed;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return p.life <= 0 || p.y < -5 || p.y > h_ + 5 ||
                       p.x < -5 || p.x > w_ + 5;
            }), particles_.end());
        while ((int)particles_.size() < count(18)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_)
                                         : Color{255, 120, 40};
            Particle p;
            p.x = frand(rng_, 0, w_ - 1);
            p.y = frand(rng_, h_ * 0.4, h_ + 3.0);      // lower half + below
            p.vx = frand(rng_, 0, kTau);                // waver phase
            p.vy = pick_speed(cfg_, 5.0, 10.0, rng_);
            p.max_life = frand(rng_, 1.2, 2.2);
            p.life = 1.0;
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = 1; p.extra = 0.0;
            particles_.push_back(p);
        }
    }

    // A heartbeat envelope over one beat period: a strong "lub" then a softer
    // "dub" a moment later, then rest — 0..1. bpm sets the (slow) pace.
    double heartbeat_env() const {
        const double bpm    = std::max(20.0, jnum(cfg_, "bpm", 50.0));
        const double period = 60.0 / bpm;
        const double ph     = std::fmod(hb_t_, period) / period;   // 0..1
        auto thump = [](double x, double c, double w) {
            const double d = (x - c) / w;
            return std::exp(-d * d);
        };
        const double e = thump(ph, 0.00, 0.045) + 0.6 * thump(ph, 0.18, 0.05);
        return std::clamp(e, 0.0, 1.0);
    }

    cv::Mat render() override {
        cv::Mat c = blank();
        double dx, dy; direction_unit(dx, dy, 270.0);
        for (auto& p : particles_) {
            for (int i = 0; i < 3; ++i) {               // short shimmer streak
                const double a = std::clamp(p.life, 0.0, 1.0) * 0.30 *
                                 (1.0 - i / 3.0);
                draw_pixel(c, (int)std::lround(p.x - dx * i),
                              (int)std::lround(p.y - dy * i),
                           (int)p.r, (int)p.g, (int)p.b, (int)(a * 255));
            }
        }

        // Heartbeat: an orange glow pulsing along the SAME panel edges the
        // frost creeps in from (canvas-edge band, corners deeper), throbbing
        // lub-dub. Additive over the shimmer.
        if (heartbeat_) {
            const double env = heartbeat_env();
            if (env > 0.01) {
                const bool   top  = cfg_.value("include_top", false);
                const double reach = std::clamp(
                    jnum(cfg_, "reach", jnum(cfg_, "depth_frac", 0.32)), 0.05, 0.9);
                const double reach_px = reach * std::min(cw_, ch_);
                Color glow = has_colors(cfg_) ? pick_color(cfg_, rng_)
                                              : Color{255, 90, 20};
                const double amp = std::clamp(jnum(cfg_, "pulse_alpha", 0.55), 0.0, 1.0)
                                   * std::max(0.05, intensity_);
                for (int ly = 0; ly < h_; ++ly) {
                    for (int lx = 0; lx < w_; ++lx) {
                        const int gx = lx + ox_, gy = ly + oy_;
                        const double de = std::min({ (double)gx, (double)(cw_ - 1 - gx),
                                                     (double)(ch_ - 1 - gy),
                                                     top ? (double)gy : 1e9 });
                        double band = 1.0 - de / std::max(1.0, reach_px);
                        if (band <= 0.02) continue;
                        band = band * band;             // concentrate at the rim
                        const double a = amp * band * env;
                        draw_pixel(c, lx, ly, glow.r, glow.g, glow.b,
                                   (int)std::clamp(a * 255.0, 0.0, 255.0));
                    }
                }
            }
        }
        return c;
    }

private:
    bool   heartbeat_ = false;
    double hb_t_ = 0.0;
};

// ── Fireflies ────────────────────────────────────────────────────────────────

class FirefliesEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const double lo = jnum(cfg_, "speed_min", 3.0);
        const double hi = jnum(cfg_, "speed_max", 6.0);
        for (auto& p : particles_) {
            p.extra += dt;
            // Per-fly speed, derived from its own wander phase — one speed
            // used to be rolled per FRAME for the whole flock (lockstep
            // jitter). Deriving keeps live speed-slider edits applying.
            const double spd = lo + (hi - lo) * (0.5 + 0.5 * std::sin(p.vx * 3.7));
            p.x += std::cos(p.extra * 1.3 + p.vx) * spd * dt;
            p.y += std::sin(p.extra       + p.vy) * spd * dt;
            p.x = std::fmod(std::fmod(p.x, w_) + w_, w_);
            p.y = std::fmod(std::fmod(p.y, h_) + h_, h_);
            p.life = 0.5 + 0.5 * std::sin(p.extra * 2.5);
        }
        // Retire a fly only WHILE DARK once its lifespan is up, then respawn
        // fresh — flies were immortal, so builder colour edits never applied
        // to them; now edits fade in blink by blink with no visible pop.
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [](const Particle& p){ return p.extra > p.max_life && p.life < 0.05; }),
            particles_.end());
        while ((int)particles_.size() < count(8)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{180, 255, 100};
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, 0, h_ - 1);
            p.vx = frand(rng_, 0, kTau); p.vy = frand(rng_, 0, kTau);
            p.life = 1; p.max_life = frand(rng_, 6.0, 12.0);
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) draw_particle(c, p, std::clamp(p.life, 0.0, 1.0));
        return c;
    }
};

// ── Breath (expanding puff) ──────────────────────────────────────────────────
// A soft, semi-transparent cloud that grows from a point and fades as it goes —
// a visible exhale. Unlike the ambient effects this one is EVENT-shaped: it can
// free-run at a rate, or fire on a threshold crossing (speaking, a jolt) so a
// puff coincides with the thing that caused it.
//
// Config: origin_x/origin_y (0..1 of the panel), colors, alpha_max (peak
// opacity), size_min/size_max (start/end radius in px), speed_min/speed_max
// (px/s of expansion — this sets how long a puff lives), rate (puffs/s when
// free-running), trigger ("none"|"audio"|"motion"), threshold, count (puffs per
// firing), spread_frac (jitter around the origin as a fraction of the panel).
class BreathEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;

    void update(double dt) override {
        const std::string trig = cfg_.value("trigger", std::string("none"));
        const double thr = jnum(cfg_, "threshold", 0.25);
        bool fire = false;
        if (trig == "audio" || trig == "motion") {
            const double lvl = (trig == "audio")
                             ? audio_
                             : std::fabs(motion_.accel_g - 1.0);
            // Rising-edge with hysteresis: one puff per crossing, not one per
            // frame for as long as the signal stays loud.
            if (lvl >= thr && armed_) { fire = true; armed_ = false; }
            if (lvl < thr * 0.6)      armed_ = true;
        } else {
            emit_cd_ -= dt;
            if (emit_cd_ <= 0.0) {
                fire = true;
                emit_cd_ = 1.0 / std::max(0.05, jnum(cfg_, "rate", 0.6));
            }
        }
        if (fire) spawn();

        for (auto& p : puffs_) p.age += dt;
        puffs_.erase(std::remove_if(puffs_.begin(), puffs_.end(),
                                    [](const Puff& p){ return p.age >= p.life; }),
                     puffs_.end());
    }

    cv::Mat render() override {
        cv::Mat c = blank();
        const double amax = std::clamp(jnum(cfg_, "alpha_max", 0.55), 0.0, 1.0);
        for (const auto& p : puffs_) {
            const double t = std::clamp(p.age / std::max(1e-3, p.life), 0.0, 1.0);
            const double r = p.r0 + (p.r1 - p.r0) * t;
            // Fade in briefly then out, so a puff appears rather than pops.
            const double env = (t < 0.15) ? (t / 0.15) : (1.0 - (t - 0.15) / 0.85);
            const double a = amax * std::clamp(env, 0.0, 1.0);
            if (a <= 0.004 || r <= 0.5) continue;
            draw_soft_disc(c, p.x, p.y, r, p.r, p.g, p.b, a);
        }
        return c;
    }

private:
    struct Puff {
        double x = 0, y = 0, age = 0, life = 1, r0 = 1, r1 = 8;
        int r = 255, g = 255, b = 255;
    };

    void spawn() {
        const int n = count(1);
        const double ox = jnum(cfg_, "origin_x", 0.5) * w_;
        const double oy = jnum(cfg_, "origin_y", 0.5) * h_;
        // Origin can be a LINE rather than a point, so the puff can leave a
        // long muzzle along its whole width instead of pinching out of one
        // spot. origin_len is a fraction of panel width (0 = point) and
        // origin_angle tilts the line (0 = horizontal, +ve = clockwise).
        const double llen = std::max(0.0, jnum(cfg_, "origin_len", 0.0)) * w_;
        const double lrad = jnum(cfg_, "origin_angle", 0.0) * M_PI / 180.0;
        const double lux = std::cos(lrad), luy = std::sin(lrad);
        const double jitter = jnum(cfg_, "spread_frac", 0.06) *
                              std::min(w_, h_);
        const double r0lo = jnum(cfg_, "size_min", 1.5);
        const double r1hi = jnum(cfg_, "size_max", 14.0);
        const double slo  = jnum(cfg_, "speed_min", 6.0);
        const double shi  = jnum(cfg_, "speed_max", 12.0);
        for (int i = 0; i < n; ++i) {
            Puff p;
            // Even spread along the line, then jitter — a row of puffs that
            // reads as one exhale rather than n separate ones.
            const double t = (llen > 0.0) ? frand(rng_, -0.5, 0.5) * llen : 0.0;
            p.x = ox + lux * t + frand(rng_, -jitter, jitter);
            p.y = oy + luy * t + frand(rng_, -jitter, jitter);
            p.r0 = r0lo;
            p.r1 = std::max(r0lo + 1.0, r1hi * frand(rng_, 0.75, 1.0));
            const double spd = std::max(0.5, frand(rng_, slo, shi));
            p.life = (p.r1 - p.r0) / spd;      // speed sets how long it lives
            const Color col = pick_color(cfg_, rng_);
            p.r = col.r; p.g = col.g; p.b = col.b;
            puffs_.push_back(p);
        }
    }

    // Radial falloff so the puff reads as vapour, not a hard circle.
    void draw_soft_disc(cv::Mat& c, double cx, double cy, double rad,
                        int cr, int cg, int cb, double a) {
        const int x0 = std::max(0, static_cast<int>(cx - rad) - 1);
        const int x1 = std::min(c.cols - 1, static_cast<int>(cx + rad) + 1);
        const int y0 = std::max(0, static_cast<int>(cy - rad) - 1);
        const int y1 = std::min(c.rows - 1, static_cast<int>(cy + rad) + 1);
        const double inv = 1.0 / std::max(1e-3, rad);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                const double d = std::sqrt(dx * dx + dy * dy) * inv;
                if (d >= 1.0) continue;
                // smoothstep edge — soft rim, denser core
                const double f = (1.0 - d) * (1.0 - d);
                // Accumulate by MAX rather than overwrite: overlapping puffs
                // should read as one denser cloud, and draw_pixel replaces the
                // pixel outright (alpha included), which would let a young puff
                // punch a hole through an older one it crosses.
                const int na = static_cast<int>(
                    std::clamp(a * f * 255.0, 0.0, 255.0));
                if (x >= 0 && x < c.cols && y >= 0 && y < c.rows) {
                    cv::Vec4b& px = c.at<cv::Vec4b>(y, x);
                    if (na > px[3])
                        px = cv::Vec4b(cv::saturate_cast<uchar>(cr),
                                       cv::saturate_cast<uchar>(cg),
                                       cv::saturate_cast<uchar>(cb),
                                       static_cast<uchar>(na));
                }
            }
    }

    std::vector<Puff> puffs_;
    double emit_cd_ = 0.0;
    bool   armed_   = true;
};

// ── Clouds (nebula) ──────────────────────────────────────────────────────────

class CloudsEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;

    void update(double dt) override {
        double churn = jnum(cfg_, "churn", 0.4);
        // When the user has set direction_deg, particles travel along the
        // (dx, dy) unit vector. Otherwise the historical behaviour is a
        // random horizontal drift per clump (sign baked into vx by spawn()),
        // and we leave that intact.
        const bool has_dir = cfg_.contains("direction_deg");
        double dx = 0, dy = 0;
        if (has_dir) direction_unit(dx, dy, 0.0);   // 0° = right (default)
        for (auto& c : clumps_) {
            if (has_dir) {
                const double spd = std::abs(c.vx);
                c.x += spd * dx * dt;
                c.y += spd * dy * dt;
            } else {
                c.x += c.vx * dt;
            }
            c.phase += dt * churn;
            const double margin = c.size * 2 + 2;
            const bool off_x = c.x - margin > w_ || c.x + margin < 0;
            const bool off_y = c.y - margin > h_ || c.y + margin < 0;
            if (off_x || off_y) {
                if (has_dir) {
                    // Re-enter from a TRAILING EDGE, spread along it.
                    //
                    // ⚠ NEVER test an axis for zero here. direction_unit()
                    // goes through cos/sin, so only 0° gives a true 0 — 90°,
                    // 180°, 270° and 360° come back with a ~1e-16 residue on
                    // the other axis. The old code tested `dx > 0` / `dx < 0`
                    // and treated that residue as real travel, so it pinned
                    // the clump to an edge it had no velocity to leave: at
                    // 180° every clump respawned at y = -margin and drifted
                    // left forever, off-screen. Every direction the 10° slider
                    // can reach except exactly 0° was affected.
                    //
                    // An axis below kAxisEps is therefore treated as no motion
                    // at all, and the clump is spread randomly along it.
                    constexpr double kAxisEps = 1e-3;
                    const double ax = std::fabs(dx), ay = std::fabs(dy);
                    const bool can_x = ax > kAxisEps, can_y = ay > kAxisEps;
                    // With both axes live, pick the entry edge in proportion to
                    // the travel on each. A 45° drift then feeds the left AND
                    // top edges instead of funnelling every clump through one
                    // corner and down a single diagonal.
                    const bool use_x_edge =
                        can_x && (!can_y || frand(rng_, 0.0, ax + ay) < ax);
                    if (use_x_edge) {
                        c.x = (dx >= 0) ? -margin : w_ + margin;
                        c.y = frand(rng_, 0, h_ - 1);
                    } else {
                        c.y = (dy >= 0) ? -margin : h_ + margin;
                        c.x = frand(rng_, 0, w_ - 1);
                    }
                } else {
                    c.x = (c.vx >= 0) ? -margin : w_ + margin;
                    c.y = frand(rng_, 0, h_ - 1);
                }
                reseed(c);
            }
        }
        while ((int)clumps_.size() < count(6)) clumps_.push_back(spawn(-1));
    }

    cv::Mat render() override {
        if (acc_.rows != h_ || acc_.cols != w_ || acc_.type() != CV_32FC4)
            acc_.create(h_, w_, CV_32FC4);
        acc_.setTo(cv::Scalar(0, 0, 0, 0));
        double softness = std::clamp(jnum(cfg_, "softness", 0.6), 0.0, 1.0);
        double power = 1.0 + (1.0 - softness) * 2.0;
        order_.clear();
        for (auto& c : clumps_) order_.push_back(&c);
        std::sort(order_.begin(), order_.end(),
                  [](const Clump* a, const Clump* b){ return a->size > b->size; });
        for (const Clump* c : order_) draw_clump(acc_, *c, power);

        cv::Mat out = blank();
        for (int y = 0; y < h_; ++y) {
            const cv::Vec4f* a = acc_.ptr<cv::Vec4f>(y);
            cv::Vec4b*       o = out.ptr<cv::Vec4b>(y);
            for (int x = 0; x < w_; ++x) {
                o[x][0] = cv::saturate_cast<uchar>(a[x][0]);
                o[x][1] = cv::saturate_cast<uchar>(a[x][1]);
                o[x][2] = cv::saturate_cast<uchar>(a[x][2]);
                o[x][3] = cv::saturate_cast<uchar>(a[x][3] * 255.0f);
            }
        }
        return out;
    }

private:
    struct Lobe { double ox, oy, lr, wt; int r, g, b; };
    struct Clump {
        double x = 0, y = 0, vx = 0;
        int    size = 8;
        double base_alpha = 0.3;
        std::vector<Lobe> lobes;
        double warp[6] = {0,0,0,0,0,0};
        double phase = 0;
    };

    std::vector<std::array<int,3>> palette() {
        std::vector<std::array<int,3>> pool;
        auto it = cfg_.find("colors");
        if (it != cfg_.end() && it->is_array() && !it->empty()) {
            for (auto& c : *it) pool.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
        } else {
            pool = {{200,70,150},{120,80,210},{70,120,220},{40,170,190},{230,120,180}};
        }
        return pool;
    }

    std::vector<Lobe> make_lobes(double R, const std::vector<std::array<int,3>>& pool) {
        int lo = jint(cfg_, "lobes_min", 3), hi = jint(cfg_, "lobes_max", 6);
        int n = std::max(1, irand(rng_, std::min(lo, hi), std::max(lo, hi)));
        std::vector<Lobe> lobes;
        for (int i = 0; i < n; ++i) {
            Lobe L;
            if (i == 0) { L.ox = 0; L.oy = 0; L.lr = R * frand(rng_, 0.55, 0.9); }
            else {
                double ang = frand(rng_, 0, kTau), dist = frand(rng_, 0.2, 0.75) * R;
                L.ox = std::cos(ang) * dist; L.oy = std::sin(ang) * dist;
                L.lr = R * frand(rng_, 0.3, 0.7);
            }
            L.lr = std::max(1.0, L.lr);
            L.wt = frand(rng_, 0.5, 1.0);
            const auto& col = pool[irand(rng_, 0, (int)pool.size() - 1)];
            L.r = col[0]; L.g = col[1]; L.b = col[2];
            lobes.push_back(L);
        }
        return lobes;
    }

    void reseed(Clump& c) { c.lobes = make_lobes((double)c.size, palette()); }

    Clump spawn(double x) {
        Clump c;
        double R = pick_size(cfg_, 5, 16, rng_);
        double spd = pick_speed(cfg_, 1.5, 5.0, rng_);
        if (frand(rng_, 0, 1) < 0.5) spd = -spd;
        c.base_alpha = frand(rng_, jnum(cfg_, "alpha_min", 0.15), jnum(cfg_, "alpha_max", 0.5));
        c.x = (x < 0) ? frand(rng_, 0, w_ - 1) : x;
        c.y = frand(rng_, 0, h_ - 1);
        c.vx = spd; c.size = (int)std::lround(R);
        c.lobes = make_lobes(R, palette());
        for (int i = 0; i < 6; ++i) c.warp[i] = (i % 3 == 2) ? frand(rng_, 0, kTau) : frand(rng_, 0.2, 0.6);
        c.phase = frand(rng_, 0, kTau);
        return c;
    }

    void draw_clump(cv::Mat& acc, const Clump& c, double power) {
        double R = std::max(1.0, (double)c.size);
        double reach = R * 1.7;
        int x0 = std::max(0, (int)std::floor(c.x - reach));
        int x1 = std::min(w_, (int)std::ceil(c.x + reach) + 1);
        int y0 = std::max(0, (int)std::floor(c.y - reach));
        int y1 = std::min(h_, (int)std::ceil(c.y + reach) + 1);
        if (x0 >= x1 || y0 >= y1) return;
        double floor_ = 1.0 - std::clamp(jnum(cfg_, "turbulence", 0.6), 0.0, 1.0);
        double f1 = c.warp[0], g1 = c.warp[1], ph1 = c.warp[2];
        double f2 = c.warp[3], g2 = c.warp[4], ph2 = c.warp[5];
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                double wsum = 0, csr = 0, csg = 0, csb = 0;
                for (const Lobe& L : c.lobes) {
                    double dx = x - (c.x + L.ox), dy = y - (c.y + L.oy);
                    double d = std::sqrt(dx * dx + dy * dy) / L.lr;
                    double f = std::pow(std::clamp(1.0 - d, 0.0, 1.0), power) * L.wt;
                    wsum += f; csr += f * L.r; csg += f * L.g; csb += f * L.b;
                }
                double lx = x - c.x, ly = y - c.y;
                double n1 = 0.5 + 0.5 * std::sin(lx * f1 + ly * g1 + ph1 + c.phase);
                double n2 = 0.5 + 0.5 * std::sin(lx * f2 - ly * g2 + ph2 - c.phase * 0.7);
                double turb = 0.6 * n1 + 0.4 * n2;
                turb = floor_ + (1.0 - floor_) * turb;
                double dens = std::clamp(wsum * turb, 0.0, 1.0);
                double a = dens * c.base_alpha;
                cv::Vec4f& px = acc.at<cv::Vec4f>(y, x);
                if (a > px[3]) {
                    double safe = wsum > 1e-6 ? wsum : 1.0;
                    px[0] = (float)(csr / safe); px[1] = (float)(csg / safe);
                    px[2] = (float)(csb / safe); px[3] = (float)a;
                }
            }
    }

    std::vector<Clump> clumps_;
    cv::Mat acc_;                          // float accumulator, reused each frame
    std::vector<const Clump*> order_;      // draw order scratch, reused each frame
};

// ── Lightning ──────────────────────────────────────────────────────────────────
// Jagged bolts that flash and fade, with random forked branches. "rate" ≈
// strikes/sec; "branches" (0..1) sets fork density. "origin": "edge" (default)
// strikes from the directional spawn edge and travels the configured
// direction; "random" strikes from anywhere on the canvas in a random
// direction with varied length. With "arc": true it becomes continuous
// crackling electrical arcs between drifting points ("count" arcs).
class LightningEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const bool arc = cfg_.value("arc", false)
                      || cfg_.value("mode", std::string("bolt")) == "arc";
        if (arc) update_arcs(dt); else update_bolts(dt);
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (const auto& b : bolts_) {
            const double a = b.arc ? b.alpha : std::clamp(b.life / b.max_life, 0.0, 1.0);
            const int A = (int)std::clamp(a * 255.0, 0.0, 255.0);
            for (const auto& stroke : b.strokes)
                for (size_t i = 1; i < stroke.size(); ++i)
                    cv::line(c, stroke[i-1], stroke[i],
                             cv::Scalar(b.r, b.g, b.b, A), 1, cv::LINE_8);
        }
        return c;
    }
private:
    struct Bolt {
        std::vector<std::vector<cv::Point>> strokes;   // [0] = trunk, rest = branches
        double life = 0, max_life = 1, alpha = 1.0;
        int r = 255, g = 255, b = 255;
        bool arc = false;
        cv::Point a{0,0}, e{0,0};                       // arc endpoints
        double relocate = 0.0, age = 0.0;
    };
    std::vector<Bolt> bolts_;
    double timer_ = 0.0;

    cv::Point rand_point() {
        return cv::Point(irand(rng_, 0, std::max(0, w_ - 1)),
                         irand(rng_, 0, std::max(0, h_ - 1)));
    }
    // A point on one of the screen edges — arcs anchored here read like a
    // discharge jumping off a wire / between insulators.
    cv::Point rand_edge_point() {
        const int W = std::max(1, w_), H = std::max(1, h_);
        switch (irand(rng_, 0, 3)) {
            case 0:  return cv::Point(irand(rng_, 0, W - 1), 0);        // top
            case 1:  return cv::Point(irand(rng_, 0, W - 1), H - 1);    // bottom
            case 2:  return cv::Point(0,        irand(rng_, 0, H - 1)); // left
            default: return cv::Point(W - 1,    irand(rng_, 0, H - 1)); // right
        }
    }
    // Jagged polyline a→e with perpendicular jitter; randomly forks branches
    // (up to 2 levels deep) into `strokes` with the trunk pushed first.
    void build_strokes(cv::Point a, cv::Point e, int steps, double jit,
                       double branch_prob, std::vector<std::vector<cv::Point>>& strokes,
                       int depth) {
        const double dxn = e.x - a.x, dyn = e.y - a.y;
        const double len = std::hypot(dxn, dyn);
        if (len < 1.0 || steps < 1) { strokes.push_back({a, e}); return; }
        const double ux = dxn / len, uy = dyn / len;   // along
        const double px = -uy, py = ux;                // perpendicular
        std::vector<cv::Point> path;
        std::vector<cv::Point> seeds;                  // branch start points
        for (int i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            const double off = (i == 0 || i == steps) ? 0.0 : frand(rng_, -jit, jit);
            const int x = (int)std::lround(a.x + ux * len * t + px * off);
            const int y = (int)std::lround(a.y + uy * len * t + py * off);
            path.emplace_back(x, y);
            if (depth < 2 && i > 0 && i < steps && frand(rng_, 0, 1) < branch_prob)
                seeds.emplace_back(x, y);
        }
        strokes.push_back(std::move(path));            // trunk first
        for (const auto& s : seeds) {
            const double bang = std::atan2(uy, ux) + frand(rng_, -1.0, 1.0);
            const double blen = len * frand(rng_, 0.18, 0.42);
            const cv::Point bend((int)std::lround(s.x + std::cos(bang) * blen),
                                 (int)std::lround(s.y + std::sin(bang) * blen));
            build_strokes(s, bend, std::max(3, steps / 2), jit * 0.7,
                          branch_prob * 0.5, strokes, depth + 1);
        }
    }
    void update_bolts(double dt) {
        for (auto& b : bolts_) b.life -= dt;
        bolts_.erase(std::remove_if(bolts_.begin(), bolts_.end(),
            [](const Bolt& b){ return b.arc || b.life <= 0; }), bolts_.end());
        timer_ -= dt;
        if (timer_ > 0) return;
        const double rate = std::max(0.05, jnum(cfg_, "rate", 1.0) * intensity_);
        timer_ = frand(rng_, 0.4, 1.6) / rate;
        Bolt b;
        b.max_life = b.life = frand(rng_, 0.10, 0.22);
        const Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{180, 210, 255};
        b.r = col.r; b.g = col.g; b.b = col.b;
        const double L = std::max(w_, h_) * 1.4;
        cv::Point a, e;
        if (cfg_.value("origin", std::string("edge")) == "random") {
            // Random origin: each strike starts anywhere on the canvas and
            // travels a random direction, with varied length so interior
            // strikes read as localized discharges (cv::line clips whatever
            // runs off the edge).
            a = rand_point();
            const double ang  = frand(rng_, 0.0, 2.0 * CV_PI);
            const double blen = L * frand(rng_, 0.45, 1.0);
            e = cv::Point((int)std::lround(a.x + std::cos(ang) * blen),
                          (int)std::lround(a.y + std::sin(ang) * blen));
        } else {
            double dx, dy; direction_unit(dx, dy, 90.0);   // default fall = down
            double sx, sy; direction_spawn_point(0.0, 90.0, sx, sy);
            a = cv::Point((int)sx, (int)sy);
            e = cv::Point((int)(sx + dx * L), (int)(sy + dy * L));
        }
        build_strokes(a, e, irand(rng_, 8, 14), jnum(cfg_, "jitter", 6.0),
                      jnum(cfg_, "branches", 0.35), b.strokes, 0);
        bolts_.push_back(std::move(b));
    }
    void update_arcs(double dt) {
        bolts_.erase(std::remove_if(bolts_.begin(), bolts_.end(),
            [](const Bolt& b){ return !b.arc; }), bolts_.end());   // drop leftover bolts
        const int want = std::max(1, count(2));        // fewer, deliberate arcs
        while (static_cast<int>(bolts_.size()) < want) {
            Bolt b; b.arc = true;
            const Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{150, 200, 255};
            b.r = col.r; b.g = col.g; b.b = col.b;
            b.a = rand_edge_point(); b.e = rand_edge_point();   // both ends on edges
            b.relocate = frand(rng_, 0.6, 1.8);                  // re-strike interval
            bolts_.push_back(std::move(b));
        }
        while (static_cast<int>(bolts_.size()) > want) bolts_.pop_back();
        const double jit = jnum(cfg_, "jitter", 5.0);
        const double bp  = jnum(cfg_, "branches", 0.30);
        for (auto& b : bolts_) {
            b.age += dt;
            b.alpha = frand(rng_, 0.55, 1.0);          // electrical flicker
            if (b.age >= b.relocate) {                 // re-strike: jump an endpoint to a new edge
                b.age = 0.0; b.relocate = frand(rng_, 0.6, 1.8);
                if (irand(rng_, 0, 1)) b.e = rand_edge_point(); else b.a = rand_edge_point();
            }
            b.strokes.clear();                         // regenerate each frame → crackle
            build_strokes(b.a, b.e, irand(rng_, 6, 12), jit, bp, b.strokes, 0);
        }
    }
};

// ── Bubbles ─────────────────────────────────────────────────────────────────────
// Rising wobbling rings that grow and actually POP — a one-tick expanding ring
// with flung droplets — at the top edge or at end of life, instead of the old
// slow fade-out. Directional (default up). Particle.extra: wobble phase;
// negative = popping.
class BubblesEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double dx, dy; direction_unit(dx, dy, 270.0);        // default up
        for (auto& p : particles_) {
            if (p.extra < 0) { p.life -= dt / p.max_life; continue; }   // popping
            p.extra += dt * 3.0;
            const double wob = std::sin(p.extra) * jnum(cfg_, "wobble", 6.0);
            p.x += (p.vy * dx) * dt + wob * dt;
            p.y += (p.vy * dy) * dt;
            p.size = (int)std::min(6.0, p.size + dt * 1.5);   // grow as they rise
            p.life -= dt / p.max_life;
            if (p.life <= 0.08 || p.y <= p.size + 2.0) {      // surface → pop
                p.extra = -1.0; p.life = 1.0; p.max_life = 0.13;
            }
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return p.life <= 0 ||
                       !(p.x > -8 && p.x < w_ + 8 && p.y > -8 && p.y < h_ + 8);
            }), particles_.end());
        while ((int)particles_.size() < count(14)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{160, 220, 255};
            Particle p;
            direction_spawn_point(2.0, 270.0, p.x, p.y);
            p.vy = pick_speed(cfg_, 8, 18, rng_);
            p.max_life = pick_life(cfg_, 1.5, 3.5, rng_); p.life = 1;
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 2, 4, rng_); p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (const auto& p : particles_) {
            if (p.extra < 0) {                               // pop: burst + droplets
                const double t = 1.0 - std::clamp(p.life, 0.0, 1.0);   // 0 → 1
                const int rad  = p.size + (int)std::lround(t * 3.0);
                const int A    = (int)std::clamp((1.0 - t) * 230.0, 0.0, 255.0);
                cv::circle(c, {(int)p.x, (int)p.y}, std::max(1, rad),
                           cv::Scalar(p.r, p.g, p.b, A), 1, cv::LINE_AA);
                for (int i = 0; i < 3; ++i) {                // flung droplets
                    const double ang = -kPi * 0.5 + (i - 1) * 0.9;
                    const double dr  = rad + 1.0 + t * 2.5;
                    draw_pixel(c, (int)std::lround(p.x + std::cos(ang) * dr),
                                  (int)std::lround(p.y + std::sin(ang) * dr),
                               255, 255, 255, A * 2 / 3);
                }
                continue;
            }
            // Alive bubbles hold steady brightness — real bubbles don't fade,
            // they pop (that's what the state above is for).
            cv::circle(c, {(int)p.x, (int)p.y}, std::max(1, p.size),
                       cv::Scalar(p.r, p.g, p.b, 195), 1, cv::LINE_8);
            const int off = std::max(1, p.size / 2);         // glint tracks size
            draw_pixel(c, (int)p.x - off, (int)p.y - off, 255, 255, 255, 150);
        }
        return c;
    }
};

// ── Fireworks ───────────────────────────────────────────────────────────────────
// Rockets climb from the bottom on a sputtering exhaust, stall at the apex and
// burst into a spark shower. Sparks bloom fast then float (air drag), fall in
// gravity arcs with velocity-aligned streaks, cool white → shell colour →
// ember red, and twinkle out near burnout. "count" ≈ concurrent rockets;
// "burst" = sparks per explosion; "drag" shapes the bloom.
// Particle.extra: >0.5 = rocket; negative = a spark's twinkle phase.
class FireworksEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const double grav = jnum(cfg_, "gravity", 26.0);
        const double drag = jnum(cfg_, "drag", 2.2);
        for (auto& p : particles_) {
            if (p.extra > 0.5) {                       // rocket: ballistic climb
                p.vy += grav * dt;
                if (p.vy >= 0) { explode(p); p.life = 0; continue; }  // apex
            } else {                                   // spark: drag + gravity
                const double k = std::exp(-drag * dt); // bloom fast, then float
                p.vx *= k;
                p.vy  = p.vy * k + grav * dt;
            }
            p.x += p.vx * dt; p.y += p.vy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){ return p.life <= 0 || p.y > h_ + 6; }), particles_.end());
        int rockets = 0;
        for (const auto& p : particles_) if (p.extra > 0.5) ++rockets;
        if (rockets < count(2) && frand(rng_, 0, 1) < 0.06) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{255, 220, 120};
            Particle r;
            r.x = frand(rng_, w_ * 0.2, w_ * 0.8); r.y = h_;
            // Panel-relative launch: pick an apex height inside the panel so the
            // burst is always visible (apex = where vy crosses 0). v = sqrt(2gh).
            const double rise = h_ * frand(rng_, 0.45, 0.72);
            r.vy = -std::sqrt(2.0 * grav * std::max(4.0, rise));
            r.vx = frand(rng_, -3, 3);
            r.r = col.r; r.g = col.g; r.b = col.b; r.size = 1;
            r.max_life = 4.0; r.life = 1; r.extra = 1.0;   // >0.5 == rocket
            particles_.push_back(r);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (const auto& p : particles_) {
            if (p.extra > 0.5) {
                // Rocket: near-white head over a flickering exhaust sputter.
                draw_dot(c, p.x, p.y, 255, 242, 214, 1.0, p.size);
                const double sp = std::hypot(p.vx, p.vy);
                if (sp > 1e-3) {
                    const double ux = p.vx / sp, uy = p.vy / sp;
                    for (int i = 1; i <= 3; ++i)
                        draw_dot(c, p.x - ux * i, p.y - uy * i,
                                 (int)p.r, (int)(p.g * 0.75), (int)(p.b * 0.45),
                                 frand(rng_, 0.2, 1.0) * (1.0 - i * 0.27), 1);
                }
                continue;
            }
            const double life = std::clamp(p.life, 0.0, 1.0);
            // Colour cooling: overexposed white right after the burst, back to
            // the shell colour, then dimming toward ember red at burnout.
            double r, g, b;
            if (life > 0.72) {
                const double t = (life - 0.72) / 0.28;
                r = p.r + (255 - p.r) * t;
                g = p.g + (255 - p.g) * t;
                b = p.b + (255 - p.b) * t;
            } else {
                const double t = 1.0 - life / 0.72;    // 0 shell → 1 ember
                r = p.r + (200.0 - p.r) * t * 0.6;
                g = p.g * (1.0 - 0.80 * t);
                b = p.b * (1.0 - 0.95 * t);
            }
            // Twinkle: strobe once the spark burns low.
            double a = life;
            if (life < 0.38) {
                const double tw = 0.5 + 0.5 * std::sin((1.0 - life) * 55.0
                                                       - p.extra * 7.0);
                a *= 0.35 + 0.65 * tw;
            }
            // Streak along the velocity — long right after the burst,
            // shrinking to a floating dot as drag bleeds the speed off.
            const double sp = std::hypot(p.vx, p.vy);
            const double len = std::min(6.0, sp * 0.09);
            if (len >= 0.8) {
                const double ux = p.vx / sp, uy = p.vy / sp;
                cv::line(c,
                         {(int)std::lround(p.x - ux * len),
                          (int)std::lround(p.y - uy * len)},
                         {(int)std::lround(p.x), (int)std::lround(p.y)},
                         cv::Scalar(r, g, b,
                                    (int)std::clamp(a * 190.0, 0.0, 255.0)),
                         1, cv::LINE_AA);
            }
            draw_dot(c, p.x, p.y, (int)r, (int)g, (int)b, a, p.size);
        }
        return c;
    }
private:
    void explode(Particle& rocket) {
        const int n = jint(cfg_, "burst", 22);
        // Scale the burst radius to the panel so the shower stays on-screen.
        const double vmax = jnum(cfg_, "burst_speed", std::min(w_, h_) * 0.45);
        for (int i = 0; i < n; ++i) {
            const double ang = kTau * i / n + frand(rng_, -0.1, 0.1);
            const double spd = frand(rng_, vmax * 0.25, vmax);
            Particle s;
            s.x = rocket.x; s.y = rocket.y;
            s.vx = std::cos(ang) * spd; s.vy = std::sin(ang) * spd;
            s.r = rocket.r; s.g = rocket.g; s.b = rocket.b; s.size = 1;
            s.max_life = frand(rng_, 0.5, 1.1); s.life = 1;
            s.extra = -frand(rng_, 0.5, 7.0);          // negative = twinkle phase
            particles_.push_back(s);
        }
    }
};

// ── Vortex ──────────────────────────────────────────────────────────────────────
// A whirlpool: thin streamline streaks spiralling into a central drain.
// Differential rotation (inner turns faster, ~1/r^0.8) and infall that
// accelerates toward the centre are what make it read as swirling fluid
// rather than a rigid ring of comets; each streak is the particle's own
// recent path traced backwards through the same flow field, so inner streaks
// wind tight and long while outer ones stay short and lazy.
// "swirl" sets nominal angular speed; negative "infall" spirals outward.
class VortexEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;

    // Flow field, shared by the integrator and the streak tracer. rref is the
    // radius where the angular speed equals the configured "swirl".
    static double ang_speed(double swirl, double rref, double r) {
        const double w = swirl * std::pow(rref / std::max(3.0, r), 0.8);
        return std::clamp(w, -std::abs(swirl) * 6.0, std::abs(swirl) * 6.0);
    }
    static double infall_speed(double infall, double rref, double r) {
        // ×3 on the configured rate plus a strong 1/r pull: with the presets'
        // infall=7 a streak crosses the whole disc in ~4 s and takes its final
        // plunge fast — the old constant 7 px/s never got a particle to the
        // drain inside its lifetime, which is why nothing ever spiralled in.
        return infall * 3.0 * (0.4 + 0.6 * (rref / std::max(3.0, r)));
    }

    void update(double dt) override {
        const double swirl  = jnum(cfg_, "swirl", 2.2);
        const double infall = jnum(cfg_, "infall", 6.0);
        // Span the whole canvas (reaches the corners) so it's not a small blob
        // stuck in the middle. cw_/ch_ default to this panel when no canvas set.
        const double maxr = jnum(cfg_, "max_radius", std::hypot(cw_, ch_) * 0.55);
        const double rref = maxr * 0.45;
        for (auto& p : particles_) {
            p.extra += ang_speed(swirl, rref, p.vy) * dt;      // angle
            p.vy    -= infall_speed(infall, rref, p.vy) * dt;  // radius (reuse vy)
            p.life  -= dt / p.max_life;
        }
        // Die at a small finite radius — the drain's "eye" swallows streaks
        // instead of letting them pile up as dots at the exact centre.
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [](const Particle& p){ return p.life <= 0 || p.vy <= 2.5; }), particles_.end());
        while ((int)particles_.size() < count(60)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{120, 180, 255};
            Particle p;
            p.extra = frand(rng_, 0, kTau);                    // angle
            // Uniform in radius (not area) — infall dwells longest at the rim,
            // so seeding evenly keeps the middle of the funnel populated.
            p.vy    = maxr * (0.15 + 0.85 * frand(rng_, 0, 1));
            // Long default life: a streak should DIE AT THE DRAIN (r <= 2.5
            // above), not fade mid-journey — the completed spiral is the
            // whole whirlpool read. Presets can still set "life".
            p.max_life = pick_life(cfg_, 5.0, 9.0, rng_); p.life = 1;
            p.r = col.r; p.g = col.g; p.b = col.b; p.size = pick_size(cfg_, 1, 2, rng_);
            particles_.push_back(p);
        }
    }

    cv::Mat render() override {
        cv::Mat c = blank();
        // Centre on the whole canvas; render this panel's slice (continuous
        // across a multi-panel face). Each streak is drawn by re-integrating
        // the particle's path BACKWARDS through the flow for a fixed time
        // window — thin anti-aliased line segments that taper and fade, no
        // bright comet head. Fast inner particles cover more arc in the same
        // window, which is exactly the tight winding a whirlpool shows.
        const double cx = cw_ * 0.5, cy = ch_ * 0.5;
        const double swirl  = jnum(cfg_, "swirl", 2.2);
        const double infall = jnum(cfg_, "infall", 6.0);
        const double maxr   = jnum(cfg_, "max_radius", std::hypot(cw_, ch_) * 0.55);
        const double rref   = maxr * 0.45;
        const double tail_s = jnum(cfg_, "tail_s", 0.30);   // streak time window
        const int    K      = 12;                           // segments per streak
        const double dt2    = tail_s / K;
        for (const auto& p : particles_) {
            const double life = std::clamp(p.life, 0.0, 1.0);
            // Ease in after spawn and out before death so streaks surface and
            // sink inside the flow rather than popping.
            const double fade = std::min(1.0, life * 3.0) *
                                std::min(1.0, (1.0 - life) * 6.0 + 0.15);
            if (fade <= 0.02) continue;
            double a = p.extra, r = std::max(2.0, p.vy);
            double px = cx + std::cos(a) * r - ox_;
            double py = cy + std::sin(a) * r - oy_;
            for (int k = 1; k <= K; ++k) {
                a -= ang_speed(swirl, rref, r) * dt2;          // backwards in time
                r += infall_speed(infall, rref, r) * dt2;      //   = back outward
                const double qx = cx + std::cos(a) * r - ox_;
                const double qy = cy + std::sin(a) * r - oy_;
                const double t  = static_cast<double>(k) / K;  // 0 head … 1 tail
                const int A = (int)std::clamp(
                    fade * std::pow(1.0 - t, 1.35) * 235.0, 0.0, 255.0);
                if (A > 2) {
                    const int th = std::max(
                        1, (int)std::lround(p.size * (1.0 - 0.6 * t)));
                    cv::line(c, {(int)std::lround(px), (int)std::lround(py)},
                                {(int)std::lround(qx), (int)std::lround(qy)},
                             cv::Scalar(p.r, p.g, p.b, A), th, cv::LINE_AA);
                }
                px = qx; py = qy;
            }
        }
        return c;
    }
};

// ── Water / liquid fill ─────────────────────────────────────────────────────────
// The panel looks partially filled with a liquid. The surface stays level in
// world space — so it tilts on the panel as you roll your head (motion_.roll_deg)
// — and sloshes when the gyro/accel kicks (motion_.yaw_rate / accel_g). Multiple
// "colors" form a deep→surface gradient. Best with "blend":"normal" (opaque
// liquid); use "add" for a glowy lava/plasma look.
//   level (0..1) fill fraction · alpha · tilt_gain · slosh (px) · wave_count
//   · wave_speed · viscosity (0..1 thicker=sluggish) · pitch_fill (head pitch
//   shifts level) · bubbles (count) · bubble_mode ("rise" in liquid | "drip"
//   droplets above the surface)
class WaterEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        if (dt <= 0.0) return;
        dt = std::min(dt, 0.05);   // clamp big frame gaps so the slosh solver stays stable
        // viscosity 0 = thin/snappy water, 1 = thick/sluggish (lags + resists
        // slosh). Default 0.15 = plain water: fast waves, long ring-down.
        // Thick liquids set their own (lava 0.6, mercury 0.7).
        const double v = std::clamp(jnum(cfg_, "viscosity", 0.15), 0.0, 1.0);
        if (!tilt_init_) {
            tilt_smooth_  = motion_.roll_deg;
            pitch_smooth_ = motion_.pitch_deg;
            tilt_init_ = true;
        }
        // The "resting" lean follows head roll (gravity); a thicker liquid lags.
        const double rate = std::min(1.0, dt * 6.0 * (1.0 - 0.85 * v));
        tilt_smooth_  += (motion_.roll_deg  - tilt_smooth_)  * rate;
        pitch_smooth_ += (motion_.pitch_deg - pitch_smooth_) * rate;

        // Sloshing as a damped spring on BOTH axes: slosh_x_ is the height (px)
        // the fluid piles up at a side wall (head roll), slosh_y_ is the vertical
        // bob from nodding (head pitch). Both overshoot and settle like a real
        // liquid. "slosh" (≈6 nominal) scales how hard tilts drive the fluid.
        const double sloshg = jnum(cfg_, "slosh", 6.0) / 6.0;   // drive gain (0=still)
        const double k = 34.0 * (1.0 - 0.45 * v);     // stiffness → slosh frequency
        const double c = 5.0 + 14.0 * v;              // damping → thicker settles faster
        const double drive_x = ((motion_.roll_deg - tilt_smooth_) * 0.9
                              + motion_.yaw_rate * 0.025
                              + (motion_.accel_g - 1.0) * 7.0) * sloshg;
        const double accel_x = -k * slosh_x_ - c * slosh_v_ + drive_x * (h_ * 0.02);
        slosh_v_ += accel_x * dt;
        slosh_x_  = std::clamp(slosh_x_ + slosh_v_ * dt, -h_ * 0.5, h_ * 0.5);

        // Vertical slosh: quick pitch (nod) and fore/aft accel pump the level up
        // and down before it settles — the y-axis counterpart to the roll lean.
        const double drive_y = ((motion_.pitch_deg - pitch_smooth_) * 0.9
                              + (motion_.accel_g - 1.0) * 5.0) * sloshg;
        const double accel_y = -k * slosh_y_ - c * slosh_vy_ + drive_y * (h_ * 0.02);
        slosh_vy_ += accel_y * dt;
        slosh_y_   = std::clamp(slosh_y_ + slosh_vy_ * dt, -h_ * 0.5, h_ * 0.5);

        // Container geometry: a volume-conserving rest surface (see
        // compute_rest) — the liquid pivots around the fill line as the head
        // tilts, pools corner-to-corner at high tilt and leaves the high side
        // dry, like water in a real tank. The wave field rides on top, and
        // rest-surface motion is injected into it (surface continuity), so
        // every head movement makes real traveling waves.
        compute_rest();
        inject_impulses(dt);

        // Surface dynamics: a 1-D wave-equation heightfield (see
        // step_heightfield) replaces the old rigid tilt-plane + global sine
        // chop — waves now propagate, reflect off the side walls (that IS the
        // slosh) and interfere, so the surface reads as liquid.
        step_heightfield(dt, v);

        update_bubbles(dt);
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        const std::vector<Color> cols = read_colors();
        const double alpha = std::clamp(jnum(cfg_, "alpha", 0.85), 0.0, 1.0);
        const double sheen_k = std::clamp(jnum(cfg_, "sheen", 0.55), 0.0, 1.0);
        const double bottom = ch_ - 1;                 // canvas-space deepest row
        for (int lx = 0; lx < w_; ++lx) {
            const double sy = surface_y_local(lx) + oy_;    // canvas-space surface (float)
            const double span = std::max(1.0, bottom - sy);
            for (int ly = 0; ly < h_; ++ly) {
                const double Y = oy_ + ly;              // top edge of this pixel
                // Sub-pixel coverage: how much of this pixel sits below the
                // surface. Soft top edge → no cartoony stair-stepping.
                const double cover = std::clamp((Y + 1.0) - sy, 0.0, 1.0);
                if (cover <= 0.0) continue;
                const double depth = std::clamp((Y - sy) / span, 0.0, 1.0);
                Color col = sample_grad(cols, depth);
                // Specular sheen: lighten the first few px under the surface so it
                // catches the light like a real meniscus, fading with depth.
                // "sheen" scales the glint (mercury shiny, lava matte).
                const double sheen = std::exp(-std::max(0.0, Y - sy) / 2.2) * sheen_k;
                col.r = (int)std::lround(col.r + (255 - col.r) * sheen);
                col.g = (int)std::lround(col.g + (255 - col.g) * sheen);
                col.b = (int)std::lround(col.b + (255 - col.b) * sheen);
                draw_pixel(c, lx, ly, col.r, col.g, col.b,
                           (int)std::lround(alpha * 255.0 * cover));
            }
        }
        render_bubbles(c, cols, (int)(alpha * 255.0));
        return c;
    }
    // How strongly the face should glow back through the liquid (0 = opaque).
    double face_glow() const override {
        // Default ON for liquids: a bare "water" layer (Single Effects) lets
        // the face shine through at 0.55 like the curated presets do; set
        // face_glow: 0 in the layer cfg for an opaque liquid.
        return std::clamp(jnum(cfg_, "face_glow", 0.55), 0.0, 1.0);
    }
private:
    // Effective fill fraction — base level shifted by smoothed pitch (look
    // down → liquid rises) when pitch_fill is set.
    double level_eff() const {
        return std::clamp(base_level()
                          // Default ON (0.3): in a 3-D container, pitching
                          // forward brings the liquid toward you — reads as
                          // the level rising. pitch_fill: 0 restores flat.
                          + jnum(cfg_, "pitch_fill", 0.3) * (pitch_smooth_ / 45.0),
                          0.0, 1.0);
    }
    // Base (untilted) fill fraction. Normally the static "level", but with
    // "level_from":"humidity" the tank fills with the room's relative humidity:
    // the reading (0..1) is mapped onto [level_min, level_max] so a dry room
    // isn't bone-empty nor a muggy one overflowing. Falls back to "level" when
    // no sensor reading has arrived yet.
    double base_level() const {
        if (cfg_.value("level_from", std::string("none")) == "humidity" &&
            humidity_ >= 0.0) {
            const double lo = std::clamp(jnum(cfg_, "level_min", 0.10), 0.0, 1.0);
            const double hi = std::clamp(jnum(cfg_, "level_max", 0.90), 0.0, 1.0);
            return lo + (hi - lo) * std::clamp(humidity_, 0.0, 1.0);
        }
        return jnum(cfg_, "level", 0.40);
    }
    // Local surface row at local column lx, computed in canvas space (so the
    // tank is continuous across panels) then shifted into this panel's frame.
    double surface_y_local(double lx) const {
        // Water height above the tank floor = volume-conserving rest surface
        // + wave deviation, sampled with linear interpolation so sub-pixel
        // columns and bubbles get a smooth surface.
        const double X  = ox_ + lx;
        const double fx = std::clamp(X, 0.0, (double)(cw_ - 1));
        const int    x0 = (int)fx;
        const int    x1 = std::min(x0 + 1, cw_ - 1);
        const double t  = fx - x0;
        double wh = 0.0;
        if (!rest_px_.empty())
            wh += rest_px_[x0] + (rest_px_[x1] - rest_px_[x0]) * t;
        if (!hf_.empty())
            wh += hf_[x0] + (hf_[x1] - hf_[x0]) * t;
        if (wh <= 0.05) return (double)ch_ + 2.0 - oy_;   // dry column
        double syc = ch_ - wh;
        // Meniscus: the surface curls up where it meets the container walls.
        const double menisc = jnum(cfg_, "meniscus", 1.5);
        if (menisc > 0.0) {
            const double dl = X, dr = (cw_ - 1) - X;
            syc -= menisc * (std::exp(-dl / 2.5) + std::exp(-dr / 2.5));
        }
        return syc - oy_;
    }
    void update_bubbles(double dt) {
        const int want = jint(cfg_, "bubbles", 0);
        if (want <= 0) { bubbles_.clear(); return; }
        const bool drip = (cfg_.value("bubble_mode", std::string("rise")) == "drip");
        const double spd = jnum(cfg_, "bubble_speed", drip ? 16.0 : 12.0);
        for (auto& b : bubbles_) {
            b.extra += dt * 2.0;
            if (!drip) b.x += std::sin(b.extra) * 6.0 * dt;      // bubbles wobble as they rise
            b.y += (drip ? spd : -spd) * dt;
            const double sy = surface_y_local(b.x);
            if (drip ? (b.y >= sy) : (b.y <= sy)) b.life = 0;    // hit surface → gone/pop
        }
        bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(),
            [&](const Particle& b){
                return b.life <= 0 || b.x < -2 || b.x > w_ + 2 || b.y < -4 || b.y > h_ + 4;
            }), bubbles_.end());
        int guard = 0;
        while ((int)bubbles_.size() < want && guard++ < want * 3) {
            const double bx = frand(rng_, 0, w_ - 1);
            const double sy = surface_y_local(bx);
            Particle p; p.x = bx; p.size = irand(rng_, 1, 2);
            p.extra = frand(rng_, 0, kTau); p.life = 1;
            if (drip) {
                if (sy <= 1.0) continue;                          // no room above the liquid here
                p.y = frand(rng_, 0.0, std::max(1.0, sy - 1.0));
            } else {
                if (sy >= h_ - 1.0) continue;                     // no liquid in this column
                p.y = frand(rng_, sy + 1.0, static_cast<double>(h_));
            }
            bubbles_.push_back(p);
        }
    }
    void render_bubbles(cv::Mat& c, const std::vector<Color>& cols, int A) {
        if (bubbles_.empty()) return;
        const bool drip = (cfg_.value("bubble_mode", std::string("rise")) == "drip");
        const Color base = cols.front();
        const Color bcol{ std::min(255, base.r + 110), std::min(255, base.g + 110),
                          std::min(255, base.b + 110) };
        for (const auto& b : bubbles_) {
            if (drip) {                                           // droplet above the surface
                draw_dot(c, b.x, b.y, bcol.r, bcol.g, bcol.b, 0.85, b.size);
            } else {                                              // bubble ring inside the liquid
                cv::circle(c, {(int)b.x, (int)b.y}, std::max(1, b.size),
                           cv::Scalar(bcol.r, bcol.g, bcol.b, std::min(255, A + 30)),
                           1, cv::LINE_8);
            }
        }
    }
    std::vector<Color> read_colors() const {
        std::vector<Color> v;
        auto it = cfg_.find("colors");
        if (it != cfg_.end() && it->is_array() && !it->empty()) {
            for (const auto& c : *it)
                if (c.is_array() && c.size() >= 3)
                    v.push_back({ c[0].get<int>(), c[1].get<int>(), c[2].get<int>() });
        } else {
            auto c = cfg_.find("color");
            if (c != cfg_.end() && c->is_array() && c->size() >= 3)
                v.push_back({ (*c)[0].get<int>(), (*c)[1].get<int>(), (*c)[2].get<int>() });
        }
        if (v.empty()) { v.push_back({120, 220, 255}); v.push_back({0, 80, 200}); }
        return v;
    }
    // 1-D wave-equation surface: per-canvas-column height (px, + = lower)
    // and vertical velocity. Reflective walls make waves bounce back —
    // that reflection is the slosh. Forced toward the gravity/slosh tilt
    // line, with random micro-impulses for idle ripples and churn.
    std::vector<double> hf_, hv_;
    // Volume-conserving rest surface — the liquid as a fixed amount of water
    // in the visor "tank". rest_px_[x] is the resting water HEIGHT above the
    // tank floor per canvas column. The surface is the tilt plane clipped to
    // the tank, with its offset bisected until the clipped surface holds the
    // fill volume: at small tilt it pivots around the fill line (one side up,
    // other down), at large tilt it pools in the low corner and the high side
    // runs dry — no artificial slope limit, only the real floor/ceiling.
    void compute_rest() {
        if ((int)rest_px_.size() != cw_) rest_px_.assign(cw_, 0.0);
        const double H    = (double)ch_;
        const double half = std::max(1.0, cw_ * 0.5);
        const double ccx  = cw_ * 0.5;
        const double gain = jnum(cfg_, "tilt_gain", 1.0);
        const double roll = std::clamp(tilt_smooth_ * gain * kPi / 180.0,
                                       -1.45, 1.45);      // ±83°: tan stays sane
        // Static gravity lean + the dynamic slosh pile-up as extra slope.
        const double s = std::tan(roll) + slosh_x_ / half;
        // Mean water height: fill level shifted by the vertical (pitch) bob.
        const double m = std::clamp(level_eff() * H - slosh_y_, 0.0, H);
        // Bisect the plane offset until the tank-clipped surface holds the
        // volume (mean == m). Monotonic in o, ~30 rounds ≈ sub-0.001 px.
        const double span = half * std::fabs(s);
        double lo = -span - 1.0, hi = H + span + 1.0;
        for (int it = 0; it < 30; ++it) {
            const double o = 0.5 * (lo + hi);
            double sum = 0.0;
            for (int x = 0; x < cw_; ++x)
                sum += std::clamp(o + ((x + 0.5) - ccx) * s, 0.0, H);
            ((sum / cw_) < m ? lo : hi) = o;
        }
        const double o = 0.5 * (lo + hi);
        // Surface continuity: the rest surface just moved (head motion), but
        // real liquid doesn't teleport — inject the difference into the wave
        // field so the ABSOLUTE surface stays where it was and sloshes its
        // way to the new rest. Both surfaces hold the same volume, so the
        // injection sums to ~zero and conservation is exact. wave_gain
        // scales how much of the motion becomes waves (0 = old glide).
        const double wgain = std::clamp(jnum(cfg_, "wave_gain", 1.0), 0.0, 2.0);
        const bool   inject = wgain > 0.0 && (int)hf_.size() == cw_ && rest_init_;
        for (int x = 0; x < cw_; ++x) {
            const double nr = std::clamp(o + ((x + 0.5) - ccx) * s, 0.0, H);
            if (inject) {
                // 1.35: slight inertial overshoot — the free surface doesn't
                // just stay put, it swings past, like real liquid.
                const double d = std::clamp((rest_px_[x] - nr) * wgain * 1.35,
                                            -H * 0.5, H * 0.5);
                hf_[x] = std::clamp(hf_[x] + d, -nr, H - nr);
            }
            rest_px_[x] = nr;
        }
        rest_init_ = true;
    }

    // Impulse wave sources: vertical jolts splash the whole surface,
    // fast turns push a traveling wave off the trailing wall.
    void inject_impulses(double dt) {
        if (hf_.empty() || rest_px_.empty()) return;
        const double wgain = std::clamp(jnum(cfg_, "wave_gain", 1.0), 0.0, 2.0);
        if (wgain <= 0.0) return;
        // Jolt splash: g-deviation beyond ~0.12 g rains random dips, more and
        // deeper the harder the hit (jump, landing, headbang).
        const double jolt = std::fabs(motion_.accel_g - 1.0);
        if (jolt > 0.12 && cw_ > 4) {
            const double amt = std::min(jolt - 0.12, 0.8) * wgain;
            const int n = 1 + (int)(amt * 6.0 * (cw_ / 64.0) * std::min(1.0, dt * 60.0));
            for (int i = 0; i < n; ++i) {
                const int x = irand(rng_, 1, cw_ - 2);
                if (rest_px_[x] < 1.0) continue;               // dry side
                const double d = frand(rng_, 0.8, 2.4) * (0.5 + amt);
                hf_[x] -= d; hf_[x - 1] -= d * 0.5; hf_[x + 1] -= d * 0.5;
            }
        }
        // Turn swish: a quick yaw sweep drags the liquid — velocity impulse
        // at the trailing wall becomes a wave that travels across the tank.
        const double yr = motion_.yaw_rate;
        if (std::fabs(yr) > 50.0 && (int)hv_.size() == cw_ && cw_ > 6) {
            const double push = std::clamp((std::fabs(yr) - 50.0) / 300.0, 0.0, 1.0)
                                * 140.0 * wgain * dt;
            const int wall = (yr > 0) ? 0 : cw_ - 1;
            const int dir  = (yr > 0) ? 1 : -1;
            for (int i = 0; i < 8 && i < cw_; ++i) {
                const int x = wall + dir * i;
                if (rest_px_[x] > 1.0) hv_[x] += push * (1.0 - i * 0.11);
            }
        }
    }

    void step_heightfield(double dt, double viscosity) {
        if ((int)hf_.size() != cw_) { hf_.assign(cw_, 0.0); hv_.assign(cw_, 0.0); }
        if ((int)rest_px_.size() != cw_) compute_rest();
        // Propagation speed (px/s): wave_speed keeps its old knob meaning
        // (bigger = livelier surface); thick liquid carries slower waves.
        const double c      = jnum(cfg_, "wave_speed", 2.0) * 22.0
                              * (1.0 - 0.55 * viscosity);
        const double c2     = c * c;
        const double damp   = 1.0 + 6.0 * viscosity;
        // Relaxation toward rest: soft enough that motion-injected waves
        // survive to travel and reflect (that IS the slosh) before settling.
        const double k_pull = 14.0;
        const double H      = (double)ch_;
        // Idle ripples + churn while sloshing: random micro-impulses that the
        // wave equation spreads into expanding rings.
        const double rate = jnum(cfg_, "ripple", 0.8) + std::fabs(slosh_v_) * 0.25;
        // Sub-step for stability (CFL: c*dts must stay well under 1 px).
        const int    n   = std::max(1, (int)std::ceil(dt / 0.008));
        const double dts = dt / n;
        for (int st = 0; st < n; ++st) {
            // Expected `rate` drops per second (scaled with tank width):
            // each pokes a 3-column dip the wave equation spreads outward.
            // Only wet columns ripple — no rain on the dry side of the tank.
            if (cw_ > 3 && frand(rng_, 0.0, 1.0) < rate * (cw_ / 64.0) * dts) {
                const int x = irand(rng_, 1, cw_ - 3);
                if (rest_px_[x] > 1.0) {
                    const double d = frand(rng_, 0.9, 2.2);
                    hf_[x]     -= d;
                    hf_[x + 1] -= d * 0.5;
                    hf_[x > 0 ? x - 1 : 0] -= d * 0.5;
                }
            }
            // hf_ is the wave DEVIATION from the rest surface (rest is
            // piecewise-linear, so its Laplacian is ~0 and the wave equation
            // on the deviation matches the one on the full surface).
            for (int x = 0; x < cw_; ++x) {
                const double hl  = hf_[x > 0 ? x - 1 : 0];        // reflective
                const double hr  = hf_[x < cw_ - 1 ? x + 1 : cw_ - 1];
                const double lap = hl + hr - 2.0 * hf_[x];
                hv_[x] += (c2 * lap - k_pull * hf_[x]) * dts
                          - damp * hv_[x] * dts;
            }
            // The only hard rails are the tank itself: total water height
            // stays within [floor, ceiling]. (The old ±0.6·H deviation clamp
            // pinned the surface into flat plateaus past ~17° of roll.)
            for (int x = 0; x < cw_; ++x)
                hf_[x] = std::clamp(hf_[x] + hv_[x] * dts,
                                    -rest_px_[x], H - rest_px_[x]);
        }
    }
    double tilt_smooth_ = 0.0, pitch_smooth_ = 0.0;
    double slosh_x_ = 0.0, slosh_v_  = 0.0;  // roll lean (px) + velocity
    double slosh_y_ = 0.0, slosh_vy_ = 0.0;  // pitch bob (px) + velocity
    bool   tilt_init_ = false;
    bool   rest_init_ = false;               // first compute_rest done (no inject)
    std::vector<double> rest_px_;            // volume-conserving rest surface
    std::vector<Particle> bubbles_;
};

// ── Snooze ───────────────────────────────────────────────────────────────────
// Floating Z's: little Z glyphs drift up from the mouth area, swaying as they
// rise, growing a step and fading out — the classic cartoon sleep marker. The
// asleep reaction drives this as its ambient effect; also usable standalone.
// cfg: count (default 3), speed_min/max (rise px/s), colors (default soft blue-white).

class SnoozeEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        for (auto& p : particles_) {
            p.extra += dt;
            p.y -= p.vy * dt;                                   // rise
            p.x += (p.vx * 0.35 + std::sin(p.extra * 1.8) * 2.2) * dt;  // drift + sway
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){ return p.life <= 0 || p.y < -8; }),
            particles_.end());
        while ((int)particles_.size() < count(3)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_)
                                         : Color{200, 220, 255};
            Particle p;
            // Spawn near the mouth: bottom-centre of the whole canvas, in
            // canvas space so mirrored panels share the same source.
            p.x = cw_ * 0.5 + frand(rng_, -cw_ * 0.10, cw_ * 0.10) - ox_;
            p.y = ch_ * 0.80 + frand(rng_, -2.0, 2.0) - oy_;
            p.vx = frand(rng_, 2.0, 7.0) * (frand(rng_, 0, 1) < 0.5 ? -1.0 : 1.0);
            p.vy = pick_speed(cfg_, 4.0, 8.0, rng_);
            p.max_life = frand(rng_, 2.2, 3.6);
            p.life = 1.0;
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        // Glyph size / stroke are configurable. Bigger box + thin 1 px sharp
        // strokes (LINE_8, no AA), full-bright: the larger box gives the Z
        // open counter-spaces so the thin strokes read clearly, without the
        // chunky blob a thick stroke made on a small glyph.
        const int base_s = std::max(2, jint(cfg_, "size", 3));       // half-size px
        const int thick  = std::max(1, jint(cfg_, "thickness", 1));  // stroke px
        for (auto& p : particles_) {
            // Fade in quickly, out slowly; grow one size step mid-flight.
            const double env = std::clamp(std::min((1.0 - p.life) * 5.0,
                                                   p.life * 2.5), 0.0, 1.0);
            const int a = (int)std::lround(env * 255.0);             // full-bright
            if (a <= 0) continue;
            const int s = base_s + (p.life < 0.55 ? 1 : 0);         // grow one step as it rises
            const int x = (int)p.x, y = (int)p.y, w = 2 * s;
            const cv::Scalar col(p.r, p.g, p.b, a);
            cv::line(c, {x, y},         {x + w, y},         col, thick);  // top bar
            cv::line(c, {x + w, y},     {x, y + w},         col, thick);  // diagonal
            cv::line(c, {x, y + w},     {x + w, y + w},     col, thick);  // bottom bar
        }
        return c;
    }
};

// ── Star field (3-D parallax) ────────────────────────────────────────────────
// Stars stream outward from the full-canvas centre (the origin) toward the
// panel edges (the far plane). Each star has a fixed world direction (vx/vy in
// [-1,1]) and a depth z in `extra`; depth shrinks each frame so the projection
// screen = centre + dir * fov / z pushes it out, brightening and growing as it
// "approaches". The centre uses cw_/ch_ (full canvas) so the origin stays put
// even when the canvas is split across panels; positions convert to panel-local
// via -ox_/-oy_. Stars recycle to the far depth once they leave the canvas.

class StarfieldEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const double cx = cw_ * 0.5, cy = ch_ * 0.5;
        const double fov = jnum(cfg_, "fov", cw_ * 0.5);
        const int size_max = jint(cfg_, "size_max", 2);
        const double zfar = kZFar, znear = znear_();
        const double span = std::max(1e-3, zfar - znear);
        for (auto& p : particles_) {
            p.extra -= p.max_life * dt;          // approach: depth shrinks
            bool recycle = p.extra <= znear;
            double gx = 0, gy = 0;
            if (!recycle) {
                const double f = fov / p.extra;
                gx = cx + p.vx * f;
                gy = cy + p.vy * f;
                if (gx < -2 || gx > cw_ + 1 || gy < -2 || gy > ch_ + 1) recycle = true;
            }
            if (recycle) {
                respawn(p);
                const double f = fov / p.extra;
                gx = cx + p.vx * f;
                gy = cy + p.vy * f;
            }
            p.x = gx - ox_;                      // full-canvas → panel-local
            p.y = gy - oy_;
            const double near = std::clamp((zfar - p.extra) / span, 0.0, 1.0);
            p.life = near;
            p.size = (size_max > 1 && near > 0.75) ? size_max : 1;
        }
        while ((int)particles_.size() < count(70)) particles_.push_back(spawn());
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (const auto& p : particles_)
            draw_dot(c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, 0.2 + 0.8 * p.life, p.size);
        return c;
    }

protected:
    static constexpr double kZFar = 8.0;
    virtual double znear_() const { return 0.55; }
    virtual void speed_range(double& lo, double& hi) const { lo = 1.2; hi = 3.0; }

    Color pick() {
        if (has_colors(cfg_)) return pick_color(cfg_, rng_);
        static const Color pool[4] = {{255,255,255},{200,220,255},{255,240,210},{210,235,255}};
        return pool[irand(rng_, 0, 3)];
    }
    void respawn(Particle& p) {
        double wx = frand(rng_, -1, 1), wy = frand(rng_, -1, 1);
        while (std::fabs(wx) < 0.05 && std::fabs(wy) < 0.05) {   // avoid dead centre
            wx = frand(rng_, -1, 1); wy = frand(rng_, -1, 1);
        }
        p.vx = wx; p.vy = wy; p.extra = kZFar;
        double lo, hi; speed_range(lo, hi);
        p.max_life = pick_speed(cfg_, lo, hi, rng_);
        Color col = pick(); p.r = col.r; p.g = col.g; p.b = col.b;
        p.life = 0;
    }
    Particle spawn() {
        Particle p; respawn(p);
        p.extra = frand(rng_, znear_() + 0.5, kZFar);   // spread depths on first fill
        return p;
    }
};

// ── Warp (hyperspace) ────────────────────────────────────────────────────────
// The star field cranked to "jump to lightspeed": faster stars smeared into
// radial streaks that point back to the centre and lengthen as they near the
// edge. Reuses the star field's projection/update via inheritance.

class WarpEffect : public StarfieldEffect {
public:
    using StarfieldEffect::StarfieldEffect;
    cv::Mat render() override {
        cv::Mat c = blank();
        const double cx = cw_ * 0.5 - ox_, cy = ch_ * 0.5 - oy_;   // panel-local centre
        const double gain = jnum(cfg_, "streak", 1.0);
        for (const auto& p : particles_) {
            const double alpha = 0.25 + 0.75 * p.life;
            double dx = p.x - cx, dy = p.y - cy;
            double dist = std::hypot(dx, dy); if (dist < 1e-6) dist = 1.0;
            const double ux = dx / dist, uy = dy / dist;           // outward unit
            const double length = (1.0 + p.life * 6.0) * gain;     // longer near edge
            const int n = std::max(1, (int)length);
            for (int i = 0; i <= n; ++i) {                         // streak toward centre
                const double t = (double)i / n;
                const double a = alpha * (1.0 - t);
                if (a <= 0) break;
                draw_dot(c, p.x - ux * length * t, p.y - uy * length * t,
                         (int)p.r, (int)p.g, (int)p.b, a, i == 0 ? p.size : 1);
            }
        }
        return c;
    }
protected:
    double znear_() const override { return 0.4; }
    void speed_range(double& lo, double& hi) const override { lo = 2.5; hi = 5.5; }
};

// ── Constellation (still twinkling sky) ──────────────────────────────────────
// Fixed stars that breathe brightness on independent phases/rates; a fraction
// are "bright" and grow a soft cross glint at their peak. Nothing moves off
// screen, so no centre/edge geometry — stars fill the panel like sparkle.

class ConstellationEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        // vx = twinkle rate, vy = base brightness, extra = phase, size 2 = bright.
        for (auto& p : particles_) {
            p.extra += p.vx * dt;
            p.life = p.vy * (0.45 + 0.55 * (0.5 + 0.5 * std::sin(p.extra)));
        }
        while ((int)particles_.size() < count(50)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : default_pick();
            const bool bright = frand(rng_, 0, 1) < jnum(cfg_, "bright_frac", 0.15);
            Particle p;
            p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, 0, h_ - 1);
            p.vx = frand(rng_, jnum(cfg_, "twinkle_min", 0.8), jnum(cfg_, "twinkle_max", 3.0));
            p.vy = bright ? 1.0 : frand(rng_, 0.4, 0.85);
            p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = bright ? 2 : 1; p.max_life = 9999; p.life = 1;
            p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (const auto& p : particles_) {
            const double alpha = std::clamp(p.life, 0.0, 1.0);
            const int r = (int)p.r, g = (int)p.g, b = (int)p.b;
            draw_dot(c, p.x, p.y, r, g, b, alpha, 1);
            if (p.size >= 2 && alpha > 0.55) {                 // soft cross glint at peak
                const double a2 = (alpha - 0.55) * 1.5;
                draw_dot(c, p.x + 1, p.y, r, g, b, a2, 1);
                draw_dot(c, p.x - 1, p.y, r, g, b, a2, 1);
                draw_dot(c, p.x, p.y + 1, r, g, b, a2, 1);
                draw_dot(c, p.x, p.y - 1, r, g, b, a2, 1);
            }
        }
        return c;
    }
private:
    Color default_pick() {
        static const Color pool[4] = {{255,255,255},{200,220,255},{255,240,210},{255,255,235}};
        return pool[irand(rng_, 0, 3)];
    }
};

// ── Shooting stars (meteors from centre) ─────────────────────────────────────
// Sparse meteors that launch from the full-canvas centre (the origin) out to a
// panel edge with a fading tail, each flying radially from the centre.

class ShootingStarsEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        const double cx = cw_ * 0.5, cy = ch_ * 0.5;
        const double tail = jnum(cfg_, "tail", 8.0);
        for (auto& p : particles_) {
            p.x += p.vx * dt; p.y += p.vy * dt; p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                const double gx = p.x + ox_, gy = p.y + oy_;       // back to full canvas
                return !(p.life > 0 && gx >= -tail && gx <= cw_ + tail
                         && gy >= -tail && gy <= ch_ + tail);
            }), particles_.end());
        const double rate = jnum(cfg_, "rate", 0.8) * intensity_;
        acc_ += dt * rate;
        const int cap = count(3);
        while (acc_ >= 1.0) {
            acc_ -= 1.0;
            if ((int)particles_.size() < cap) particles_.push_back(spawn(cx, cy));
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        const double tail = jnum(cfg_, "tail", 8.0);
        for (const auto& p : particles_) {
            const double sp = std::hypot(p.vx, p.vy);
            if (sp < 1e-3) continue;
            const double ux = p.vx / sp, uy = p.vy / sp;
            const double head = std::clamp(p.life * 1.5, 0.0, 1.0);
            // Tapered AA streak instead of the old chain of dots, with a
            // brighter head pixel.
            draw_streak(c, p.x, p.y, ux, uy, tail, p.r, p.g, p.b, head * 0.85);
            draw_dot(c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, head, p.size);
        }
        return c;
    }
private:
    double acc_ = 0.0;
    Particle spawn(double cx, double cy) {
        const double ang = frand(rng_, 0, kTau);
        const double spd = pick_speed(cfg_, 55, 90, rng_);
        Color col;
        if (has_colors(cfg_)) col = pick_color(cfg_, rng_);
        else { static const Color pool[3] = {{255,255,255},{200,225,255},{255,240,220}};
               col = pool[irand(rng_, 0, 2)]; }
        const double r0 = frand(rng_, 0, 6);                       // nudge off-centre
        Particle p;
        p.x = cx + std::cos(ang) * r0 - ox_;
        p.y = cy + std::sin(ang) * r0 - oy_;
        p.vx = std::cos(ang) * spd; p.vy = std::sin(ang) * spd;
        p.r = col.r; p.g = col.g; p.b = col.b;
        p.size = pick_size(cfg_, 1, 1, rng_);
        p.max_life = 2.5; p.life = 1; p.extra = ang;
        return p;
    }
};

// ── Effect factory ───────────────────────────────────────────────────────────

std::unique_ptr<BaseEffect> make_effect(const std::string& name, int w, int h, const json& cfg) {
    if (name == "sparkle")   return std::make_unique<SparkleEffect>(w, h, cfg);
    if (name == "snow")      return std::make_unique<SnowEffect>(w, h, cfg);
    if (name == "embers")    return std::make_unique<EmbersEffect>(w, h, cfg);
    if (name == "confetti")  return std::make_unique<ConfettiEffect>(w, h, cfg);
    if (name == "flames")    return std::make_unique<FlamesEffect>(w, h, cfg);
    if (name == "rings")     return std::make_unique<RingsEffect>(w, h, cfg);
    if (name == "rain")      return std::make_unique<RainEffect>(w, h, cfg);
    if (name == "steam")     return std::make_unique<SteamEffect>(w, h, cfg);
    if (name == "waveform")  return std::make_unique<WaveformEffect>(w, h, cfg);
    if (name == "matrix")    return std::make_unique<MatrixEffect>(w, h, cfg);
    if (name == "circuit")   return std::make_unique<CircuitEffect>(w, h, cfg);
    if (name == "frost")     return std::make_unique<FrostEffect>(w, h, cfg);
    if (name == "heatwave")  return std::make_unique<HeatwaveEffect>(w, h, cfg);
    if (name == "snooze")    return std::make_unique<SnoozeEffect>(w, h, cfg);
    if (name == "fireflies") return std::make_unique<FirefliesEffect>(w, h, cfg);
    if (name == "clouds")    return std::make_unique<CloudsEffect>(w, h, cfg);
    if (name == "breath")    return std::make_unique<BreathEffect>(w, h, cfg);
    if (name == "lightning") return std::make_unique<LightningEffect>(w, h, cfg);
    if (name == "bubbles")   return std::make_unique<BubblesEffect>(w, h, cfg);
    if (name == "fireworks") return std::make_unique<FireworksEffect>(w, h, cfg);
    if (name == "vortex")    return std::make_unique<VortexEffect>(w, h, cfg);
    if (name == "water")     return std::make_unique<WaterEffect>(w, h, cfg);
    if (name == "starfield")     return std::make_unique<StarfieldEffect>(w, h, cfg);
    if (name == "warp")          return std::make_unique<WarpEffect>(w, h, cfg);
    if (name == "constellation") return std::make_unique<ConstellationEffect>(w, h, cfg);
    if (name == "shootingstars") return std::make_unique<ShootingStarsEffect>(w, h, cfg);
    return nullptr;
}

// ── Presets (ported from particles/presets.yaml) ─────────────────────────────

const std::map<std::string, json>& presets() {
    static const std::map<std::string, json> kPresets = [] {
        const char* kJson = R"JSON({
          "steam": {"effect":"steam","count":16,"blend":"add"},
          "waveform": {"effect":"waveform","blend":"add"},
          "matrix": {"effect":"matrix","blend":"add"},
          "circuit": {"effect":"circuit","count":5,"blend":"add"},
          "frost": {"effect":"frost","count":44,"fractal":true,"blend":"add"},
          "heatwave": {"effect":"heatwave","count":18,"heartbeat":true,"blend":"add"},
          "snooze": {"effect":"snooze","count":3,"blend":"add"},
          "petals": {"effect":"confetti","count":14,"colors":[[255,150,180],[255,190,210],[240,120,160]],"speed_min":3.0,"speed_max":6.5,"drift_x":2.5,"tumble":2.5,"blend":"add"},
          "dizzy": {"layers":[
            {"effect":"vortex","count":24,"swirl":3.2,"infall":4,"colors":[[255,230,120],[255,255,255],[255,200,80]],"blend":"add"},
            {"effect":"sparkle","count":5,"colors":[[255,255,255]],"life_min":0.1,"life_max":0.3,"blend":"add"}]},
          "cold_breath": {"effect":"breath","count":2,"colors":[[215,230,245],[235,245,255]],"origin_x":0.5,"origin_y":0.62,"size_min":1.5,"size_max":16.0,"speed_min":6.0,"speed_max":11.0,"alpha_max":0.5,"rate":0.5,"spread_frac":0.05,"trigger":"none","threshold":0.25,"blend":"add"},
          "gentle_snow": {"effect":"snow","count":15,"colors":[[200,215,255],[220,235,255]],"speed_min":4.0,"speed_max":7.0,"drift_x":0.8,"blend":"add"},
          "heavy_snow": {"effect":"snow","count":60,"colors":[[240,245,255],[255,255,255]],"speed_min":10.0,"speed_max":18.0,"drift_x":3.0,"blend":"add"},
          "campfire": {"layers":[
            {"effect":"flames","height":0.32,"turbulence":0.7,"blend":"normal"},
            {"effect":"embers","count":18,"colors":[[255,80,10],[255,140,20]],"speed_min":14.0,"speed_max":26.0,"spread":0.6,"size_min":1,"size_max":1,"blend":"add"}]},
          "galaxy": {"effect":"sparkle","count":60,"colors":[[255,80,80],[80,180,255],[255,220,80],[180,80,255],[80,255,160]],"life_min":0.05,"life_max":0.5,"blend":"add"},
          "party": {"effect":"confetti","count":35,"colors":[[255,50,50],[255,180,30],[50,220,50],[50,150,255],[220,50,220],[255,255,50]],"speed_min":6,"speed_max":10,"blend":"normal"},
          "radar": {"effect":"rings","count":2,"colors":[[0,255,80]],"speed_min":12,"speed_max":18,"max_radius":25,"blend":"add"},
          "fire": {"layers":[
            {"effect":"flames","height":0.55,"blend":"normal"},
            {"effect":"embers","count":12,"colors":[[255,160,0],[255,200,20],[255,120,0]],"speed_min":18,"speed_max":40,"size_min":1,"size_max":1,"blend":"add"},
            {"effect":"sparkle","count":4,"colors":[[255,255,200],[255,240,180]],"life_min":0.05,"life_max":0.12,"blend":"add"}]},
          "aurora": {"layers":[
            {"effect":"fireflies","count":20,"colors":[[0,200,180],[0,160,255],[0,220,200]],"speed_min":3,"speed_max":6,"blend":"add"},
            {"effect":"sparkle","count":10,"colors":[[100,255,220],[80,220,255]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "blizzard": {"layers":[
            {"effect":"snow","count":70,"colors":[[210,225,255],[235,245,255]],"speed_min":35,"speed_max":55,"drift_x":1.5,"direction_deg":118,"blend":"add"},
            {"effect":"snow","count":45,"colors":[[235,245,255],[255,255,255]],"speed_min":55,"speed_max":85,"drift_x":2.0,"direction_deg":113,"size_min":1,"size_max":1,"blend":"add"}]},
          "sonar": {"layers":[
            {"effect":"rings","count":2,"colors":[[0,220,180],[0,200,160]],"speed_min":12,"speed_max":16,"max_radius":28,"blend":"add"},
            {"effect":"fireflies","count":4,"colors":[[0,255,200]],"speed_min":1,"speed_max":3,"blend":"add"}]},
          "celebration": {"layers":[
            {"effect":"confetti","count":30,"colors":[[255,50,120],[255,160,0],[50,200,255],[200,50,255]],"speed_min":5,"speed_max":12,"blend":"normal"},
            {"effect":"sparkle","count":15,"colors":[[255,255,255],[255,220,255]],"life_min":0.05,"life_max":0.25,"blend":"add"}]},
          "plasma": {"layers":[
            {"effect":"embers","count":30,"colors":[[0,180,255],[0,140,220],[20,160,255]],"speed_min":8,"speed_max":20,"blend":"add"},
            {"effect":"embers","count":12,"colors":[[180,220,255],[200,240,255]],"speed_min":20,"speed_max":40,"size_min":1,"size_max":1,"blend":"add"},
            {"effect":"rings","count":1,"colors":[[0,200,255]],"speed_min":10,"speed_max":15,"max_radius":15,"blend":"add"}]},
          "rain": {"effect":"rain","count":35,"colors":[[120,150,220],[150,180,255]],"speed_min":70,"speed_max":120,"drift_x":1.5,"blend":"add"},
          "water": {"effect":"water","level":0.42,"alpha":0.85,"slosh":6,"wave_count":2,"wave_speed":2.0,"pitch_fill":0.30,"sheen":0.55,"face_glow":0.55,"bubbles":8,"bubble_mode":"rise","colors":[[120,220,255],[0,110,210],[0,40,120]],"blend":"normal"},
          "lava": {"effect":"water","level":0.36,"alpha":0.95,"slosh":4,"wave_count":2,"wave_speed":1.3,"viscosity":0.6,"pitch_fill":0.20,"sheen":0.20,"face_glow":0.30,"meniscus":1.0,"colors":[[255,230,120],[255,90,0],[150,20,0]],"blend":"normal"},
          "toxic": {"effect":"water","level":0.40,"alpha":0.9,"slosh":7,"wave_count":3,"wave_speed":2.4,"pitch_fill":0.30,"sheen":0.5,"face_glow":0.6,"bubbles":12,"bubble_mode":"rise","colors":[[210,255,120],[60,200,0],[15,110,0]],"blend":"normal"},
          "ocean": {"effect":"water","level":0.52,"alpha":0.85,"slosh":8,"wave_count":3,"wave_speed":1.8,"pitch_fill":0.35,"sheen":0.5,"face_glow":0.45,"colors":[[90,235,225],[0,130,170],[0,40,90]],"blend":"normal"},
          "plasma_fluid": {"effect":"water","level":0.40,"alpha":0.9,"slosh":6,"wave_count":2,"pitch_fill":0.30,"sheen":0.6,"face_glow":0.7,"colors":[[255,130,255],[150,0,200],[40,0,80]],"blend":"normal"},
          "mercury": {"effect":"water","level":0.36,"alpha":0.96,"slosh":3,"wave_count":2,"viscosity":0.7,"pitch_fill":0.15,"sheen":0.9,"face_glow":0.12,"colors":[[235,240,250],[120,130,150],[55,60,75]],"blend":"normal"},
          "thunderstorm": {"layers":[
            {"effect":"rain","count":40,"colors":[[120,150,220],[150,180,255]],"speed_min":85,"speed_max":135,"drift_x":3.0,"blend":"add"},
            {"effect":"lightning","rate":0.7,"branches":0.45,"colors":[[200,220,255],[180,200,255]],"blend":"add"}]},
          "arc": {"effect":"lightning","arc":true,"count":2,"branches":0.35,"jitter":5,"colors":[[160,200,255],[210,225,255],[120,170,255]],"blend":"add"},
          "fireworks": {"effect":"fireworks","count":3,"burst":24,"colors":[[255,80,80],[80,180,255],[255,220,80],[180,80,255],[80,255,160]],"blend":"add"},
          "bubbles": {"effect":"bubbles","count":16,"colors":[[160,220,255],[120,200,255],[200,240,255]],"speed_min":8,"speed_max":18,"wobble":7,"blend":"add"},
          "vortex": {"layers":[
            {"effect":"vortex","count":70,"swirl":2.6,"infall":7,"colors":[[0,220,255],[0,255,200],[120,90,255],[200,80,255],[60,140,255]],"blend":"add"},
            {"effect":"sparkle","count":8,"colors":[[255,255,255]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "vortex_ember": {"layers":[
            {"effect":"vortex","count":70,"swirl":2.6,"infall":7,"colors":[[255,220,80],[255,140,0],[255,70,0],[210,40,20],[255,180,40]],"blend":"add"},
            {"effect":"sparkle","count":8,"colors":[[255,240,200]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "vortex_toxic": {"layers":[
            {"effect":"vortex","count":70,"swirl":2.6,"infall":7,"colors":[[210,255,120],[120,255,40],[40,200,0],[0,160,60],[160,255,80]],"blend":"add"},
            {"effect":"sparkle","count":8,"colors":[[230,255,200]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "vortex_rose": {"layers":[
            {"effect":"vortex","count":70,"swirl":2.6,"infall":7,"colors":[[255,120,200],[255,60,140],[210,40,255],[150,60,255],[255,150,230]],"blend":"add"},
            {"effect":"sparkle","count":8,"colors":[[255,230,245]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "vortex_rainbow": {"layers":[
            {"effect":"vortex","count":80,"swirl":2.6,"infall":7,"colors":[[255,60,60],[255,170,0],[80,230,40],[0,210,255],[120,90,255],[230,80,230]],"blend":"add"},
            {"effect":"sparkle","count":8,"colors":[[255,255,255]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "nebula": {"layers":[
            {"effect":"clouds","count":4,"size_min":10,"size_max":18,"lobes_min":3,"lobes_max":6,"turbulence":0.55,"churn":0.3,"alpha_min":0.18,"alpha_max":0.4,"speed_min":0.5,"speed_max":1.8,"colors":[[150,40,140],[90,60,200],[40,90,200]],"blend":"add"},
            {"effect":"clouds","count":5,"size_min":5,"size_max":9,"lobes_min":2,"lobes_max":4,"turbulence":0.8,"churn":0.5,"alpha_min":0.15,"alpha_max":0.4,"speed_min":1.5,"speed_max":4.0,"colors":[[220,80,170],[60,160,200],[120,80,210]],"blend":"add"},
            {"effect":"sparkle","count":10,"colors":[[255,255,255],[200,220,255]],"life_min":0.4,"life_max":1.2,"blend":"add"}]},
          "starfield": {"effect":"starfield","count":80,"colors":[[255,255,255],[200,220,255],[255,240,210],[210,235,255]],"speed_min":1.2,"speed_max":3.2,"size_max":2,"blend":"add"},
          "warp": {"effect":"warp","count":70,"colors":[[255,255,255],[200,225,255],[180,210,255]],"speed_min":3.0,"speed_max":6.0,"streak":1.2,"blend":"add"},
          "constellation": {"effect":"constellation","count":55,"colors":[[255,255,255],[200,220,255],[255,240,210],[255,255,235]],"twinkle_min":0.7,"twinkle_max":3.0,"bright_frac":0.18,"blend":"add"},
          "shooting_stars": {"effect":"shootingstars","count":3,"colors":[[255,255,255],[200,225,255],[255,240,220]],"speed_min":55,"speed_max":95,"rate":0.8,"tail":9,"blend":"add"},
          "night_sky": {"layers":[
            {"effect":"constellation","count":55,"colors":[[255,255,255],[200,220,255],[255,240,210],[255,255,235]],"twinkle_min":0.6,"twinkle_max":2.6,"bright_frac":0.18,"blend":"add"},
            {"effect":"shootingstars","count":2,"colors":[[255,255,255],[210,230,255]],"speed_min":55,"speed_max":95,"rate":0.5,"tail":10,"blend":"add"}]}
        })JSON";
        std::map<std::string, json> m;
        json all = json::parse(kJson);
        for (auto& [k, v] : all.items()) m[k] = v;
        return m;
    }();
    return kPresets;
}

// ── Config resolution (mirror _resolve_cfg) ──────────────────────────────────

json resolve_cfg(const json& cfg) {
    json empty; empty["layers"] = json::array();

    if (cfg.is_null()) return empty;
    if (cfg.is_string()) {
        std::string s = cfg.get<std::string>();
        if (s == "none") return empty;
        json out; out["layers"] = json::array(); out["layers"].push_back({{"effect", s}});
        return out;
    }
    if (cfg.is_object()) {
        if (cfg.empty()) return empty;
        if (cfg.contains("preset")) {
            std::string name = cfg["preset"].get<std::string>();
            auto it = presets().find(name);
            if (it == presets().end()) return empty;
            return resolve_cfg(it->second);
        }
        if (cfg.contains("layers")) return cfg;
        if (cfg.contains("effect")) { json out; out["layers"] = json::array(); out["layers"].push_back(cfg); return out; }
        std::string name = cfg.value("active", std::string("none"));
        if (name == "none" || name.empty()) return empty;
        json layer; layer["effect"] = name;
        if (cfg.contains(name) && cfg[name].is_object())
            for (auto& [k, v] : cfg[name].items()) layer[k] = v;
        layer["intensity"] = jnum(cfg, "intensity", 1.0);
        json out; out["layers"] = json::array(); out["layers"].push_back(layer);
        return out;
    }
    return empty;
}

// ── Layer ────────────────────────────────────────────────────────────────────

struct ParticleLayer {
    std::unique_ptr<BaseEffect> effect;
    Blend blend = Blend::Add;
    std::string name = "none";

    // Per-effect blend default: flames is an opaque BODY — additive over lit
    // face art saturates straight to white (how "flames" looked in Single
    // Effects, which sends no blend key). Everything else keeps "add".
    static Blend default_blend(const json& layer_cfg) {
        const std::string def =
            layer_cfg.value("effect", std::string()) == "flames" ? "normal"
                                                                 : "add";
        return layer_cfg.value("blend", def) == "add" ? Blend::Add
                                                      : Blend::Normal;
    }

    ParticleLayer(const json& layer_cfg, int w, int h) {
        blend = default_blend(layer_cfg);
        name = layer_cfg.value("effect", std::string("none"));
        if (name != "none") effect = make_effect(name, w, h, layer_cfg);
    }
    void update(double dt) { if (effect) effect->update(dt); }
    cv::Mat render()       { return effect ? effect->render() : cv::Mat(); }
    double  face_glow() const { return effect ? effect->face_glow() : 0.0; }
    const cv::Mat* face_tint(uint8_t& r, uint8_t& g, uint8_t& b) const {
        return effect ? effect->face_tint(r, g, b) : nullptr;
    }
    void set_motion(const MotionInput& m) { if (effect) effect->set_motion(m); }
    void set_audio(double level)          { if (effect) effect->set_audio(level); }
    void set_humidity(double h)           { if (effect) effect->set_humidity(h); }
    void set_ambient(double lux, double t, double d) {
        if (effect) effect->set_ambient(lux, t, d);
    }
    void set_motion_reactive(bool on)     { if (effect) effect->set_motion_reactive(on); }
    void set_canvas_geometry(int cw, int ch, int ox, int oy) {
        if (effect) effect->set_canvas_geometry(cw, ch, ox, oy);
    }
    // In-place param update (same effect) — keeps the running sim.
    void update_cfg(const json& layer_cfg) {
        blend = default_blend(layer_cfg);
        if (effect) effect->set_cfg(layer_cfg);
    }
};

} // namespace

// ── ParticleSystem (pImpl) ───────────────────────────────────────────────────

struct ParticleSystem::Impl {
    int w, h;
    std::vector<ParticleLayer> layers;
    MotionInput motion{};   // latest IMU state, re-applied to rebuilt layers
    double audio = 0.0;     // latest mic level, re-applied to rebuilt layers
    double humidity = -1.0; // latest rel humidity 0..1 (<0 = none), re-applied
    int cw, ch, ox = 0, oy = 0;   // canvas geometry, re-applied to rebuilt layers
    bool motion_reactive = false; // global gravity default, re-applied on rebuilds

    // Transient boop ripples — expanding rings drawn over whatever the layers
    // produce, independent of the configured effect. Centre is stored in
    // canvas-normalised coords so a multi-panel face shows one shared ring.
    struct Ripple {
        double  cxn, cyn;
        double  age = 0.0, max_age = 0.9;
        uint8_t r, g, b;
        double  speed = 1.0;   // scales age advance: expansion AND fade rate
    };
    std::vector<Ripple> ripples;

    // render() scratch buffers, reused across frames (see render for the
    // aliasing contract on rgba8).
    cv::Mat outf, framef, rgba8;

    void build(const json& cfg) {
        layers.clear();
        json resolved = resolve_cfg(cfg);
        for (auto& lc : resolved["layers"]) {
            layers.emplace_back(lc, w, h);
            layers.back().set_motion(motion);
            layers.back().set_audio(audio);
            layers.back().set_humidity(humidity);
            layers.back().set_motion_reactive(motion_reactive);
            layers.back().set_canvas_geometry(cw, ch, ox, oy);
        }
    }
    // Update params in place when the layer structure (count + effect names) is
    // unchanged, so live edits don't reset the particle sim. Returns false (→
    // caller rebuilds) when the structure differs.
    bool try_update(const json& cfg) {
        json resolved = resolve_cfg(cfg);
        const auto& nl = resolved["layers"];
        if (nl.size() != layers.size()) return false;
        for (size_t i = 0; i < layers.size(); ++i)
            if (nl[i].value("effect", std::string("none")) != layers[i].name) return false;
        for (size_t i = 0; i < layers.size(); ++i) layers[i].update_cfg(nl[i]);
        return true;
    }
};

ParticleSystem::ParticleSystem(int width, int height, const json& cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->w = width; impl_->h = height;
    impl_->cw = width; impl_->ch = height;   // default canvas = this panel (per-panel)
    impl_->build(cfg);
}
ParticleSystem::~ParticleSystem() = default;

json merge_effect_specs(const json& base, const json& over) {
    json bl = resolve_cfg(base), ol = resolve_cfg(over);
    json out; out["layers"] = json::array();
    if (bl.contains("layers"))
        for (auto& l : bl["layers"]) out["layers"].push_back(l);
    if (ol.contains("layers"))
        for (auto& l : ol["layers"]) out["layers"].push_back(l);
    if (out["layers"].empty()) return json("none");
    return out;
}

void ParticleSystem::set_effect(const json& cfg) {
    if (!impl_->try_update(cfg)) impl_->build(cfg);   // in-place when structure matches
}

void ParticleSystem::set_canvas_geometry(int canvas_w, int canvas_h, int off_x, int off_y) {
    impl_->cw = canvas_w; impl_->ch = canvas_h; impl_->ox = off_x; impl_->oy = off_y;
    for (auto& l : impl_->layers) l.set_canvas_geometry(canvas_w, canvas_h, off_x, off_y);
}

void ParticleSystem::set_motion(const MotionInput& m) {
    impl_->motion = m;
    for (auto& l : impl_->layers) l.set_motion(m);
}

void ParticleSystem::set_audio(double level) {
    impl_->audio = level;
    for (auto& l : impl_->layers) l.set_audio(level);
}

void ParticleSystem::set_humidity(double humidity01) {
    impl_->humidity = humidity01;
    for (auto& l : impl_->layers) l.set_humidity(humidity01);
}

void ParticleSystem::set_ambient(double lux, double temp_c, double day_frac) {
    for (auto& l : impl_->layers) l.set_ambient(lux, temp_c, day_frac);
}

void ParticleSystem::set_motion_reactive(bool on) {
    impl_->motion_reactive = on;
    for (auto& l : impl_->layers) l.set_motion_reactive(on);
}

void ParticleSystem::trigger_ripple(double cx_norm, double cy_norm,
                                    uint8_t r, uint8_t g, uint8_t b,
                                    double speed) {
    if (impl_->ripples.size() >= 6)                      // rapid boops: cap, drop oldest
        impl_->ripples.erase(impl_->ripples.begin());
    impl_->ripples.push_back({cx_norm, cy_norm, 0.0, 0.9, r, g, b,
                              speed > 0.0 ? speed : 1.0});
}

void ParticleSystem::update(double dt) {
    for (auto& l : impl_->layers) l.update(dt);
    for (auto& rp : impl_->ripples) rp.age += dt * rp.speed;
    impl_->ripples.erase(std::remove_if(impl_->ripples.begin(), impl_->ripples.end(),
        [](const Impl::Ripple& rp){ return rp.age >= rp.max_age; }),
        impl_->ripples.end());
}

ParticleFrame ParticleSystem::render() {
    ParticleFrame result;
    if (impl_->layers.empty() && impl_->ripples.empty()) return result;

    const int w = impl_->w, h = impl_->h;
    // Scratch mats live on the Impl and are reused every frame — the returned
    // ParticleFrame is consumed within the same render tick (composite +
    // face-glow), so handing out a shallow header over impl_->rgba8 is safe.
    cv::Mat& outf = impl_->outf;                 // R,G,B,A on a 0..255 scale
    if (outf.rows != h || outf.cols != w || outf.type() != CV_32FC4)
        outf.create(h, w, CV_32FC4);
    outf.setTo(cv::Scalar(0, 0, 0, 0));
    bool has = false, all_add = true;

    for (auto& layer : impl_->layers) {
        cv::Mat frame = layer.render();
        if (frame.empty()) continue;
        has = true;
        if (layer.blend != Blend::Add) all_add = false;
        result.face_glow = std::max(result.face_glow, layer.face_glow());
        // Face-tint request (frost): first tinting layer wins — the mask is a
        // scratch header consumed this tick, same contract as rgba.
        if (result.face_tint.empty()) {
            uint8_t tr, tg, tb;
            if (const cv::Mat* tm = layer.face_tint(tr, tg, tb)) {
                result.face_tint = *tm;
                result.tint_r = tr; result.tint_g = tg; result.tint_b = tb;
            }
        }

        if (layer.blend == Blend::Add) {
            // Vectorized accumulate: uchar layer → float, summed in one pass.
            frame.convertTo(impl_->framef, CV_32F);
            cv::add(outf, impl_->framef, outf);
        } else {
            for (int y = 0; y < h; ++y) {
                const cv::Vec4b* s = frame.ptr<cv::Vec4b>(y);
                cv::Vec4f*       o = outf.ptr<cv::Vec4f>(y);
                for (int x = 0; x < w; ++x) {
                    float sa = s[x][3] / 255.f, da = o[x][3] / 255.f;
                    float oa = sa + da * (1.f - sa);
                    float safe = oa > 0.f ? oa : 1.f;
                    o[x][0] = (s[x][0] * sa + o[x][0] * da * (1.f - sa)) / safe;
                    o[x][1] = (s[x][1] * sa + o[x][1] * da * (1.f - sa)) / safe;
                    o[x][2] = (s[x][2] * sa + o[x][2] * da * (1.f - sa)) / safe;
                    o[x][3] = oa * 255.f;
                }
            }
        }
    }
    if (!has && impl_->ripples.empty()) return result;

    cv::Mat& rgba = impl_->rgba8;
    if (has) {
        outf.convertTo(rgba, CV_8U);             // saturating, same as the old loop
    } else {
        if (rgba.rows != h || rgba.cols != w || rgba.type() != CV_8UC4)
            rgba.create(h, w, CV_8UC4);
        rgba.setTo(cv::Scalar(0, 0, 0, 0));      // ripples on an empty layer
    }

    // Boop ripples on top: an expanding ring per touch, fading as it grows.
    // Radius scales with the full canvas so multi-panel faces share one ring;
    // the centre is canvas-normalised and shifted into this panel's space.
    for (const auto& rp : impl_->ripples) {
        const double t  = rp.age / rp.max_age;
        const double cx = rp.cxn * impl_->cw - impl_->ox;
        const double cy = rp.cyn * impl_->ch - impl_->oy;
        const double radius = 1.5 + t * 0.8 * std::max(impl_->cw, impl_->ch);
        const int    a     = static_cast<int>((1.0 - t) * 220.0);
        const int    steps = std::max(24, static_cast<int>(radius * 7));
        for (int i = 0; i < steps; ++i) {
            const double ang = kTau * i / steps;
            draw_pixel(rgba,
                       static_cast<int>(std::lround(cx + std::cos(ang) * radius)),
                       static_cast<int>(std::lround(cy + std::sin(ang) * radius)),
                       rp.r, rp.g, rp.b, a);
        }
    }

    result.has = true;
    result.rgba = rgba;
    result.blend = all_add ? Blend::Add : Blend::Normal;
    return result;
}

} // namespace face

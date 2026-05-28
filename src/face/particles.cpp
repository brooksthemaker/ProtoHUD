#include "particles.h"

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

// ── Base effect ──────────────────────────────────────────────────────────────

class BaseEffect {
public:
    BaseEffect(int w, int h, json cfg)
        : w_(w), h_(h), cfg_(std::move(cfg)),
          intensity_(jnum(cfg_, "intensity", 1.0)),
          rng_(static_cast<uint32_t>(
              std::chrono::steady_clock::now().time_since_epoch().count())) {}
    virtual ~BaseEffect() = default;
    virtual void update(double dt) = 0;
    virtual cv::Mat render() = 0;   // CV_8UC4

protected:
    int count(int def) const {
        return std::max(1, static_cast<int>(jnum(cfg_, "count", def) * intensity_));
    }
    void draw_particle(cv::Mat& c, const Particle& p, double alpha) {
        std::string shape = cfg_.value("shape", std::string("dot"));
        if (shape == "rect") draw_rect(c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, alpha, p.size);
        else                 draw_dot (c, p.x, p.y, (int)p.r, (int)p.g, (int)p.b, alpha, p.size);
    }
    cv::Mat blank() const { return cv::Mat::zeros(h_, w_, CV_8UC4); }
    // Direction unit vector for directional effects (snow / rain / embers /
    // confetti / clouds). Convention: 0° = right, 90° = down, 180° = left,
    // 270° = up (screen-space, +Y down). default_deg lets each effect keep
    // its historical motion direction when no "direction_deg" override is set.
    void direction_unit(double& dx, double& dy, double default_deg) const {
        const double deg = jnum(cfg_, "direction_deg", default_deg);
        const double rad = deg * kPi / 180.0;
        dx = std::cos(rad);
        dy = std::sin(rad);
    }

    int w_, h_;
    json cfg_;
    double intensity_;
    std::mt19937 rng_;
    std::vector<Particle> particles_;
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
        for (auto& p : particles_) draw_particle(c, p, std::sin(p.life * kPi));
        return c;
    }
};

// ── Snow ─────────────────────────────────────────────────────────────────────

class SnowEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double drift_x = jnum(cfg_, "drift_x", 1.5);
        double dx, dy; direction_unit(dx, dy, 90.0);   // default down
        for (auto& p : particles_) {
            double spd = (p.extra == 0) ? pick_speed(cfg_, 6, 12, rng_) : p.extra;
            p.extra = spd;
            p.vx = drift_x * std::sin(p.vx + p.y * 0.3);
            p.x += (spd * dx + p.vx) * dt;
            p.y += spd * dy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return !(p.x > -4 && p.x < w_ + 4 && p.y > -4 && p.y < h_ + 4 && p.life > 0);
            }), particles_.end());
        while ((int)particles_.size() < count(30)) {
            double spd = pick_speed(cfg_, 6, 12, rng_);
            double ml  = pick_life(cfg_, 1.5, 4.0, rng_);
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{200, 220, 255};
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, -2, 0);
            p.vx = frand(rng_, 0, kTau); p.max_life = ml; p.life = 1;
            p.r = col.r; p.g = col.g; p.b = col.b; p.size = pick_size(cfg_, 1, 1, rng_); p.extra = spd;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) draw_particle(c, p, 0.9);
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
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = h_; p.vy = spd;
            p.max_life = ml; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        std::string shape = cfg_.value("shape", std::string("dot"));
        for (auto& p : particles_) {
            double alpha = p.life;
            int gr = (int)std::clamp(p.r, 0.0, 255.0);
            int gg = (int)std::clamp(p.g * p.life, 0.0, 255.0);
            int gb = (int)std::clamp(p.b * p.life * p.life, 0.0, 255.0);
            if (shape == "rect") draw_rect(c, p.x, p.y, gr, gg, gb, alpha, p.size);
            else                 draw_dot (c, p.x, p.y, gr, gg, gb, alpha, p.size);
        }
        return c;
    }
};

// ── Confetti ─────────────────────────────────────────────────────────────────

class ConfettiEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double spd     = pick_speed(cfg_, 4, 10, rng_);
        double drift_x = jnum(cfg_, "drift_x", 0.0);
        double dx, dy; direction_unit(dx, dy, 90.0);  // default down
        for (auto& p : particles_) {
            p.vx = std::sin(p.extra) * 2.0 + drift_x;
            p.extra += dt * 4.0;
            p.x += (spd * dx + p.vx) * dt;
            p.y += spd * dy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return !(p.x > -4 && p.x < w_ + 4 && p.y > -4 && p.y < h_ + 4 && p.life > 0);
            }), particles_.end());
        static const int kDef[][3] = {
            {255,50,50},{255,180,30},{50,220,50},{50,150,255},{220,50,220},{255,255,50}};
        while ((int)particles_.size() < count(20)) {
            double ml = pick_life(cfg_, 2.0, 5.0, rng_);
            Color col;
            if (has_colors(cfg_)) col = pick_color(cfg_, rng_);
            else { const int* d = kDef[irand(rng_, 0, 5)]; col = {d[0], d[1], d[2]}; }
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, -4, 0);
            p.max_life = ml; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = frand(rng_, 0, kTau);
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) draw_particle(c, p, 1.0);
        return c;
    }
};

// ── Rings ────────────────────────────────────────────────────────────────────

class RingsEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double max_r  = jnum(cfg_, "max_radius", 20);
        double expand = pick_speed(cfg_, 15, 25, rng_);
        for (auto& p : particles_) { p.extra += expand * dt; p.life = std::max(0.0, 1.0 - p.extra / max_r); }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){ return !(p.life > 0); }), particles_.end());
        while ((int)particles_.size() < count(3)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{0, 200, 255};
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, 0, h_ - 1);
            p.max_life = max_r / expand; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b; p.extra = 0;
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

class RainEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        int    length  = jint(cfg_, "length", 4);
        double drift_x = jnum(cfg_, "drift_x", 0.0);
        double dx, dy; direction_unit(dx, dy, 90.0);  // default down
        for (auto& p : particles_) {
            p.x += (p.vy * dx + drift_x) * dt;
            p.y += p.vy * dy * dt;
            p.life -= dt / p.max_life;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
            [&](const Particle& p){
                return !(p.x > -length - 2 && p.x < w_ + length + 2 &&
                         p.y > -length - 2 && p.y < h_ + length + 2 && p.life > 0);
            }), particles_.end());
        while ((int)particles_.size() < count(15)) {
            double spd = pick_speed(cfg_, 30, 50, rng_);
            double ml  = (h_ + length) / spd;
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{100, 150, 255};
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, -(length + 2), 0);
            p.vy = spd; p.max_life = ml; p.life = 1; p.r = col.r; p.g = col.g; p.b = col.b;
            p.size = pick_size(cfg_, 1, 1, rng_); p.extra = length;
            particles_.push_back(p);
        }
    }
    cv::Mat render() override {
        cv::Mat c = blank();
        for (auto& p : particles_) {
            int length = (int)p.extra, ix = (int)p.x;
            for (int i = 0; i < length; ++i) {
                int iy = (int)p.y - i;
                double alpha = (1.0 - (double)i / length) * 0.9;
                draw_dot(c, ix, iy, (int)p.r, (int)p.g, (int)p.b, alpha, p.size);
            }
        }
        return c;
    }
};

// ── Fireflies ────────────────────────────────────────────────────────────────

class FirefliesEffect : public BaseEffect {
public:
    using BaseEffect::BaseEffect;
    void update(double dt) override {
        double spd = pick_speed(cfg_, 3, 6, rng_);
        for (auto& p : particles_) {
            p.extra += dt;
            p.x += std::cos(p.extra * 1.3 + p.vx) * spd * dt;
            p.y += std::sin(p.extra       + p.vy) * spd * dt;
            p.x = std::fmod(std::fmod(p.x, w_) + w_, w_);
            p.y = std::fmod(std::fmod(p.y, h_) + h_, h_);
            p.life = 0.5 + 0.5 * std::sin(p.extra * 2.5);
        }
        while ((int)particles_.size() < count(8)) {
            Color col = has_colors(cfg_) ? pick_color(cfg_, rng_) : Color{180, 255, 100};
            Particle p; p.x = frand(rng_, 0, w_ - 1); p.y = frand(rng_, 0, h_ - 1);
            p.vx = frand(rng_, 0, kTau); p.vy = frand(rng_, 0, kTau);
            p.life = 1; p.max_life = 9999; p.r = col.r; p.g = col.g; p.b = col.b;
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
                    // Respawn opposite the direction of travel so the clump
                    // re-enters the canvas. For axis-aligned cases this
                    // mimics the old left/right wrap; for diagonals it picks
                    // a corner along the trailing edges.
                    c.x = (dx > 0) ? -margin : (dx < 0 ? w_ + margin
                                               : frand(rng_, 0, w_ - 1));
                    c.y = (dy > 0) ? -margin : (dy < 0 ? h_ + margin
                                               : frand(rng_, 0, h_ - 1));
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
        cv::Mat acc(h_, w_, CV_32FC4, cv::Scalar(0, 0, 0, 0));
        double softness = std::clamp(jnum(cfg_, "softness", 0.6), 0.0, 1.0);
        double power = 1.0 + (1.0 - softness) * 2.0;
        std::vector<const Clump*> order;
        for (auto& c : clumps_) order.push_back(&c);
        std::sort(order.begin(), order.end(),
                  [](const Clump* a, const Clump* b){ return a->size > b->size; });
        for (const Clump* c : order) draw_clump(acc, *c, power);

        cv::Mat out(h_, w_, CV_8UC4);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const cv::Vec4f& a = acc.at<cv::Vec4f>(y, x);
                cv::Vec4b& o = out.at<cv::Vec4b>(y, x);
                o[0] = cv::saturate_cast<uchar>(a[0]);
                o[1] = cv::saturate_cast<uchar>(a[1]);
                o[2] = cv::saturate_cast<uchar>(a[2]);
                o[3] = cv::saturate_cast<uchar>(a[3] * 255.0f);
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
};

// ── Effect factory ───────────────────────────────────────────────────────────

std::unique_ptr<BaseEffect> make_effect(const std::string& name, int w, int h, const json& cfg) {
    if (name == "sparkle")   return std::make_unique<SparkleEffect>(w, h, cfg);
    if (name == "snow")      return std::make_unique<SnowEffect>(w, h, cfg);
    if (name == "embers")    return std::make_unique<EmbersEffect>(w, h, cfg);
    if (name == "confetti")  return std::make_unique<ConfettiEffect>(w, h, cfg);
    if (name == "rings")     return std::make_unique<RingsEffect>(w, h, cfg);
    if (name == "rain")      return std::make_unique<RainEffect>(w, h, cfg);
    if (name == "fireflies") return std::make_unique<FirefliesEffect>(w, h, cfg);
    if (name == "clouds")    return std::make_unique<CloudsEffect>(w, h, cfg);
    return nullptr;
}

// ── Presets (ported from particles/presets.yaml) ─────────────────────────────

const std::map<std::string, json>& presets() {
    static const std::map<std::string, json> kPresets = [] {
        const char* kJson = R"JSON({
          "gentle_snow": {"effect":"snow","count":15,"colors":[[200,215,255],[220,235,255]],"speed_min":4.0,"speed_max":7.0,"drift_x":0.8,"blend":"add"},
          "heavy_snow": {"effect":"snow","count":60,"colors":[[240,245,255],[255,255,255]],"speed_min":10.0,"speed_max":18.0,"drift_x":3.0,"blend":"add"},
          "campfire": {"effect":"embers","count":40,"colors":[[255,80,10],[255,100,20]],"speed_min":14.0,"speed_max":22.0,"spread":0.6,"blend":"add"},
          "galaxy": {"effect":"sparkle","count":60,"colors":[[255,80,80],[80,180,255],[255,220,80],[180,80,255],[80,255,160]],"life_min":0.05,"life_max":0.5,"blend":"add"},
          "party": {"effect":"confetti","count":35,"colors":[[255,50,50],[255,180,30],[50,220,50],[50,150,255],[220,50,220],[255,255,50]],"speed_min":6,"speed_max":10,"blend":"normal"},
          "radar": {"effect":"rings","count":2,"colors":[[0,255,80]],"speed_min":12,"speed_max":18,"max_radius":25,"blend":"add"},
          "fire": {"layers":[
            {"effect":"embers","count":35,"colors":[[255,50,0],[220,60,0],[200,40,0]],"speed_min":8,"speed_max":20,"size_min":1,"size_max":2,"blend":"add"},
            {"effect":"embers","count":15,"colors":[[255,160,0],[255,200,20],[255,180,10]],"speed_min":18,"speed_max":40,"size_min":1,"size_max":1,"blend":"add"},
            {"effect":"sparkle","count":5,"colors":[[255,255,200],[255,240,180]],"life_min":0.05,"life_max":0.12,"blend":"add"}]},
          "aurora": {"layers":[
            {"effect":"fireflies","count":20,"colors":[[0,200,180],[0,160,255],[0,220,200]],"speed_min":3,"speed_max":6,"blend":"add"},
            {"effect":"sparkle","count":10,"colors":[[100,255,220],[80,220,255]],"life_min":0.1,"life_max":0.4,"blend":"add"}]},
          "blizzard": {"layers":[
            {"effect":"snow","count":40,"colors":[[180,200,255],[200,215,255],[220,230,255]],"speed_min":18,"speed_max":35,"drift_x":4.0,"blend":"add"},
            {"effect":"rain","count":10,"colors":[[160,190,255],[180,200,255]],"speed_min":50,"speed_max":70,"drift_x":6.0,"blend":"add"}]},
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
          "nebula": {"layers":[
            {"effect":"clouds","count":4,"size_min":10,"size_max":18,"lobes_min":3,"lobes_max":6,"turbulence":0.55,"churn":0.3,"alpha_min":0.18,"alpha_max":0.4,"speed_min":0.5,"speed_max":1.8,"colors":[[150,40,140],[90,60,200],[40,90,200]],"blend":"add"},
            {"effect":"clouds","count":5,"size_min":5,"size_max":9,"lobes_min":2,"lobes_max":4,"turbulence":0.8,"churn":0.5,"alpha_min":0.15,"alpha_max":0.4,"speed_min":1.5,"speed_max":4.0,"colors":[[220,80,170],[60,160,200],[120,80,210]],"blend":"add"},
            {"effect":"sparkle","count":10,"colors":[[255,255,255],[200,220,255]],"life_min":0.4,"life_max":1.2,"blend":"add"}]}
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

    ParticleLayer(const json& layer_cfg, int w, int h) {
        blend = (layer_cfg.value("blend", std::string("add")) == "add") ? Blend::Add : Blend::Normal;
        std::string name = layer_cfg.value("effect", std::string("none"));
        if (name != "none") effect = make_effect(name, w, h, layer_cfg);
    }
    void update(double dt) { if (effect) effect->update(dt); }
    cv::Mat render()       { return effect ? effect->render() : cv::Mat(); }
};

} // namespace

// ── ParticleSystem (pImpl) ───────────────────────────────────────────────────

struct ParticleSystem::Impl {
    int w, h;
    std::vector<ParticleLayer> layers;

    void build(const json& cfg) {
        layers.clear();
        json resolved = resolve_cfg(cfg);
        for (auto& lc : resolved["layers"]) layers.emplace_back(lc, w, h);
    }
};

ParticleSystem::ParticleSystem(int width, int height, const json& cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->w = width; impl_->h = height;
    impl_->build(cfg);
}
ParticleSystem::~ParticleSystem() = default;

void ParticleSystem::set_effect(const json& cfg) { impl_->build(cfg); }

void ParticleSystem::update(double dt) {
    for (auto& l : impl_->layers) l.update(dt);
}

ParticleFrame ParticleSystem::render() {
    ParticleFrame result;
    if (impl_->layers.empty()) return result;

    const int w = impl_->w, h = impl_->h;
    cv::Mat outf(h, w, CV_32FC4, cv::Scalar(0, 0, 0, 0));   // R,G,B,A on a 0..255 scale
    bool has = false, all_add = true;

    for (auto& layer : impl_->layers) {
        cv::Mat frame = layer.render();
        if (frame.empty()) continue;
        has = true;
        if (layer.blend != Blend::Add) all_add = false;

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const cv::Vec4b& s = frame.at<cv::Vec4b>(y, x);
                cv::Vec4f& o = outf.at<cv::Vec4f>(y, x);
                if (layer.blend == Blend::Add) {
                    o[0] += s[0]; o[1] += s[1]; o[2] += s[2]; o[3] += s[3];
                } else {
                    float sa = s[3] / 255.f, da = o[3] / 255.f;
                    float oa = sa + da * (1.f - sa);
                    float safe = oa > 0.f ? oa : 1.f;
                    o[0] = (s[0] * sa + o[0] * da * (1.f - sa)) / safe;
                    o[1] = (s[1] * sa + o[1] * da * (1.f - sa)) / safe;
                    o[2] = (s[2] * sa + o[2] * da * (1.f - sa)) / safe;
                    o[3] = oa * 255.f;
                }
            }
    }
    if (!has) return result;

    cv::Mat rgba(h, w, CV_8UC4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const cv::Vec4f& o = outf.at<cv::Vec4f>(y, x);
            cv::Vec4b& d = rgba.at<cv::Vec4b>(y, x);
            d[0] = cv::saturate_cast<uchar>(o[0]);
            d[1] = cv::saturate_cast<uchar>(o[1]);
            d[2] = cv::saturate_cast<uchar>(o[2]);
            d[3] = cv::saturate_cast<uchar>(o[3]);
        }

    result.has = true;
    result.rgba = rgba;
    result.blend = all_add ? Blend::Add : Blend::Normal;
    return result;
}

} // namespace face

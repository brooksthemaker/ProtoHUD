#include "accessory_leds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace accessory {

namespace {
constexpr double kPi = 3.14159265358979323846;

// The zone's sections resolved to a working count list. A "single" zone (or one
// with no sections set) collapses to a single run of the total count.
std::vector<int> resolve_sections(const ZoneConfig& z) {
    if (z.shape != Shape::Single && !z.sections.empty()) {
        std::vector<int> v;
        for (int n : z.sections) if (n > 0) v.push_back(n);
        if (!v.empty()) return v;
    }
    return { std::max(0, zone_total(z)) };
}
}  // namespace

int zone_total(const ZoneConfig& z) {
    if (z.sections.empty()) return std::max(0, z.count);
    int s = 0;
    for (int n : z.sections) if (n > 0) s += n;
    return s;
}

std::vector<float> zone_len_fracs(const ZoneConfig& z) {
    const auto secs = resolve_sections(z);
    const int nsec = static_cast<int>(secs.size());
    std::vector<float> out;
    for (int s = 0; s < nsec; ++s) {
        const int n = secs[s];
        for (int j = 0; j < n; ++j) {
            float f;
            if (nsec <= 1)
                f = (n <= 1) ? 0.f : static_cast<float>(j) / (n - 1);   // linear
            else
                f = static_cast<float>(s) / (nsec - 1);                 // per section
            out.push_back(f);
        }
    }
    if (z.reverse) std::reverse(out.begin(), out.end());
    return out;
}

std::vector<LedPoint> zone_geometry(const ZoneConfig& z) {
    const auto secs = resolve_sections(z);
    const int nsec = static_cast<int>(secs.size());
    const double rot = z.rotation * kPi / 180.0;
    const double cs = std::cos(rot), sn = std::sin(rot);

    // Layout-unit constants (arbitrary; the visualizer letterbox-scales to fit).
    // band_spacing scales the gap BETWEEN rings/lines — cosmetic (it only moves
    // the preview x/y; len_frac and LED output are unaffected). It also nudges
    // the inner ring radius so rings spread from a tighter/looser hub.
    const double bs      = std::max(0.05, static_cast<double>(z.band_spacing));
    const double sc      = std::max(0.05, static_cast<double>(z.scale));  // overall size
    const double SPACING = 1.0;         // LED pitch along a line
    const double LSTEP   = 1.4 * bs;    // line-to-line spacing
    const double R0      = 2.0 * bs;    // inner ring radius
    const double RSTEP   = 1.5 * bs;    // ring-to-ring spacing
    int lmax = 1;                       // longest line — anchor for End Align justify
    for (int n : secs) lmax = std::max(lmax, n);

    std::vector<LedPoint> pts;
    for (int s = 0; s < nsec; ++s) {
        const int n = secs[s];
        for (int j = 0; j < n; ++j) {
            double lx = 0.0, ly = 0.0;
            float lf;
            if (z.shape == Shape::Rings) {
                const double R   = R0 + s * RSTEP;
                const int    dir = (z.serpentine && (s & 1)) ? -1 : 1;
                const double ang = 2.0 * kPi * dir * j / std::max(1, n);
                lx = R * std::cos(ang);
                ly = R * std::sin(ang);
                lf = (nsec <= 1) ? 0.f : static_cast<float>(s) / (nsec - 1);
            } else if (z.shape == Shape::Lines) {
                const int col = (z.serpentine && (s & 1)) ? (n - 1 - j) : j;
                // End Align justifies lines of unequal length: shift each line by
                // its slack (lmax-n) toward one side. -1 lines up the left/base
                // ends, +1 the right/tip ends, 0 leaves them centred.
                const double just = z.line_align * (lmax - n) * 0.5 * SPACING;
                lx = (col - (n - 1) / 2.0) * SPACING + just;
                ly = (s - (nsec - 1) / 2.0) * LSTEP;
                lf = (nsec <= 1) ? 0.f : static_cast<float>(s) / (nsec - 1);
            } else {                                       // Single: one straight run
                lx = (j - (n - 1) / 2.0) * SPACING;
                lf = (n <= 1) ? 0.f : static_cast<float>(j) / (n - 1);
            }
            // Mirror flips the shape left↔right in its own frame; scale sizes the
            // whole zone. Both are cosmetic (preview-only), applied before the
            // rotation + translation into canvas space.
            if (z.mirror) lx = -lx;
            lx *= sc; ly *= sc;
            const float X = static_cast<float>(z.pos_x + lx * cs - ly * sn);
            const float Y = static_cast<float>(z.pos_y + lx * sn + ly * cs);
            pts.push_back(LedPoint{ X, Y, s, lf });
        }
    }
    if (z.reverse) std::reverse(pts.begin(), pts.end());
    return pts;
}

std::vector<float> zone_spatial_fracs(const ZoneConfig& z, float angle_deg) {
    const auto pts = zone_geometry(z);        // wiring order, respects reverse/serpentine
    std::vector<float> out(pts.size(), 0.f);
    if (pts.empty()) return out;
    const double a  = angle_deg * kPi / 180.0;
    const double ux = std::cos(a), uy = std::sin(a);
    // Project each LED's canvas position onto the axis; normalise across the span.
    std::vector<double> proj(pts.size());
    double mn = 1e30, mx = -1e30;
    for (size_t i = 0; i < pts.size(); ++i) {
        const double p = pts[i].x * ux + pts[i].y * uy;
        proj[i] = p;
        if (p < mn) mn = p;
        if (p > mx) mx = p;
    }
    const double span = (mx > mn) ? (mx - mn) : 1.0;
    for (size_t i = 0; i < pts.size(); ++i)
        out[i] = static_cast<float>((proj[i] - mn) / span);
    return out;
}

namespace {
// Linear-interpolate a packed-RGB LUT at position f∈[0,1] (index 0 → n-1).
inline void sample_lut(const uint32_t* lut, int n, double f,
                       double& r, double& g, double& b) {
    if (n <= 0) { r = g = b = 0; return; }
    if (n == 1) { r = (lut[0]>>16)&0xFF; g = (lut[0]>>8)&0xFF; b = lut[0]&0xFF; return; }
    f = std::clamp(f, 0.0, 1.0);
    const double x = f * (n - 1);
    const int i0 = static_cast<int>(std::floor(x));
    const int i1 = std::min(i0 + 1, n - 1);
    const double fr = x - i0;
    auto ch = [](uint32_t c, int sh) { return static_cast<double>((c >> sh) & 0xFF); };
    r = ch(lut[i0],16)*(1-fr) + ch(lut[i1],16)*fr;
    g = ch(lut[i0], 8)*(1-fr) + ch(lut[i1], 8)*fr;
    b = ch(lut[i0], 0)*(1-fr) + ch(lut[i1], 0)*fr;
}
// Same, over a vector of RGB stop colors.
inline void sample_stops(const std::vector<std::array<uint8_t,3>>& s, double f,
                         double& r, double& g, double& b) {
    const int n = static_cast<int>(s.size());
    if (n <= 0) { r = g = b = 0; return; }
    if (n == 1) { r = s[0][0]; g = s[0][1]; b = s[0][2]; return; }
    f = std::clamp(f, 0.0, 1.0);
    const double x = f * (n - 1);
    const int i0 = static_cast<int>(std::floor(x));
    const int i1 = std::min(i0 + 1, n - 1);
    const double fr = x - i0;
    r = s[i0][0]*(1-fr) + s[i1][0]*fr;
    g = s[i0][1]*(1-fr) + s[i1][1]*fr;
    b = s[i0][2]*(1-fr) + s[i1][2]*fr;
}
}  // namespace

void zone_base_colors(const ZoneConfig& z, double t, float vol, uint32_t fc,
                      const uint32_t* face_ramp, int ramp_n,
                      std::vector<uint8_t>& out) {
    const int n = std::max(0, z.count);
    out.assign(static_cast<size_t>(n) * 3, 0);
    if (n <= 0 || z.pattern == Pattern::Off) return;

    const uint8_t zr = z.follow_face ? uint8_t((fc >> 16) & 0xFF) : z.r;
    const uint8_t zg = z.follow_face ? uint8_t((fc >>  8) & 0xFF) : z.g;
    const uint8_t zb = z.follow_face ? uint8_t( fc        & 0xFF) : z.b;

    // A follow zone with a fed ramp colors each LED from the eye's gradient.
    const bool use_ramp   = z.follow_face && face_ramp && ramp_n > 0;
    // Custom multi-stop gradient overrides the 2-color one when ≥2 stops.
    const bool multi_stop = (z.pattern == Pattern::Gradient) && z.stops.size() >= 2;

    double env = 0.0;
    switch (z.pattern) {
    case Pattern::Solid:  case Pattern::Chase: case Pattern::Sparkle:
    case Pattern::Gradient: case Pattern::Wave: env = 1.0; break;
    case Pattern::Breathe: {
        const double phase = 2.0 * kPi * z.breathe_hz * t;
        env = 0.5 * (1.0 - std::cos(phase)); break;
    }
    case Pattern::Level: env = static_cast<double>(vol); break;
    default: break;
    }
    env *= z.zone_brightness / 255.0;

    // Gradient/Wave normally run along the wire/length (per-section for a
    // multi-ring hub / multi-line fin). grad_spatial instead maps each LED by
    // its real 2D position projected onto grad_angle, so the gradient sweeps
    // across the whole shape at any angle. Follow-ramp keeps the length axis.
    std::vector<float> lf;
    const bool spatial = z.grad_spatial && !use_ramp &&
        (z.pattern == Pattern::Gradient || z.pattern == Pattern::Wave);
    if (z.pattern == Pattern::Gradient || z.pattern == Pattern::Wave || use_ramp)
        lf = spatial ? zone_spatial_fracs(z, z.grad_angle) : zone_len_fracs(z);

    for (int i = 0; i < n; ++i) {
        double px = 1.0;
        double cr = zr, cg = zg, cb = zb;
        const double lf_i = (i < static_cast<int>(lf.size())) ? lf[i] : 0.0;
        // Follow ramp: each LED takes the eye's color at its length fraction,
        // then the pattern below only modulates brightness (px).
        if (use_ramp) sample_lut(face_ramp, ramp_n, lf_i, cr, cg, cb);
        switch (z.pattern) {
        case Pattern::Chase: {
            const double head = std::fmod(z.breathe_hz * t, 1.0) * n;
            double d = head - i;
            if (d < 0) d += n;
            const double tail = std::max(3.0, n * 0.5);
            px = std::max(0.0, 1.0 - d / tail);
            px *= px;
            break;
        }
        case Pattern::Sparkle: {
            const uint32_t h = (uint32_t(z.start + i) * 2654435761u) ^ 0x9E3779B9u;
            const double rate  = 0.5 + (h & 0xFF) / 96.0;
            const double phase = ((h >> 8) & 0xFFFF) / 65536.0;
            const double v = 0.5 * (1.0 - std::cos(2.0 * kPi * (rate * t + phase)));
            px = v * v * v;
            break;
        }
        case Pattern::Gradient: {
            if (use_ramp) break;              // ramp already set the color
            double m = lf_i;
            if (z.wave_speed != 0.0f) {
                double f = lf_i + z.wave_speed * t;
                f -= std::floor(f);
                m = 1.0 - std::fabs(2.0 * f - 1.0);
            }
            if (multi_stop) {
                sample_stops(z.stops, m, cr, cg, cb);
            } else {
                cr = static_cast<double>(zr) + (static_cast<double>(z.r2) - zr) * m;
                cg = static_cast<double>(zg) + (static_cast<double>(z.g2) - zg) * m;
                cb = static_cast<double>(zb) + (static_cast<double>(z.b2) - zb) * m;
            }
            break;
        }
        case Pattern::Wave: {
            const double f0 = lf_i;
            double head = std::fmod(z.wave_speed * t, 1.0);
            if (head < 0) head += 1.0;
            double d = std::fabs(f0 - head);
            d = std::min(d, 1.0 - d);
            px = std::max(0.0, 1.0 - d / 0.18);
            px *= px;
            break;
        }
        default: break;
        }
        const double e = env * px;
        out[3*i+0] = static_cast<uint8_t>(cr * e);
        out[3*i+1] = static_cast<uint8_t>(cg * e);
        out[3*i+2] = static_cast<uint8_t>(cb * e);
    }
}

AccessoryLeds::AccessoryLeds(Config cfg)
    : cfg_(std::move(cfg)),
      strip_(std::make_unique<LedStrip>(cfg_.strip)),
      coproc_(cfg_.transport == "coproc")
{
    strip_->set_global_brightness(cfg_.global_brightness);
    frame_.assign(static_cast<size_t>(std::max(0, strip_->count())) * 3, 0);
}

AccessoryLeds::~AccessoryLeds() { stop(); }

void AccessoryLeds::reconfigure(const Config& cfg) {
    // Pause the render thread but KEEP the SPI device open — reopening spidev
    // mid-run is unreliable and was leaving the chain dark. Resize the buffers
    // in place, then restart the thread (start()'s open() no-ops since the fd
    // is still valid, or opens fresh if the LEDs were off).
    stop_thread();
    const std::string keep_transport = cfg_.transport;   // transport is restart-only
    cfg_ = cfg;
    cfg_.transport = keep_transport;
    // coproc_ / frame_sink_ / the open strip fd intentionally unchanged.
    strip_->resize(cfg_.strip.count);
    strip_->set_global_brightness(cfg_.global_brightness);
    frame_.assign(static_cast<size_t>(std::max(0, strip_->count())) * 3, 0);
    start();                                  // no-op if cfg_.enabled is false
}

bool AccessoryLeds::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    // Coproc transport owns no Pi SPI device — the frames go out over the
    // coprocessor link (frame_sink_). Only the SPI path needs an open strip.
    if (!coproc_ && !strip_->open()) {
        std::fprintf(stderr, "[led] accessory LEDs unavailable — SPI open failed\n");
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&AccessoryLeds::render_loop, this);
    return true;
}

void AccessoryLeds::stop_thread() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void AccessoryLeds::stop() {
    const bool was_running = running_.load();
    stop_thread();
    if (!was_running) return;
    // Blank on shutdown so leftover pixels don't linger, and release the device.
    if (coproc_) {
        if (frame_sink_ && strip_->count() > 0) {
            std::vector<uint8_t> black(static_cast<size_t>(strip_->count()) * 3, 0);
            frame_sink_(black.data(), strip_->count());
        }
    } else {
        strip_->fill(0, 0, 0);
        strip_->show();
        strip_->close();
    }
}

void AccessoryLeds::set_enabled(bool on) {
    cfg_.enabled = on;   // start() gates on this
    if (on) start();
    else    stop();
}

void AccessoryLeds::set_zone_pattern(Zone z, Pattern p) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].pattern = p;
}

void AccessoryLeds::set_zone_color(Zone z, uint8_t r, uint8_t g, uint8_t b) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].r = r;
    cfg_.zones[zi].g = g;
    cfg_.zones[zi].b = b;
}

void AccessoryLeds::trigger_flash(Zone z, double duration_s) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (duration_s <= 0.0) duration_s = 0.001;
    const us_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const us_t end = now + static_cast<us_t>(duration_s * 1'000'000.0);
    flash_start_us_[zi].store(now);
    flash_end_us_  [zi].store(end);
}

void AccessoryLeds::set_zone_color2(Zone z, uint8_t r, uint8_t g, uint8_t b) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].r2 = r;
    cfg_.zones[zi].g2 = g;
    cfg_.zones[zi].b2 = b;
}

void AccessoryLeds::set_zone_stops(Zone z, std::vector<std::array<uint8_t,3>> stops) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].stops = std::move(stops);
}

// ── Follow-face ramps ───────────────────────────────────────────────────────
void AccessoryLeds::set_face_ramp_whole(const uint32_t* d, int n) {
    std::lock_guard<std::mutex> lk(face_ramp_mtx_);
    face_ramp_whole_.assign(d, d + std::max(0, n));
}
void AccessoryLeds::set_face_ramp_left(const uint32_t* d, int n) {
    std::lock_guard<std::mutex> lk(face_ramp_mtx_);
    face_ramp_left_.assign(d, d + std::max(0, n));
}
void AccessoryLeds::set_face_ramp_right(const uint32_t* d, int n) {
    std::lock_guard<std::mutex> lk(face_ramp_mtx_);
    face_ramp_right_.assign(d, d + std::max(0, n));
}
std::vector<uint32_t> AccessoryLeds::face_ramp_for(Zone z) const {
    std::lock_guard<std::mutex> lk(face_ramp_mtx_);
    switch (z) {
        case Zone::LeftCheekhub:  case Zone::LeftFin:  return face_ramp_left_;
        case Zone::RightCheekhub: case Zone::RightFin: return face_ramp_right_;
        default:                                       return face_ramp_whole_;
    }
}

void AccessoryLeds::set_zone_wave_speed(Zone z, float hz) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (hz < -5.0f) hz = -5.0f;
    if (hz >  5.0f) hz =  5.0f;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].wave_speed = hz;
}

void AccessoryLeds::set_zone_grad_spatial(Zone z, bool on) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].grad_spatial = on;
}

void AccessoryLeds::set_zone_grad_angle(Zone z, float deg) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].grad_angle = deg;
}

void AccessoryLeds::set_zone_breathe_hz(Zone z, float hz) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (hz < 0.05f) hz = 0.05f;
    if (hz > 5.0f)  hz = 5.0f;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].breathe_hz = hz;
}

void AccessoryLeds::set_zone_brightness(Zone z, uint8_t b) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].zone_brightness = b;
}

void AccessoryLeds::set_zone_follow_face(Zone z, bool on) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].follow_face = on;
}

void AccessoryLeds::set_global_brightness(uint8_t b) {
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        cfg_.global_brightness = b;
    }
    // strip_ has its own atomic-enough store; safe to call outside the lock.
    strip_->set_global_brightness(b);
}

ZoneConfig AccessoryLeds::zone(Zone z) const {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return {};
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(cfg_mtx_));
    return cfg_.zones[zi];
}

uint8_t AccessoryLeds::global_brightness() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(cfg_mtx_));
    return cfg_.global_brightness;
}

void AccessoryLeds::render_loop() {
    using clock = std::chrono::steady_clock;
    const double period_s = 1.0 / std::max(1.0, cfg_.frame_hz);
    const auto period = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(period_s));

    const auto t0 = clock::now();
    // The loop clock `t` restarts at 0 every time the thread (re)starts, so the
    // coproc send throttle must too — otherwise after a live reconfigure it
    // waits until t climbs back past the previous run's value (the LEDs freeze
    // on their last frame for up to a minute or two). Send on the first frame.
    last_send_ = -1.0;

    while (running_.load()) {
        const auto next_t = clock::now() + period;
        const double t = std::chrono::duration<double>(clock::now() - t0).count();

        // Snapshot zone state under the lock; release before any SPI work so
        // the menu thread doesn't stall on the SPI write.
        std::array<ZoneConfig, ZoneCount> zones;
        uint8_t gbright;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            zones   = cfg_.zones;
            gbright = cfg_.global_brightness;
        }

        // Snapshot once per frame — avoids re-reading the atomic mid-zone.
        const float vol = std::clamp(audio_volume_.load(), 0.0f, 1.0f);
        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now().time_since_epoch()).count();

        // Composite into the RGB frame buffer — the SPI strip and the coproc
        // transport both read from it. Start from black; unwritten pixels stay
        // dark. (The frame is sized to the chain length at construction.)
        std::fill(frame_.begin(), frame_.end(), uint8_t(0));

        std::vector<uint8_t> zbuf;
        for (int zi = 0; zi < ZoneCount; ++zi) {
            const auto& z = zones[zi];
            if (z.count <= 0) continue;

            // Base pattern colors (shared with the editor preview), then the
            // event-driven flash overlay: a white pulse that decays over its
            // remaining lifetime and survives whichever base pattern is on.
            // A follow_face zone samples its own eye's ramp per-LED; others fall
            // back to the flat per-side follow color.
            std::vector<uint32_t> rampbuf;
            const uint32_t* ramp = nullptr;
            int ramp_n = 0;
            if (z.follow_face) {
                rampbuf = face_ramp_for(static_cast<Zone>(zi));
                ramp = rampbuf.data();
                ramp_n = static_cast<int>(rampbuf.size());
            }
            zone_base_colors(z, t, vol, face_color_for(static_cast<Zone>(zi)),
                             ramp, ramp_n, zbuf);
            double flash = 0.0;
            const int64_t fs = flash_start_us_[zi].load();
            const int64_t fe = flash_end_us_  [zi].load();
            if (fe > now_us && fe > fs) {
                flash = static_cast<double>(fe - now_us) /
                        static_cast<double>(fe - fs);
                if (flash > 1.0) flash = 1.0;
            }
            const double inv = 1.0 - flash;

            for (int i = 0; i < z.count; ++i) {
                const size_t o = static_cast<size_t>(z.start + i) * 3;
                if (o + 2 >= frame_.size()) break;
                for (int c = 0; c < 3; ++c)
                    frame_[o + c] = static_cast<uint8_t>(
                        zbuf[3*i + c] * inv + 255.0 * flash);
            }
        }

        if (coproc_) {
            // Bake in global brightness (the coproc drives the strip raw) and
            // stream the frame — throttled to ~30 Hz so the USB link + the
            // coprocessor's parser aren't saturated by a 60 Hz render.
            if (frame_sink_ && t - last_send_ >= 1.0 / 30.0) {
                last_send_ = t;
                if (gbright < 255)
                    for (auto& v : frame_) v = static_cast<uint8_t>(v * gbright / 255);
                frame_sink_(frame_.data(), strip_->count());
            }
        } else {
            const int n = strip_->count();
            for (int i = 0; i < n; ++i)
                strip_->set_pixel(i, frame_[i*3], frame_[i*3+1], frame_[i*3+2]);
            strip_->show();
        }
        std::this_thread::sleep_until(next_t);
    }
}

} // namespace accessory

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

void zone_group_len_fracs(const ZoneConfig& a, const ZoneConfig& b,
                          std::vector<float>& out_a, std::vector<float>& out_b) {
    out_a = zone_len_fracs(a);
    out_b = zone_len_fracs(b);
    const double na = static_cast<double>(out_a.size());
    const double nb = static_cast<double>(out_b.size());
    const double n  = std::max(1.0, na + nb);
    const double wa = na / n;                 // a occupies [0, wa], b occupies [wa, 1]
    for (auto& f : out_a) f = static_cast<float>(f * wa);
    for (auto& f : out_b) f = static_cast<float>(wa + f * (nb / n));
}

void zone_group_spatial_fracs(const ZoneConfig& a, const ZoneConfig& b, float angle_deg,
                              std::vector<float>& out_a, std::vector<float>& out_b) {
    const auto pa = zone_geometry(a);
    const auto pb = zone_geometry(b);
    out_a.assign(pa.size(), 0.f);
    out_b.assign(pb.size(), 0.f);
    const double ang = angle_deg * kPi / 180.0;
    const double ux = std::cos(ang), uy = std::sin(ang);
    std::vector<double> ja(pa.size()), jb(pb.size());
    double mn = 1e30, mx = -1e30;
    for (size_t i = 0; i < pa.size(); ++i) {
        const double p = pa[i].x * ux + pa[i].y * uy;
        ja[i] = p; mn = std::min(mn, p); mx = std::max(mx, p);
    }
    for (size_t i = 0; i < pb.size(); ++i) {
        const double p = pb[i].x * ux + pb[i].y * uy;
        jb[i] = p; mn = std::min(mn, p); mx = std::max(mx, p);
    }
    const double span = (mx > mn) ? (mx - mn) : 1.0;
    for (size_t i = 0; i < pa.size(); ++i) out_a[i] = static_cast<float>((ja[i] - mn) / span);
    for (size_t i = 0; i < pb.size(); ++i) out_b[i] = static_cast<float>((jb[i] - mn) / span);
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
// One full pass of a cyclic pattern, in seconds — how long complete-on-trigger
// holds the effect open so a brief sound still plays a whole sweep.
inline double effect_period(const ZoneConfig& z) {
    switch (z.pattern) {
        case Pattern::Wave:
        case Pattern::Gradient: return 1.0 / std::max(0.05f, std::fabs(z.wave_speed));
        case Pattern::Chase:
        case Pattern::Breathe:  return 1.0 / std::max(0.05f, z.breathe_hz);
        default:                return 1.0;   // Sparkle/Solid/Level: 1 s fallback
    }
}
}  // namespace

void zone_base_colors(const ZoneConfig& z, double t, float vol, uint32_t fc,
                      const uint32_t* face_ramp, int ramp_n,
                      std::vector<uint8_t>& out, float peak, float sound_gate,
                      const std::vector<float>* lf_override,
                      const std::vector<float>* pulses) {
    const int n = std::max(0, z.count);
    out.assign(static_cast<size_t>(n) * 3, 0);
    if (n <= 0 || z.pattern == Pattern::Off) return;

    const uint8_t zr = z.follow_face ? uint8_t((fc >> 16) & 0xFF) : z.r;
    const uint8_t zg = z.follow_face ? uint8_t((fc >>  8) & 0xFF) : z.g;
    const uint8_t zb = z.follow_face ? uint8_t( fc        & 0xFF) : z.b;

    // A follow zone with a fed ramp colors each LED from the eye's gradient.
    const bool use_ramp   = z.follow_face && face_ramp && ramp_n > 0;
    // A custom multi-stop gradient (≥2 stops) colors each LED along the length.
    // For Gradient it overrides the 2-color Color→Color2 ramp; it also tints
    // Wave / Level / Chase — there the effect's envelope just modulates the
    // brightness of the gradient-coloured base. (Follow-face ramp wins over it.)
    const bool has_stops  = z.stops.size() >= 2 && !use_ramp;
    const bool multi_stop = (z.pattern == Pattern::Gradient) && has_stops;
    const bool grad_color = has_stops &&
        (z.pattern == Pattern::Wave || z.pattern == Pattern::Level ||
         z.pattern == Pattern::Chase);

    double env = 0.0;
    switch (z.pattern) {
    case Pattern::Solid:  case Pattern::Chase: case Pattern::Sparkle:
    case Pattern::Gradient: case Pattern::Wave: env = 1.0; break;
    case Pattern::Breathe: {
        const double phase = 2.0 * kPi * z.breathe_hz * t;
        env = 0.5 * (1.0 - std::cos(phase)); break;
    }
    // Level: the volume is applied per-LED by the reaction style below (so a
    // Meter can fill part of the strip at full brightness), so env is just the
    // per-zone scale here.
    case Pattern::Level: env = 1.0; break;
    default: break;
    }
    // Overall per-zone scale (kept separate from the 0..1 modulation so the
    // min-lit floor lands in the modulation, dimmed by the zone as a whole).
    const double zbright = z.zone_brightness / 255.0;
    // Minimum lit level: remap the effect's 0..1 modulation to floor..1 so a
    // modulating pattern (Level/Breathe/Wave/Chase/…) idles at a dim glow
    // instead of going fully dark. 0 = classic (can go off); Solid is
    // unaffected (its modulation is already 1).
    const double floor = std::clamp(static_cast<double>(z.min_level), 0.0, 1.0);

    // Gradient/Wave normally run along the wire/length (per-section for a
    // multi-ring hub / multi-line fin). grad_spatial instead maps each LED by
    // its real 2D position projected onto grad_angle, so the effect sweeps
    // across the whole shape at any angle — including a follow-face ramp, which
    // then reproduces the eye's gradient across the shape rather than ring-by-ring.
    // A linked hub+fin group supplies a continuous fraction (lf_override) so the
    // effect spans the pair; otherwise compute it per-zone.
    std::vector<float> lf_local;
    const bool spatial = z.grad_spatial &&
        (pulses || use_ramp || z.pattern == Pattern::Gradient ||
         z.pattern == Pattern::Wave || grad_color);
    if (!lf_override &&
        (pulses || z.pattern == Pattern::Gradient || z.pattern == Pattern::Wave ||
         z.pattern == Pattern::Level || use_ramp || grad_color))
        lf_local = spatial ? zone_spatial_fracs(z, z.grad_angle) : zone_len_fracs(z);
    const std::vector<float>& lf = lf_override ? *lf_override : lf_local;
    // Linked hub+fin area: the effect PASSES THROUGH the pair (one-directional,
    // no wrap) rather than looping within each zone, so a wave/gradient enters
    // the hub and continues out the fin instead of both ends lighting at once.
    const bool linked = (lf_override != nullptr);

    // Level reaction needs the held-peak position; default to the current
    // volume when the caller doesn't feed a decaying one (e.g. the preview).
    const double lvl_peak = (peak >= 0.f) ? std::clamp<double>(peak, 0.0, 1.0)
                                          : std::clamp<double>(vol, 0.0, 1.0);

    for (int i = 0; i < n; ++i) {
        double px = 1.0;
        double cr = zr, cg = zg, cb = zb;
        const double lf_i = (i < static_cast<int>(lf.size())) ? lf[i] : 0.0;
        // Follow ramp: each LED takes the eye's color at its length fraction,
        // then the pattern below only modulates brightness (px). The custom
        // gradient does the same for Wave/Level/Chase (Gradient colours itself
        // in its own case below, with optional scroll).
        if (use_ramp) sample_lut(face_ramp, ramp_n, lf_i, cr, cg, cb);
        else if (grad_color) sample_stops(z.stops, lf_i, cr, cg, cb);

        // Overlapping sound pulses (complete-on-trigger): brightness is the
        // strongest travelling band over all live pulses; colour is the zone's
        // (ramp/stops already applied, a plain Gradient sampled here). Each
        // pulse sweeps independently so a new trigger doesn't disturb the rest.
        if (pulses) {
            if (!use_ramp && !grad_color && z.pattern == Pattern::Gradient) {
                if (multi_stop) sample_stops(z.stops, lf_i, cr, cg, cb);
                else {
                    cr = static_cast<double>(zr) + (static_cast<double>(z.r2) - zr) * lf_i;
                    cg = static_cast<double>(zg) + (static_cast<double>(z.g2) - zg) * lf_i;
                    cb = static_cast<double>(zb) + (static_cast<double>(z.b2) - zb) * lf_i;
                }
            }
            double best = 0.0;
            for (float ph : *pulses) {
                const double d = std::fabs(lf_i - static_cast<double>(ph));
                const double b = std::max(0.0, 1.0 - d / 0.18);
                best = std::max(best, b * b);
            }
            // The pulse band IS the brightness (no base-pattern envelope), then
            // the min-lit floor + zone scale, as usual.
            const double m = std::clamp(best, 0.0, 1.0);
            const double e = (floor + (1.0 - floor) * m) * zbright;
            out[3*i+0] = static_cast<uint8_t>(cr * e);
            out[3*i+1] = static_cast<uint8_t>(cg * e);
            out[3*i+2] = static_cast<uint8_t>(cb * e);
            continue;
        }
        switch (z.pattern) {
        case Pattern::Chase: {
            if (linked && !lf.empty()) {
                // Passthrough chase: the dot walks the pair's fraction 0→1 so it
                // travels the hub then the fin, with a fractional tail behind it.
                double head = std::fmod(z.breathe_hz * t, 1.0);
                double d = head - lf_i;
                if (d < 0) d += 1.0;
                const double tail = 0.35;
                px = std::max(0.0, 1.0 - d / tail);
                px *= px;
            } else {
                const double head = std::fmod(z.breathe_hz * t, 1.0) * n;
                double d = head - i;
                if (d < 0) d += n;
                const double tail = std::max(3.0, n * 0.5);
                px = std::max(0.0, 1.0 - d / tail);
                px *= px;
            }
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
                // Linked: scroll the gradient straight through the pair
                // (sawtooth); standalone: ping-pong so it doesn't hard-jump.
                m = linked ? f : (1.0 - std::fabs(2.0 * f - 1.0));
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
            // Standalone: wrap so the band loops the zone. Linked: no wrap, so
            // the band passes THROUGH the hub→fin once and exits (then re-enters
            // at the start) instead of appearing at both ends together.
            if (!linked) d = std::min(d, 1.0 - d);
            px = std::max(0.0, 1.0 - d / 0.18);
            px *= px;
            break;
        }
        case Pattern::Level: {
            // The mic volume drives each LED per the zone's reaction style;
            // colour is the zone colour (or the custom gradient, set above).
            const double L    = std::clamp<double>(vol, 0.0, 1.0);
            const double edge = 1.0 / std::max(1, n);     // ~1-LED soft edge
            switch (z.level_style) {
            case LevelStyle::Glow:
                px = L;
                break;
            case LevelStyle::Meter:                        // base → tip fill
                px = std::clamp((L - lf_i) / edge + 0.5, 0.0, 1.0);
                break;
            case LevelStyle::CenterMeter: {                // centre → out fill
                const double d = std::fabs(lf_i - 0.5) * 2.0;
                px = std::clamp((L - d) / edge + 0.5, 0.0, 1.0);
                break;
            }
            case LevelStyle::Peak: {                       // fill + peak marker
                const double fill   = std::clamp((L - lf_i) / edge + 0.5, 0.0, 1.0);
                const double markw  = 1.5 * edge;
                const double marker = (std::fabs(lf_i - lvl_peak) <= markw) ? 1.0 : 0.0;
                px = std::max(fill * 0.65, marker);
                break;
            }
            case LevelStyle::Pulse: {                      // soft centre burst
                const double d = std::fabs(lf_i - 0.5) * 2.0;
                px = std::clamp(L * 1.5 - d * (1.0 - L), 0.0, 1.0);
                break;
            }
            }
            break;
        }
        default: break;
        }
        // Effect modulation 0..1, gated by the sound trigger (1 = open), then
        // lifted to the min-lit floor and scaled by the zone brightness.
        const double m = env * px * std::clamp(static_cast<double>(sound_gate), 0.0, 1.0);
        const double e = (floor + (1.0 - floor) * m) * zbright;
        out[3*i+0] = static_cast<uint8_t>(cr * e);
        out[3*i+1] = static_cast<uint8_t>(cg * e);
        out[3*i+2] = static_cast<uint8_t>(cb * e);
    }
}

void mirror_zone_layout(ZoneConfig& d, const ZoneConfig& s) {
    d.shape        = s.shape;
    d.sections     = s.sections;
    d.band_spacing = s.band_spacing;
    d.scale        = s.scale;
    d.line_align   = s.line_align;
    d.reverse      = s.reverse;
    d.serpentine   = s.serpentine;
    d.count        = zone_total(d);        // = sum(sections) just copied
    d.mirror       = !s.mirror;            // flip so the preview reads as a mirror
    d.pos_x        = -s.pos_x;             // placement mirrored across the centre line
    d.pos_y        = s.pos_y;
    d.rotation     = -s.rotation;
    // d.name / d.start intentionally preserved (name + chain wiring offset).
}

void mirror_zone_look(ZoneConfig& d, const ZoneConfig& s) {
    d.pattern         = s.pattern;
    d.r = s.r; d.g = s.g; d.b = s.b;
    d.r2 = s.r2; d.g2 = s.g2; d.b2 = s.b2;
    d.stops           = s.stops;
    d.breathe_hz      = s.breathe_hz;
    d.wave_speed      = s.wave_speed;
    d.grad_spatial    = s.grad_spatial;
    d.grad_angle      = -s.grad_angle;     // mirror a directional spatial sweep
    d.zone_brightness = s.zone_brightness;
    d.follow_face     = s.follow_face;     // right zone still samples its own eye
    d.level_style     = s.level_style;
    d.min_level       = s.min_level;
    d.sound_trigger   = s.sound_trigger;
    d.sound_threshold = s.sound_threshold;
    d.sound_decay     = s.sound_decay;
    d.sound_complete  = s.sound_complete;
}

void copy_zone_look(ZoneConfig& d, const ZoneConfig& s) {
    mirror_zone_look(d, s);      // same fields...
    d.grad_angle = s.grad_angle; // ...but NOT flipped (same side, not mirrored)
}

AccessoryLeds::AccessoryLeds(Config cfg)
    : cfg_(std::move(cfg)),
      strip_(std::make_unique<LedStrip>(cfg_.strip)),
      coproc_(cfg_.transport == "coproc")
{
    strip_->set_global_brightness(cfg_.global_brightness);
    frame_.assign(static_cast<size_t>(std::max(0, strip_->count())) * 3, 0);
    sync_sides_.store(cfg_.sync_sides);
    link_areas_.store(cfg_.link_areas);
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
    sync_sides_.store(cfg_.sync_sides);
    link_areas_.store(cfg_.link_areas);
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
    bool changed;
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        changed = (cfg_.zones[zi].pattern != p);
        cfg_.zones[zi].pattern = p;
    }
    // Actually CHANGING a side zone's effect while sync is on restarts the
    // shared phase so all four begin aligned. The `changed` guard matters: the
    // cheek mirror re-pushes the same pattern every frame, which must not keep
    // re-zeroing the phase (that would freeze the effect at t=0).
    if (changed && sync_sides_.load() && is_side_zone(z)) resync_pending_.store(true);
}

void AccessoryLeds::set_sync_sides(bool on) {
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        cfg_.sync_sides = on;
    }
    sync_sides_.store(on);
    if (on) resync_pending_.store(true);   // align the side zones from now
}

void AccessoryLeds::set_link_areas(bool on) {
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        cfg_.link_areas = on;
    }
    link_areas_.store(on);
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

void AccessoryLeds::set_zone_level_style(Zone z, LevelStyle s) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].level_style = s;
}

void AccessoryLeds::set_zone_min_level(Zone z, float f) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (f < 0.f) f = 0.f;
    if (f > 1.f) f = 1.f;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].min_level = f;
}

void AccessoryLeds::set_zone_sound_trigger(Zone z, bool on) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].sound_trigger = on;
}

void AccessoryLeds::set_zone_sound_threshold(Zone z, float t) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].sound_threshold = t;
}

void AccessoryLeds::set_zone_sound_decay(Zone z, float per_sec) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (per_sec < 0.05f) per_sec = 0.05f;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].sound_decay = per_sec;
}

void AccessoryLeds::set_zone_sound_complete(Zone z, bool on) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].sound_complete = on;
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
    group_t0_  = 0.0;                     // side-zone phase origin; t restarts at 0 too
    peak_hold_ = 0.f;
    prev_t_    = 0.0;
    for (auto& e : sound_env_) e.store(0.f);
    for (int i = 0; i < ZoneCount; ++i) {
        sound_prev_above_[i]  = false;
        zone_phase0_[i]       = 0.0;
        sound_hold_until_[i]  = 0.0;
        pulses_[i].clear();
    }

    while (running_.load()) {
        const auto next_t = clock::now() + period;
        const double t = std::chrono::duration<double>(clock::now() - t0).count();

        // Consume a pending side-zone resync: stamp the shared phase origin to
        // "now" so the synced side zones' effects (re)start from phase 0.
        if (resync_pending_.exchange(false)) group_t0_ = t;
        const bool sync_sides = sync_sides_.load();

        // Level "Peak" marker: track the peak of the (global) mic volume and let
        // it fall at peak_decay_ per second — a real-VU-meter falling dot.
        const double frame_dt = std::max(0.0, t - prev_t_);
        prev_t_ = t;
        const float vol_now = std::clamp(audio_volume_.load(), 0.0f, 1.0f);
        peak_hold_ = std::max(peak_hold_ - peak_decay_.load() * static_cast<float>(frame_dt),
                              vol_now);

        // Snapshot zone state under the lock; release before any SPI work so
        // the menu thread doesn't stall on the SPI write.
        std::array<ZoneConfig, ZoneCount> zones;
        uint8_t gbright;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            zones   = cfg_.zones;
            gbright = cfg_.global_brightness;
        }

        // Link areas: extend each side's hub effect across its fin so the pair
        // renders as ONE continuous area — the fin adopts the hub's look and its
        // fraction continues from the hub. Non-destructive (local snapshot only).
        std::array<std::vector<float>, ZoneCount> lf_over;
        if (link_areas_.load()) {
            const int pairs[2][2] = {
                { static_cast<int>(Zone::LeftCheekhub),  static_cast<int>(Zone::LeftFin)  },
                { static_cast<int>(Zone::RightCheekhub), static_cast<int>(Zone::RightFin) } };
            for (const auto& pr : pairs) {
                ZoneConfig& H = zones[pr[0]];
                ZoneConfig& F = zones[pr[1]];
                if (H.count <= 0 || F.count <= 0) continue;
                copy_zone_look(F, H);           // fin runs the hub's exact effect
                if (H.grad_spatial)
                    zone_group_spatial_fracs(H, F, H.grad_angle, lf_over[pr[0]], lf_over[pr[1]]);
                else
                    zone_group_len_fracs(H, F, lf_over[pr[0]], lf_over[pr[1]]);
            }
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
            // Sound gate. Two modes:
            //  - gate (default): the pattern's brightness follows the mic (open
            //    above threshold, else fall at sound_decay/sec).
            //  - complete-on-trigger (pulse): each trigger EMITS a travelling
            //    pulse; pulses overlap and each completes on its own, so a new
            //    sound fires another sweep without disturbing those in flight.
            const float sdecay = z.sound_decay * static_cast<float>(frame_dt);
            float sgate = 1.f;
            const std::vector<float>* pulse_ptr = nullptr;
            if (z.sound_trigger) {
                const bool above  = (vol_now >= z.sound_threshold);
                const bool rising = above && !sound_prev_above_[zi];
                if (z.sound_complete) {
                    // Advance + prune live pulses, then emit one on a rising
                    // edge. Head runs 0→1 (or 1→0 for a negative Wave Speed) at
                    // the Wave Speed rate; a stalled speed falls back to 1/s.
                    float sp = z.wave_speed;
                    if (std::fabs(sp) < 0.05f) sp = 1.0f;
                    const float step = sp * static_cast<float>(frame_dt);
                    for (auto& ph : pulses_[zi]) ph += step;
                    pulses_[zi].erase(
                        std::remove_if(pulses_[zi].begin(), pulses_[zi].end(),
                            [](float p){ return p > 1.2f || p < -0.2f; }),
                        pulses_[zi].end());
                    if (rising && pulses_[zi].size() < 16)
                        pulses_[zi].push_back(sp >= 0.f ? 0.f : 1.f);
                    pulse_ptr = &pulses_[zi];
                    // Preview hint: "playing" while any pulse is alive.
                    sound_env_[zi].store(pulses_[zi].empty() ? 0.f : 1.f);
                } else {
                    if (above) sound_env_[zi].store(1.f);
                    else sound_env_[zi].store(std::max(0.f, sound_env_[zi].load() - sdecay));
                    sgate = sound_env_[zi].load();
                    pulses_[zi].clear();
                }
                sound_prev_above_[zi] = above;
            } else {
                sound_env_[zi].store(0.f);
                sound_prev_above_[zi] = false;
                pulses_[zi].clear();
            }

            // Effect time: side zones share group_t0_ when sync is on.
            const double zt = (sync_sides && is_side_zone(static_cast<Zone>(zi)))
                                ? (t - group_t0_) : t;

            zone_base_colors(z, zt, vol, face_color_for(static_cast<Zone>(zi)),
                             ramp, ramp_n, zbuf, peak_hold_, sgate,
                             lf_over[zi].empty() ? nullptr : &lf_over[zi],
                             pulse_ptr);
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

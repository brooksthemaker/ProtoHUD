#pragma once
// ── accessory_leds.h ──────────────────────────────────────────────────────────
// Zone-aware manager for the accessory LED chain — cheekhubs + fins on a
// single WS2812 daisy-chain off the Pi's SPI0 MOSI line. Owns the LedStrip
// + a render thread that pushes a frame every ~16 ms with the current
// per-zone pattern composited in. Patterns are stateless functions of (t,
// zone params); future audio/event hooks (volume → Level, boop → Flash) only
// have to flip a zone's pattern and update its color.
//
// Threading: all menu / hook calls go through set_zone_*; they only touch
// the per-zone Pattern state under cfg_mtx_. The render thread reads under
// the same lock, composites, and writes the strip with no other locks held.

#include "led_strip.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace accessory {

enum class Zone : uint8_t {
    LeftCheekhub  = 0,
    RightCheekhub = 1,
    LeftFin       = 2,
    RightFin      = 3,
    Blush         = 4,
};
constexpr int ZoneCount = 5;

enum class Pattern : uint8_t {
    Off      = 0,   // zone stays dark regardless of color
    Solid    = 1,   // fill with (r, g, b)
    Breathe  = 2,   // (r, g, b) × half-cosine envelope at breathe_hz
    Level    = 3,   // (r, g, b) × current audio volume (mic-reactive bar)
    Chase    = 4,   // bright dot + fading tail walking the zone at breathe_hz
    Sparkle  = 5,   // per-pixel random twinkle
    Gradient = 6,   // color→color2 down the length (rings/lines); scrolls at wave_speed
    Wave     = 7,   // a bright band of (r,g,b) travelling down the length at wave_speed
};

// A zone's physical topology. A hub is concentric RINGS, a fin is stacked
// LINES; both are just an ordered list of "sections", each a run of LEDs wired
// in series (section 0 = base/innermost = DIN side by default). `sections`
// holds the LED count per ring/line; when it's non-empty, `count` is their sum.
// An empty `sections` means a plain linear run of `count` LEDs (shape "single").
enum class Shape : uint8_t { Single = 0, Rings = 1, Lines = 2 };

struct ZoneConfig {
    std::string name;
    int         start = 0;                       // first index on the shared chain
    int         count = 0;                        // total LEDs (= sum(sections) if set)
    Pattern     pattern    = Pattern::Solid;
    uint8_t     r = 0, g = 220, b = 180;        // primary color (teal default)
    uint8_t     r2 = 230, g2 = 60, b2 = 180;    // second color (Gradient endpoint)
    // Multi-stop custom gradient: one color stop per entry, blended evenly
    // across the zone's length (len 0→1). ≥2 stops overrides the r/g/b→r2/g2/b2
    // 2-color gradient; <2 falls back to it. Typically one stop per section.
    std::vector<std::array<uint8_t, 3>> stops;
    float       breathe_hz = 0.5f;              // half-cycle / chase-cycle per second
    float       wave_speed = 0.5f;              // Gradient scroll / Wave travel, cycles/s
    bool        grad_spatial = false;           // Gradient/Wave sweep across the 2D shape (vs along the wire/length)
    float       grad_angle   = 0.f;             // degrees: direction the spatial sweep travels
    uint8_t     zone_brightness = 255;          // per-zone scale on top of global
    bool        follow_face = false;            // color tracks the face's mean color

    // Topology + placement (drives the editor visualizer + length-aware effects).
    Shape            shape = Shape::Single;
    std::vector<int> sections;                   // LEDs per ring/line, base→tip
    int              pos_x = 0, pos_y = 0;       // zone centre on the layout canvas
    float            rotation = 0.f;             // degrees, clockwise
    float            band_spacing = 1.f;         // preview: ring/line gap scale (cosmetic)
    float            scale  = 1.f;               // preview: overall size multiplier (cosmetic)
    bool             mirror = false;             // preview: flip the zone left↔right (cosmetic)
    float            line_align = 0.f;           // preview: justify uneven lines: -1 left, 0 centre, +1 right (cosmetic)
    bool             reverse    = false;         // led 0 at the far (tip) end
    bool             serpentine = false;         // consecutive sections alternate direction
};

// One LED's place in a zone, in wiring (DIN→DOUT) order: canvas position for the
// visualizer, which section it belongs to, and its 0..1 fraction DOWN THE LENGTH
// (base=0, tip=1) that the Gradient/Wave patterns use. Built by zone_geometry().
struct LedPoint {
    float x, y;
    int   section;
    float len_frac;
};
// Full per-LED geometry for a zone, in wiring order (respecting reverse/serpentine).
std::vector<LedPoint> zone_geometry(const ZoneConfig& z);
// Just the per-LED length fractions in wiring order — what the render loop needs
// (no x/y), cheaper than the full geometry.
std::vector<float> zone_len_fracs(const ZoneConfig& z);
// Per-LED fraction 0..1 along a gradient AXIS at angle_deg (canvas space): each
// LED's zone_geometry position projected onto (cos,sin) of the angle and
// normalised across the zone. Lets a Gradient/Wave sweep across the 2D shape at
// any angle instead of following the wiring/section order. Same (wiring) order
// as zone_len_fracs so it drops into the render loop the same way.
std::vector<float> zone_spatial_fracs(const ZoneConfig& z, float angle_deg);
// Recompute count = sum(sections) (or leave count if sections is empty). Call
// after editing sections so the chain layout stays consistent.
int zone_total(const ZoneConfig& z);
// Per-pixel base colors for a zone at time t — the pattern envelope × per-pixel
// factor × color, BEFORE the flash overlay and global brightness. Fills `out`
// with count*3 RGB in strip-index order. Shared by the render loop and the
// editor's live preview so the preview matches what the LEDs will actually do.
//
// face_color is the flat follow color (used when no ramp). face_ramp (may be
// null) is a per-length follow LUT: when the zone follows the face, each LED
// samples it at its length fraction so the strip mirrors the eye's gradient,
// scaled to the zone's LED count. Pass ramp_n=0 / face_ramp=null for flat.
void zone_base_colors(const ZoneConfig& z, double t, float vol,
                      uint32_t face_color,
                      const uint32_t* face_ramp, int ramp_n,
                      std::vector<uint8_t>& out);

class AccessoryLeds {
public:
    struct Config {
        bool                          enabled = false;
        // "spidev" = drive the chain from the Pi's SPI MOSI (LedStrip); "coproc"
        // = compute frames on the Pi and stream them to the RP2350 coprocessor,
        // which drives the WS2812 on its own pin (kLedZonePin). Same zones,
        // patterns and editor either way — only the physical data wire moves.
        std::string                   transport = "spidev";
        LedStrip::Config              strip;
        std::array<ZoneConfig, ZoneCount> zones{};
        uint8_t                       global_brightness = 64;
        double                        frame_hz          = 60.0;
    };

    explicit AccessoryLeds(Config cfg);
    ~AccessoryLeds();

    AccessoryLeds(const AccessoryLeds&)            = delete;
    AccessoryLeds& operator=(const AccessoryLeds&) = delete;

    bool start();
    void stop();
    // Master switch: turns the chain on/off live (starts or stops the render
    // thread). The transport is fixed at construction, so this runs on whatever
    // transport the process started with.
    void set_enabled(bool on);

    // Rebuild the chain live from a new Config — resizes the strip/frame to the
    // new total and re-chains the zones — WITHOUT restarting the process. Keeps
    // the current transport (that stays restart-only) and the frame sink. Used
    // by the editor's "Apply Layout" so LED-count edits take effect immediately.
    void reconfigure(const Config& cfg);

    bool is_running() const { return running_.load(); }

    // Coprocessor transport: when set, the render loop streams each composited
    // RGB frame to this sink (wired to CoprocInputs::send_led_frame) instead of
    // the Pi SPI strip. Set once after the coproc link exists, before start().
    void set_frame_sink(std::function<void(const uint8_t* rgb, int count)> fn) {
        frame_sink_ = std::move(fn);
    }
    bool uses_coproc() const { return coproc_; }
    int  total_count() const { return strip_ ? strip_->count() : 0; }

    // Per-zone tunables — picked up by the next render tick (≈ next 16 ms).
    void set_zone_pattern  (Zone, Pattern);
    void set_zone_color    (Zone, uint8_t r, uint8_t g, uint8_t b);
    void set_zone_color2   (Zone, uint8_t r, uint8_t g, uint8_t b);
    void set_zone_stops    (Zone, std::vector<std::array<uint8_t, 3>> stops);
    void set_zone_breathe_hz(Zone, float hz);
    void set_zone_wave_speed(Zone, float hz);
    void set_zone_grad_spatial(Zone, bool on);
    void set_zone_grad_angle(Zone, float deg);
    void set_zone_brightness(Zone, uint8_t b);
    void set_zone_follow_face(Zone, bool on);
    void set_global_brightness(uint8_t b);

    // Latest face mean colors (fed ~5 Hz when any zone follows the face), packed
    // 0xRRGGBB. Left/right = the mean of the left/right half of the face canvas
    // (so a cheek follows its own eye); whole = the full-face mean (for centre
    // zones like blush). All lock-free atomics.
    static uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
        return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    }
    void set_face_color(uint8_t r, uint8_t g, uint8_t b)       { face_color_.store(pack(r,g,b)); }
    void set_face_color_left(uint8_t r, uint8_t g, uint8_t b)  { face_color_left_.store(pack(r,g,b)); }
    void set_face_color_right(uint8_t r, uint8_t g, uint8_t b) { face_color_right_.store(pack(r,g,b)); }

    // Per-length follow LUTs: the eye's material color sampled across its width
    // (index 0 = one edge, n-1 = the other). Left/right = each eye's half;
    // whole = the full face. Fed ~5 Hz when any zone follows the face; a follow
    // zone samples its side's ramp by each LED's length fraction so the strip
    // reproduces the eye's gradient. Guarded by face_ramp_mtx_ (arrays, not
    // single words, so an atomic won't do).
    void set_face_ramp_whole(const uint32_t* d, int n);
    void set_face_ramp_left (const uint32_t* d, int n);
    void set_face_ramp_right(const uint32_t* d, int n);
    // The ramp a given zone follows (copy under lock): each eye's half for the
    // cheek hubs/fins, the whole face for everything else (blush). Empty until
    // the first feed — callers then fall back to the flat face_color_for().
    std::vector<uint32_t> face_ramp_for(Zone z) const;

    // The face color a given zone follows: its own eye's half for the cheek
    // hubs/fins, the whole-face mean for everything else (blush). Used by the
    // render loop and the editor preview so both match.
    uint32_t face_color_for(Zone z) const {
        switch (z) {
            case Zone::LeftCheekhub:  case Zone::LeftFin:  return face_color_left_.load();
            case Zone::RightCheekhub: case Zone::RightFin: return face_color_right_.load();
            default:                                       return face_color_.load();
        }
    }
    uint32_t face_color()   const { return face_color_.load(); }
    float    audio_volume() const { return audio_volume_.load(); }

    // Live audio volume in [0, 1] — drives Pattern::Level brightness. Audio
    // thread writes; render thread reads. Atomic, no lock.
    void set_audio_volume(float v) { audio_volume_.store(v); }

    // Fire a brief white flash on a zone (event overlay; survives the
    // current Pattern). duration_s controls the fade-out time. Boop events
    // call this from the sensor thread in main.cpp; the menu may also have
    // a "Test Flash" leaf later.
    void trigger_flash(Zone z, double duration_s);

    // Read-only snapshot for the menu.
    ZoneConfig zone(Zone z) const;
    uint8_t    global_brightness() const;

    // Range of LED indices for each zone, for callers that want to do their
    // own writes (e.g. event-flash overlays from main.cpp). Indices are into
    // the underlying strip's pixel array.
    int zone_start(Zone z) const { return cfg_.zones[static_cast<int>(z)].start; }
    int zone_count(Zone z) const { return cfg_.zones[static_cast<int>(z)].count; }

private:
    void render_loop();
    void stop_thread();   // stop + join the render thread WITHOUT closing the strip

    Config            cfg_;
    std::unique_ptr<LedStrip> strip_;     // rebuilt on reconfigure()
    bool              coproc_ = false;    // cfg_.transport == "coproc"
    std::vector<uint8_t> frame_;          // composited RGB, 3 bytes/pixel
    std::function<void(const uint8_t*, int)> frame_sink_;   // coproc frame push
    double            last_send_ = 0.0;   // coproc send throttle (loop seconds)
    std::mutex        cfg_mtx_;       // guards cfg_.zones + global_brightness
    std::atomic<bool> running_ { false };
    std::thread       thread_;

    std::atomic<float> audio_volume_ { 0.0f };
    std::atomic<uint32_t> face_color_       { 0x00DCB4 };   // whole-face follow feed
    std::atomic<uint32_t> face_color_left_  { 0x00DCB4 };   // left half (left cheek zones)
    std::atomic<uint32_t> face_color_right_ { 0x00DCB4 };   // right half (right cheek zones)

    mutable std::mutex    face_ramp_mtx_;                   // guards the three ramps
    std::vector<uint32_t> face_ramp_whole_;                 // per-length follow LUTs
    std::vector<uint32_t> face_ramp_left_;
    std::vector<uint32_t> face_ramp_right_;

    // Flash overlay state — paired (start, end) microseconds since steady
    // clock epoch. atomic<int64_t> keeps trigger_flash lock-free.
    using us_t = int64_t;
    std::atomic<us_t> flash_start_us_[ZoneCount]{};
    std::atomic<us_t> flash_end_us_  [ZoneCount]{};
};

} // namespace accessory

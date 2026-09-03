#pragma once
// ── servo_controller.h ────────────────────────────────────────────────────────
// Named, calibrated coprocessor servos with smooth motion — mainly the ears,
// driven by expression actions. Each servo carries safe travel LIMITS so a
// trigger can never command it past its mechanical stops, a rest pose, and a
// slew speed. Motion is done on the coprocessor: this class just sends one
// "SERVOM <ch> <deg> <speed>" target per move (via the sink) and the RP2350
// eases toward it, so ear moves stay smooth even when the CM5 is busy.
//
// Pairing: two servos (the left/right ears) can name each other as `partner`,
// letting an expression drive both from one action — Copy (partner takes the
// same angle) or Mirror (partner reflects about its own centre). Independent
// motion is just two separate actions, one per ear.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace servo {

struct ServoConfig {
    std::string name       = "Servo";
    // Coprocessor servo channel. With a PCA9685 fitted (the normal case) that's
    // one of its 16 board outputs; without one, the firmware falls back to four
    // direct-GPIO channels, so only 0-3 exist. Either way this is a CHANNEL
    // NUMBER — the host never addresses pins.
    int         channel    = 0;      // 0..15 (PCA9685) / 0..3 (GPIO fallback)
    int         min_deg    = 0;      // safe travel window (hard clamp)
    int         max_deg    = 180;
    int         center_deg = 90;     // neutral — the Mirror reflection point
    int         rest_deg   = 90;     // idle / revert pose
    float       speed      = 120.f;  // slew deg/s (0 = snap)
    int         partner    = -1;     // paired servo index (the other ear), -1 = none
    // Pulse window in microseconds that 0..180 deg maps onto — this is what sets
    // how much TRAVEL the servo really has. The MCU's Servo library defaults to a
    // narrow 1000-2000, which yields only ~half a sweep and leaves the servo
    // clicking against its stop; 500-2500 is the usual full range. Narrow it if a
    // particular servo buzzes at the extremes.
    int         min_us     = 500;
    int         max_us     = 2500;
};

enum class Pair : uint8_t { None = 0, Copy = 1, Mirror = 2 };

// Self-check sweep modes. Sequential walks ONE servo at a time so you can watch
// each ear move on its own and confirm the right channel drives the right side;
// Together moves them all at once, which is the quicker "is anything dead?" pass.
enum class CheckMode : uint8_t { Sequential = 0, Together = 1 };

class ServoController {
public:
    struct Config { std::vector<ServoConfig> servos; };

    explicit ServoController(Config cfg) : cfg_(std::move(cfg)) {}

    // Wire to CoprocInputs::send_servo_move(channel, degrees, speed). Set once
    // after the coproc link exists; moves before it is set are dropped.
    void set_sink(std::function<void(int, int, int)> fn) { sink_ = std::move(fn); }
    // Wire to CoprocInputs::send_servo_calibration(channel, min_us, max_us).
    void set_cal_sink(std::function<void(int, int, int)> fn) { cal_sink_ = std::move(fn); }

    // Push every servo's pulse window to the MCU. Call once the link is up, and
    // again whenever the window is edited — the firmware re-attaches to apply it.
    void push_calibration() const {
        if (!cal_sink_) return;
        for (const auto& s : cfg_.servos) cal_sink_(s.channel, s.min_us, s.max_us);
    }
    void push_calibration_one(int idx) const {
        const ServoConfig* s = get(idx);
        if (s && cal_sink_) cal_sink_(s->channel, s->min_us, s->max_us);
    }

    int  count() const { return static_cast<int>(cfg_.servos.size()); }
    const ServoConfig* get(int i) const {
        return (i >= 0 && i < count()) ? &cfg_.servos[i] : nullptr;
    }
    ServoConfig* mutable_get(int i) {
        return (i >= 0 && i < count()) ? &cfg_.servos[i] : nullptr;
    }
    std::vector<ServoConfig>&       servos()       { return cfg_.servos; }
    const std::vector<ServoConfig>& servos() const { return cfg_.servos; }

    // Move one servo to an angle, clamped to its limits, at its configured speed.
    void move(int idx, int deg) const {
        const ServoConfig* s = get(idx);
        if (!s || !sink_) return;
        const int lo = std::min(s->min_deg, s->max_deg);
        const int hi = std::max(s->min_deg, s->max_deg);
        sink_(s->channel, std::clamp(deg, lo, hi), static_cast<int>(s->speed + 0.5f));
    }
    void rest_one(int idx) const {
        if (const ServoConfig* s = get(idx)) move(idx, s->rest_deg);
    }
    void rest_all() const { for (int i = 0; i < count(); ++i) rest_one(i); }

    // Stop driving a servo so it goes limp — for adjusting the mechanism by hand.
    // Bypasses the travel clamp on purpose: -1 is a detach command, not an angle.
    // Any later move() re-attaches it.
    void detach(int idx) const {
        const ServoConfig* s = get(idx);
        if (s && sink_) sink_(s->channel, -1, 0);
    }

    // ── All-servo helpers (the "check them all" rows) ─────────────────────────
    // Each still goes through move(), so every servo is clamped to its OWN travel
    // window — "all to max" means each one's own max, not one shared angle.
    void detach_all() const { for (int i = 0; i < count(); ++i) detach(i); }
    void move_all(int deg) const { for (int i = 0; i < count(); ++i) move(i, deg); }
    void all_to_min() const {
        for (int i = 0; i < count(); ++i)
            if (const ServoConfig* s = get(i)) move(i, std::min(s->min_deg, s->max_deg));
    }
    void all_to_max() const {
        for (int i = 0; i < count(); ++i)
            if (const ServoConfig* s = get(i)) move(i, std::max(s->min_deg, s->max_deg));
    }
    void all_to_center() const {
        for (int i = 0; i < count(); ++i)
            if (const ServoConfig* s = get(i)) move(i, s->center_deg);
    }

    // Expression apply: move servo `idx` to `deg`, and (for a Copy/Mirror pair)
    // drive its partner — Copy = same angle, Mirror = reflected about the
    // partner's centre. Limits are enforced per servo inside move().
    void apply(int idx, int deg, Pair pair) const {
        move(idx, deg);
        const ServoConfig* s = get(idx);
        if (pair != Pair::None && s && s->partner >= 0)
            move(s->partner, pair == Pair::Copy ? deg : mirror_deg(s->partner, deg));
    }
    // Expression revert: primary returns to the action's rest (or its own rest
    // when rest < 0); the partner returns to its own configured rest.
    void revert(int idx, int rest, Pair pair) const {
        const ServoConfig* s = get(idx);
        if (s) move(idx, rest >= 0 ? rest : s->rest_deg);
        if (pair != Pair::None && s && s->partner >= 0) rest_one(s->partner);
    }

    // ── Self-check sweep ──────────────────────────────────────────────────────
    // Walks every servo through centre -> min -> max -> centre -> rest so a
    // glance confirms all of them are wired, on the channel you think they are,
    // and reaching their real travel. Driven by tick(dt) rather than sleeping:
    // the menu runs on the same thread, so a blocking sweep would freeze the UI
    // and you could not hit Stop.
    void start_check(CheckMode mode, bool loop = false) {
        if (count() == 0) return;
        chk_ = Check{};
        chk_.active = true;
        chk_.loop   = loop;
        chk_.mode   = mode;
        chk_.status = "starting check";
        // Seed each servo's assumed position from its rest pose — startup parks
        // there and every check ends there, so the first step's travel estimate
        // is right in the normal case (and only affects dwell timing if not).
        chk_.at.resize(static_cast<size_t>(count()));
        for (int i = 0; i < count(); ++i)
            chk_.at[static_cast<size_t>(i)] = get(i)->rest_deg;
    }
    // Cancel and park — never leave an ear stopped mid-sweep.
    void stop_check() {
        const bool was = chk_.active;
        chk_ = Check{};
        if (was) rest_all();
    }
    bool        checking()     const { return chk_.active; }
    std::string check_status() const { return chk_.status; }

    // Call once per frame from the main loop.
    void tick(float dt) {
        if (!chk_.active) return;
        if (chk_.wait > 0.f) { chk_.wait -= dt; return; }
        const int n = count();
        if (n == 0) { stop_check(); return; }

        // Previous step's dwell has elapsed. Advance the cursor first, so the
        // terminal case is detected only AFTER the last step has been held.
        if (chk_.step >= kCheckSteps) {
            chk_.step = 0;
            const bool more = (chk_.mode == CheckMode::Sequential) && (chk_.idx + 1 < n);
            if (more) {
                ++chk_.idx;
            } else if (chk_.loop) {
                chk_.idx = 0;
            } else {
                // The last step IS rest, so everything is already parked.
                chk_.active = false;
                chk_.status = "check complete";
                return;
            }
        }

        float dwell = 0.f;
        if (chk_.mode == CheckMode::Together) {
            for (int i = 0; i < n; ++i)
                dwell = std::max(dwell, issue_check_step(i, chk_.step));
            chk_.status = std::string("All servos \xe2\x86\x92 ") + check_step_name(chk_.step)
                        + " (" + std::to_string(chk_.step + 1) + "/"
                        + std::to_string(kCheckSteps) + ")";
        } else {
            dwell = issue_check_step(chk_.idx, chk_.step);
            const ServoConfig* s = get(chk_.idx);
            chk_.status = (s ? s->name : std::string("Servo"))
                        + " \xe2\x86\x92 " + check_step_name(chk_.step)
                        + "  [" + std::to_string(chk_.idx + 1) + "/"
                        + std::to_string(n) + "]";
        }
        chk_.wait = dwell;
        ++chk_.step;
    }

private:
    int mirror_deg(int idx, int deg) const {
        const ServoConfig* s = get(idx);
        return s ? (2 * s->center_deg - deg) : deg;
    }

    // The check pattern. Ends on rest so a completed sweep leaves the mechanism
    // parked; centre appears twice on purpose (it frames each extreme, which is
    // what makes an off-centre or reversed servo obvious to the eye).
    static constexpr int kCheckSteps = 5;
    static const char* check_step_name(int step) {
        switch (step) {
            case 0:  return "centre";
            case 1:  return "min";
            case 2:  return "max";
            case 3:  return "centre";
            default: return "rest";
        }
    }
    int check_step_angle(const ServoConfig& s, int step) const {
        switch (step) {
            case 0:  return s.center_deg;
            case 1:  return std::min(s.min_deg, s.max_deg);
            case 2:  return std::max(s.min_deg, s.max_deg);
            case 3:  return s.center_deg;
            default: return s.rest_deg;
        }
    }
    // Command one step; return how long to hold it. The dwell is that servo's own
    // travel time at its own slew speed plus a margin, so a slow servo is really
    // given time to arrive before the next step, and a fast one doesn't stall the
    // sweep. Speed 0 means the firmware snaps, so only the margin applies.
    float issue_check_step(int idx, int step) {
        const ServoConfig* s = get(idx);
        if (!s) return 0.f;
        const int lo     = std::min(s->min_deg, s->max_deg);
        const int hi     = std::max(s->min_deg, s->max_deg);
        const int target = std::clamp(check_step_angle(*s, step), lo, hi);
        const int from   = (idx < static_cast<int>(chk_.at.size()))
                             ? chk_.at[static_cast<size_t>(idx)] : s->rest_deg;
        move(idx, target);
        if (idx < static_cast<int>(chk_.at.size()))
            chk_.at[static_cast<size_t>(idx)] = target;
        const float dist   = static_cast<float>(std::abs(target - from));
        const float travel = (s->speed > 0.f) ? dist / s->speed : 0.f;
        // The ceiling only exists so a servo left at a crawling Speed can't wedge
        // the check; 10s covers a full sweep at 20 deg/s.
        return std::clamp(travel + 0.35f, 0.30f, 10.f);
    }

    struct Check {
        bool        active = false;
        bool        loop   = false;
        CheckMode   mode   = CheckMode::Sequential;
        int         idx    = 0;      // which servo (Sequential only)
        int         step   = 0;      // cursor into the step pattern
        float       wait   = 0.f;    // seconds left holding the current step
        std::string status = "idle";
        std::vector<int> at;         // last angle commanded, for travel timing
    };

    Config cfg_;
    Check  chk_;
    std::function<void(int, int, int)> sink_;
    std::function<void(int, int, int)> cal_sink_;
};

} // namespace servo

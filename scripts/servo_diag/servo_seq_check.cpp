// Headless check of ServoController's self-check sequencer: step order, the
// sequential/together split, dwell sizing vs each servo's Speed, loop mode, and
// that a completed run leaves everything parked at rest.
//   g++ -std=c++17 -Wall -Wextra -I../../../../../home/serin/protohud/src 
//       servo_seq_check.cpp -o servo_seq_check
#include <cstdio>
#include <string>
#include <vector>

#include "servo/servo_controller.h"

struct Cmd { int ch, deg, speed; };
static std::vector<Cmd> sent;

static servo::ServoController make(int n) {
    servo::ServoController::Config c;
    for (int i = 0; i < n; ++i) {
        servo::ServoConfig s;
        s.name = "S" + std::to_string(i);
        s.channel = i;
        s.min_deg = 30; s.max_deg = 150; s.center_deg = 90; s.rest_deg = 80;
        s.speed = (i == 0) ? 30.f : 120.f;      // servo 0 deliberately slow
        c.servos.push_back(s);
    }
    return servo::ServoController(std::move(c));
}

// Run a check to completion (or until the frame budget runs out).
static int run(servo::ServoController& sv, servo::CheckMode m, bool loop, int max_frames) {
    sent.clear();
    sv.start_check(m, loop);
    int frames = 0;
    const float dt = 1.f / 60.f;
    while (sv.checking() && frames < max_frames) { sv.tick(dt); ++frames; }
    return frames;
}

static bool fail = false;
static void expect(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) fail = true;
}

int main() {
    // ── Sequential over 4 servos ──────────────────────────────────────────────
    {
        auto sv = make(4);
        sv.set_sink([](int ch, int deg, int sp){ sent.push_back({ch, deg, sp}); });
        const int frames = run(sv, servo::CheckMode::Sequential, false, 60 * 400);
        std::printf("Sequential, 4 servos: %d commands over %d frames (%.1fs)\n",
                    (int)sent.size(), frames, frames / 60.f);
        expect(sent.size() == 20, "20 commands (4 servos x 5 steps)");
        // Channel order must be strictly grouped: 0,0,0,0,0,1,1,1,1,1,...
        bool grouped = true;
        for (int i = 0; i < (int)sent.size(); ++i)
            if (sent[i].ch != i / 5) grouped = false;
        expect(grouped, "one servo at a time, in channel order");
        // Step pattern per servo: centre, min, max, centre, rest (clamped).
        bool pattern = true;
        for (int s = 0; s < 4; ++s) {
            const int want[5] = { 90, 30, 150, 90, 80 };
            for (int k = 0; k < 5; ++k)
                if (sent[s * 5 + k].deg != want[k]) pattern = false;
        }
        expect(pattern, "steps are centre->min->max->centre->rest, clamped");
        expect(!sv.checking() && sv.check_status() == "check complete",
               "finishes on its own and reports completion");
        // Last command per servo is its rest angle -> nothing left mid-travel.
        bool parked = true;
        for (int s = 0; s < 4; ++s) if (sent[s * 5 + 4].deg != 80) parked = false;
        expect(parked, "every servo ends parked at rest");
        // Each command carries that servo's own configured slew speed.
        bool speeds = true;
        for (const auto& c : sent)
            if (c.speed != (c.ch == 0 ? 30 : 120)) speeds = false;
        expect(speeds, "each move carries that servo's own Speed");
    }

    // ── Together ──────────────────────────────────────────────────────────────
    {
        auto sv = make(4);
        sv.set_sink([](int ch, int deg, int sp){ sent.push_back({ch, deg, sp}); });
        const int frames = run(sv, servo::CheckMode::Together, false, 60 * 400);
        std::printf("\nTogether, 4 servos: %d commands over %d frames (%.1fs)\n",
                    (int)sent.size(), frames, frames / 60.f);
        expect(sent.size() == 20, "20 commands (5 steps x 4 servos)");
        // Now grouped by STEP instead: each block of 4 is one step, all channels.
        bool by_step = true;
        for (int k = 0; k < 5; ++k) {
            for (int i = 0; i < 4; ++i)
                if (sent[k * 4 + i].ch != i) by_step = false;
            const int want[5] = { 90, 30, 150, 90, 80 };
            for (int i = 0; i < 4; ++i)
                if (sent[k * 4 + i].deg != want[k]) by_step = false;
        }
        expect(by_step, "all servos move on each step, together");
        expect(frames < 60 * 60, "together is much faster than sequential");
    }

    // ── Dwell honours the slow servo ──────────────────────────────────────────
    {
        // Servo 0 is 30 deg/s. Each command's dwell must cover THAT command's
        // travel: the hold after "go to min" covers centre(90)->min(30) = 60 deg
        // = 2.0s, and the hold after "go to max" covers min->max = 120 deg = 4.0s.
        auto sv = make(1);
        std::vector<float> stamps;
        float now = 0.f;
        sv.set_sink([&](int, int, int){ stamps.push_back(now); });
        sv.start_check(servo::CheckMode::Sequential, false);
        const float dt = 1.f / 60.f;
        for (int f = 0; f < 60 * 200 && sv.checking(); ++f) { sv.tick(dt); now += dt; }
        std::printf("\nDwell timing, 1 slow servo (30 deg/s): %d steps\n",
                    (int)stamps.size());
        bool ok = stamps.size() == 5;
        if (ok) {
            const float to_min = stamps[2] - stamps[1];  // holds centre->min (2.0s)
            const float to_max = stamps[3] - stamps[2];  // holds min->max   (4.0s)
            std::printf("  hold after min-command %.2fs (needs 2.00s)\n", to_min);
            std::printf("  hold after max-command %.2fs (needs 4.00s)\n", to_max);
            ok = to_min >= 2.0f && to_max >= 4.0f;
            // And the dwell must SCALE with distance, not be a flat delay.
            ok = ok && to_max > to_min * 1.8f;
        }
        expect(ok, "each hold covers that move's own travel time, scaled");
    }

    // ── Loop mode + stop ──────────────────────────────────────────────────────
    {
        auto sv = make(2);
        sv.set_sink([](int ch, int deg, int sp){ sent.push_back({ch, deg, sp}); });
        sent.clear();
        sv.start_check(servo::CheckMode::Together, true);
        const float dt = 1.f / 60.f;
        for (int f = 0; f < 60 * 120; ++f) sv.tick(dt);
        std::printf("\nLoop mode, 2 servos, 120s: %d commands\n", (int)sent.size());
        expect(sv.checking(), "still running after a full pass (loop)");
        expect(sent.size() > 20, "cycles past the first pass");
        const size_t before = sent.size();
        sv.stop_check();
        expect(!sv.checking(), "stop_check ends it");
        expect(sent.size() == before + 2, "stop parks both servos (rest_all)");
        bool at_rest = true;
        for (size_t i = before; i < sent.size(); ++i)
            if (sent[i].deg != 80) at_rest = false;
        expect(at_rest, "stop sends them to rest, not wherever they were");
    }

    // ── Degenerate: no servos ─────────────────────────────────────────────────
    {
        auto sv = make(0);
        sv.set_sink([](int ch, int deg, int sp){ sent.push_back({ch, deg, sp}); });
        sent.clear();
        sv.start_check(servo::CheckMode::Sequential, false);
        for (int f = 0; f < 600; ++f) sv.tick(1.f / 60.f);
        std::printf("\nNo servos configured: %d commands\n", (int)sent.size());
        expect(!sv.checking() && sent.empty(), "start_check is a no-op with 0 servos");
    }

    // ── No sink yet (link not up) must not spin or crash ──────────────────────
    {
        auto sv = make(4);
        sv.start_check(servo::CheckMode::Sequential, false);
        for (int f = 0; f < 60 * 400 && sv.checking(); ++f) sv.tick(1.f / 60.f);
        std::printf("\nNo sink wired: status \"%s\"\n", sv.check_status().c_str());
        expect(!sv.checking(), "completes harmlessly with no output sink");
    }

    std::printf("\n%s\n", fail ? "*** SOME CHECKS FAILED ***" : "ALL CHECKS PASSED");
    return fail ? 1 : 0;
}

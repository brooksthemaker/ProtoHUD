// Headless check of the accessory-LED overlay layer: the coverage curves, that
// the base pattern really does show through underneath, and that the director's
// rise/hold/retract lifecycle starts and ends clean.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "accessory/accessory_leds.h"
#include "accessory/led_overlay.h"

using namespace accessory;

static bool fail = false;
static void expect(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) fail = true;
}

int main() {
    // ── coverage curves ──────────────────────────────────────────────────────
    std::printf("coverage curves\n");
    {
        ZoneOverlay ov;
        ov.active = true; ov.softness = 0.2f;

        // The property that matters most at rest: amount 0 must be EXACTLY zero
        // everywhere, or a feathered edge leaves colour smeared on the zone.
        ov.amount = 0.f;
        bool clean = true;
        for (int s = 0; s <= 2; ++s) {
            ov.shape = static_cast<OverlayShape>(s);
            for (double f = 0; f <= 1.0001; f += 0.05)
                if (overlay_cover(ov, f) > 0.0) clean = false;
        }
        expect(clean, "amount 0 gives zero coverage for every shape");

        // Rise: fills from f=0 upward, and full coverage at amount 1.
        ov.shape = OverlayShape::Rise;
        ov.amount = 0.5f;
        expect(overlay_cover(ov, 0.0) > 0.99, "Rise: base end fully covered at 50%");
        expect(overlay_cover(ov, 1.0) < 0.01, "Rise: far end untouched at 50%");
        bool monotonic = true;
        double prev = 2.0;
        for (double f = 0; f <= 1.0001; f += 0.05) {
            const double c = overlay_cover(ov, f);
            if (c > prev + 1e-9) monotonic = false;
            prev = c;
        }
        expect(monotonic, "Rise: coverage only decreases along the axis");
        ov.amount = 1.f;
        bool all_covered = true;
        for (double f = 0; f <= 1.0001; f += 0.05)
            if (overlay_cover(ov, f) < 0.99) all_covered = false;
        expect(all_covered, "Rise: amount 1 covers the whole axis");

        // Sweep: a band around phase, base showing on both sides.
        ov.shape = OverlayShape::Sweep;
        ov.amount = 1.f; ov.phase = 0.5f; ov.softness = 0.15f;
        expect(overlay_cover(ov, 0.5) > 0.99, "Sweep: peak at the band centre");
        expect(overlay_cover(ov, 0.0) < 0.01, "Sweep: clear before the band");
        expect(overlay_cover(ov, 1.0) < 0.01, "Sweep: clear behind the band");

        // Bloom: grows from the middle outward.
        ov.shape = OverlayShape::Bloom;
        ov.amount = 0.3f; ov.softness = 0.15f;
        expect(overlay_cover(ov, 0.5) > 0.99, "Bloom: centre covered first");
        expect(overlay_cover(ov, 0.0) < 0.01, "Bloom: edges still clear at 30%");
        ov.amount = 1.f;
        expect(overlay_cover(ov, 0.0) > 0.99 && overlay_cover(ov, 1.0) > 0.99,
               "Bloom: amount 1 reaches both edges");
    }

    // ── the base really shows through ────────────────────────────────────────
    std::printf("\ncompositing over a live pattern\n");
    {
        ZoneConfig z;
        z.count = 40;
        z.pattern = Pattern::Solid;
        z.r = 0; z.g = 220; z.b = 180;      // teal base
        z.zone_brightness = 255;
        z.shape = Shape::Single;

        std::vector<uint8_t> plain, with_ov;
        zone_base_colors(z, 0.0, 0.f, 0, nullptr, 0, plain, -1.f, 1.f,
                         nullptr, nullptr, nullptr);

        ZoneOverlay ov;
        ov.active = true; ov.shape = OverlayShape::Rise;
        ov.r = 255; ov.g = 105; ov.b = 180;  // pink
        ov.opacity = 1.f; ov.softness = 0.1f; ov.amount = 0.4f; ov.angle = 0.f;
        zone_base_colors(z, 0.0, 0.f, 0, nullptr, 0, with_ov, -1.f, 1.f,
                         nullptr, nullptr, &ov);

        // Count LEDs that changed vs the un-overlaid frame.
        int changed = 0, untouched = 0;
        for (int i = 0; i < z.count; ++i) {
            const bool same = plain[3*i] == with_ov[3*i] &&
                              plain[3*i+1] == with_ov[3*i+1] &&
                              plain[3*i+2] == with_ov[3*i+2];
            if (same) ++untouched; else ++changed;
        }
        std::printf("  %d of %d LEDs covered, %d still showing the base\n",
                    changed, z.count, untouched);
        expect(changed > 0, "the overlay covers part of the zone");
        expect(untouched > 0, "the REST still shows the base pattern");

        // At 40% coverage roughly half the strip should remain base.
        expect(untouched > z.count / 4, "an unblended majority remains at 40%");

        // Partial opacity must TINT rather than replace.
        ov.opacity = 0.5f; ov.amount = 1.f;
        std::vector<uint8_t> tinted;
        zone_base_colors(z, 0.0, 0.f, 0, nullptr, 0, tinted, -1.f, 1.f,
                         nullptr, nullptr, &ov);
        const bool blended = tinted[1] > 40 && tinted[1] < 200 && tinted[0] > 40;
        expect(blended, "50% opacity blends base and overlay, not replaces");

        // An Off zone must still take the overlay (blush on a dark cheek).
        z.pattern = Pattern::Off;
        ov.opacity = 1.f; ov.amount = 1.f;
        std::vector<uint8_t> offz;
        zone_base_colors(z, 0.0, 0.f, 0, nullptr, 0, offz, -1.f, 1.f,
                         nullptr, nullptr, &ov);
        expect(offz[0] > 200 && offz[2] > 100, "an Off zone still shows the overlay");
    }

    // ── director lifecycle ───────────────────────────────────────────────────
    std::printf("\ndirector: rise / hold / retract\n");
    {
        AccessoryLeds::Config cfg;
        for (int i = 0; i < ZoneCount; ++i) { cfg.zones[i].count = 10; }
        AccessoryLeds leds(cfg);

        OverlayDirector dir;
        OverlaySpec s;
        s.zone_mask = 0b00011;          // two zones only
        s.mode = OverlayMode::RiseHold;
        s.rise_s = 0.5f; s.fall_s = 0.5f;
        dir.start("blush", s);

        const float dt = 1.f / 60.f;
        for (int i = 0; i < 60; ++i) dir.tick(dt, leds);   // 1s: past full rise
        expect(leds.zone_overlay(Zone::LeftCheekhub).amount > 0.99f,
               "rises to full and holds");
        expect(leds.zone_overlay(Zone::LeftCheekhub).active, "zone 0 is covered");
        expect(!leds.zone_overlay(Zone::LeftFin).active,
               "a zone outside the mask is NOT touched");

        for (int i = 0; i < 120; ++i) dir.tick(dt, leds);  // 2s more, still held
        expect(leds.zone_overlay(Zone::LeftCheekhub).amount > 0.99f,
               "holds indefinitely while the expression is active");

        dir.release("blush");
        for (int i = 0; i < 60; ++i) dir.tick(dt, leds);   // 1s: past full retract
        expect(!leds.zone_overlay(Zone::LeftCheekhub).active,
               "retracts and clears the zone");
        expect(!dir.any_active(), "the director drops the finished overlay");

        // Once: self-terminates even while held.
        s.mode = OverlayMode::Once;
        dir.start("pulse", s);
        for (int i = 0; i < 20; ++i) dir.tick(dt, leds);
        expect(dir.any_active(), "Once is still running mid-pass");
        for (int i = 0; i < 120; ++i) dir.tick(dt, leds);
        expect(!dir.any_active(), "Once finishes on its own without a release");

        // Cycle: keeps going while held.
        s.mode = OverlayMode::Cycle;
        dir.start("throb", s);
        for (int i = 0; i < 300; ++i) dir.tick(dt, leds);  // 5s
        expect(dir.any_active(), "Cycle keeps running while held");
        dir.release("throb");
        for (int i = 0; i < 90; ++i) dir.tick(dt, leds);
        expect(!dir.any_active(), "Cycle still stops on release");
    }

    std::printf("\n%s\n", fail ? "*** SOME CHECKS FAILED ***" : "ALL CHECKS PASSED");
    return fail ? 1 : 0;
}

#pragma once
// ── led_overlay.h ────────────────────────────────────────────────────────────
// Drives the accessory-LED overlay layer over time: a colour that rises across
// part of a zone while the zone's own pattern keeps running underneath, then
// retracts. Used by face expressions ("blushing" → pink rising up the cheeks).
//
// WHY THIS EXISTS SEPARATELY from the expression actions: every other action is
// apply-once / revert-once, but an overlay has to ANIMATE for as long as the
// expression is held. So the expression's apply() starts a rise here, its
// revert() asks for a retract, and tick() carries it between those points.
//
// The host owns the timing even in coproc_local mode, and streams the two
// animated scalars (amount/phase) to the Pico at the existing command rate —
// the same shape as the LVOL mic feed that already drives the audio-reactive
// patterns. That keeps ONE state machine rather than duplicating this one in
// firmware, where it would be a third thing to keep in parity.

#include <algorithm>
#include <cstdint>
#include <string>

#include "accessory/accessory_leds.h"

namespace accessory {

enum class OverlayMode : uint8_t {
    RiseHold = 0,   // rise, hold while the expression is held, retract at the end
    Cycle    = 1,   // rise and fall repeatedly for as long as it's held
    Once     = 2,   // rise then retract immediately, even if still held
};

// What an expression asks for. Everything the look needs, plus the timing.
struct OverlaySpec {
    uint32_t     zone_mask = 0x1F;              // bit per zone; default all five
    uint8_t      r = 255, g = 105, b = 180;
    OverlayShape shape     = OverlayShape::Rise;
    OverlayMode  mode      = OverlayMode::RiseHold;
    float        opacity   = 0.85f;
    float        angle     = 90.f;
    float        softness  = 0.18f;
    float        rise_s    = 0.6f;              // 0..1 travel time
    float        fall_s    = 0.9f;              // 1..0 travel time
    float        cycle_hz  = 0.5f;              // Cycle rate, and the Sweep's travel
};

// One running overlay. Multiple can run at once as long as they cover different
// zones; a later one claiming the same zone simply takes it over.
class OverlayDirector {
public:
    // Start (or restart) the overlay for `key` — the expression that owns it.
    // Restarting an already-running key keeps its current amount, so a re-trigger
    // doesn't visibly snap back to zero.
    void start(const std::string& key, const OverlaySpec& spec) {
        Item* it = find(key);
        if (!it) { items_.push_back(Item{}); it = &items_.back(); it->key = key; }
        const float keep = it->live ? it->amount : 0.f;
        it->spec     = spec;
        it->live     = true;
        it->releasing = false;
        it->amount   = keep;
        it->done     = false;
    }

    // The expression ended: retract, then drop out once it has faded.
    void release(const std::string& key) {
        if (Item* it = find(key)) it->releasing = true;
    }

    // Advance every running overlay and push the result to the strip.
    void tick(float dt, AccessoryLeds& leds) {
        if (items_.empty()) return;
        uint32_t touched = 0;
        for (auto& it : items_) {
            if (!it.live) continue;
            const OverlaySpec& s = it.spec;
            // Sweep/Bloom travel continuously while the overlay exists.
            it.phase += dt * std::max(0.f, s.cycle_hz);
            it.phase -= std::floor(it.phase);

            const float up   = (s.rise_s > 0.01f) ? dt / s.rise_s : 1.f;
            const float down = (s.fall_s > 0.01f) ? dt / s.fall_s : 1.f;
            if (it.releasing) {
                it.amount -= down;
            } else {
                switch (s.mode) {
                case OverlayMode::RiseHold:
                    it.amount = std::min(1.f, it.amount + up);
                    break;
                case OverlayMode::Cycle:
                    // Ping-pong between the ends for as long as it's held.
                    it.amount += it.rising ? up : -down;
                    if (it.amount >= 1.f) { it.amount = 1.f; it.rising = false; }
                    if (it.amount <= 0.f) { it.amount = 0.f; it.rising = true;  }
                    break;
                case OverlayMode::Once:
                    // One pass: up to full, then straight back down and finish,
                    // regardless of how long the expression is held.
                    if (!it.peaked) {
                        it.amount = std::min(1.f, it.amount + up);
                        if (it.amount >= 1.f) it.peaked = true;
                    } else {
                        it.amount -= down;
                    }
                    break;
                }
            }
            if (it.amount <= 0.f &&
                (it.releasing || (s.mode == OverlayMode::Once && it.peaked))) {
                it.amount = 0.f;
                it.live   = false;
                it.done   = true;
                // Fall through to push one last zeroed frame so the zones clear.
            }
            touched |= s.zone_mask;
            push(leds, it);
        }
        // Drop finished entries once their clearing frame has gone out.
        items_.erase(std::remove_if(items_.begin(), items_.end(),
                                    [](const Item& i){ return i.done && !i.live; }),
                     items_.end());
        (void)touched;
    }

    bool any_active() const {
        for (const auto& i : items_) if (i.live) return true;
        return false;
    }

private:
    struct Item {
        std::string key;
        OverlaySpec spec;
        float       amount = 0.f;
        float       phase  = 0.f;
        bool        live   = false;
        bool        releasing = false;
        bool        rising = true;     // Cycle direction
        bool        peaked = false;    // Once: reached the top
        bool        done   = false;
    };

    Item* find(const std::string& key) {
        for (auto& i : items_) if (i.key == key) return &i;
        return nullptr;
    }

    static void push(AccessoryLeds& leds, const Item& it) {
        for (int zi = 0; zi < ZoneCount; ++zi) {
            if (!(it.spec.zone_mask & (1u << zi))) continue;
            const Zone z = static_cast<Zone>(zi);
            if (!it.live) { leds.clear_zone_overlay(z); continue; }
            ZoneOverlay ov;
            ov.active   = true;
            ov.r = it.spec.r; ov.g = it.spec.g; ov.b = it.spec.b;
            ov.shape    = it.spec.shape;
            ov.opacity  = it.spec.opacity;
            ov.angle    = it.spec.angle;
            ov.softness = it.spec.softness;
            ov.amount   = std::clamp(it.amount, 0.f, 1.f);
            ov.phase    = it.phase;
            leds.set_zone_overlay(z, ov);
        }
    }

    std::vector<Item> items_;
};

}  // namespace accessory

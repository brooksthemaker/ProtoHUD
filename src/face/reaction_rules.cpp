#include "reaction_rules.h"

#include <algorithm>
#include <cmath>

namespace face {

// ── Serialization ─────────────────────────────────────────────────────────────

nlohmann::json ReactionOutcome::to_json() const {
    nlohmann::json j;
    j["kind"] = (kind == Kind::Gif) ? "gif" : "expression";
    if (kind == Kind::Gif) j["gif_slot"] = gif_slot;
    else                   j["expression"] = expression;
    return j;
}
ReactionOutcome ReactionOutcome::from_json(const nlohmann::json& j) {
    ReactionOutcome o;
    if (!j.is_object()) return o;
    if (j.value("kind", "expression") == "gif") {
        o.kind = Kind::Gif;
        o.gif_slot = j.value("gif_slot", 0);
    } else {
        o.kind = Kind::Expression;
        o.expression = j.value("expression", o.expression);
    }
    return o;
}

nlohmann::json ReactionRule::to_json() const {
    return {{"enabled", enabled}, {"threshold", threshold},
            {"hold_s", hold_s}, {"outcome", outcome.to_json()}};
}
ReactionRule ReactionRule::from_json(const nlohmann::json& j) {
    ReactionRule r;
    if (!j.is_object()) return r;
    r.enabled   = j.value("enabled", r.enabled);
    r.threshold = j.value("threshold", r.threshold);
    r.hold_s    = j.value("hold_s", r.hold_s);
    if (j.contains("outcome")) r.outcome = ReactionOutcome::from_json(j["outcome"]);
    return r;
}

// ── Built-in reactions ────────────────────────────────────────────────────────

const std::vector<ReactionRules::Meta>& ReactionRules::metas() {
    static const std::vector<Meta> kMetas = {
        { "move_bright", "Move into Bright",
          "Fires when the ambient light rises past the threshold — stepping "
          "into sunlight or a lit room.",
          800.f, 50.f, 3000.f, 50.f, " lux", "squint" },
        { "move_dark", "Move into Dark",
          "Fires when the ambient light falls past the threshold — stepping "
          "into shade or a dark room.",
          40.f, 0.f, 1000.f, 10.f, " lux", "surprised" },
        { "dizzy", "Dizzy",
          "Fires after enough fast spinning (accumulated head/turn rotation) "
          "— spin in a circle or whip your head around.",
          540.f, 180.f, 1440.f, 30.f, "\xc2\xb0", "surprised" },
        { "low_battery", "Low Battery",
          "Fires when the controller battery drops below this percentage.",
          15.f, 5.f, 50.f, 1.f, "%", "sad" },
        { "loud_noise", "Loud Noise",
          "Fires when the mic level spikes past the threshold — a shout or a "
          "bang.",
          0.7f, 0.1f, 1.0f, 0.05f, "", "surprised" },
        { "upside_down", "Upside-Down",
          "Fires when the head rolls past this angle — helmet tipped way over "
          "or inverted.",
          150.f, 60.f, 180.f, 5.f, "\xc2\xb0", "surprised" },
    };
    return kMetas;
}

const ReactionRules::Meta* ReactionRules::meta(const std::string& id) {
    for (const auto& m : metas()) if (id == m.id) return &m;
    return nullptr;
}

// ── Rule access ───────────────────────────────────────────────────────────────

ReactionRule& ReactionRules::rule_locked(const std::string& id) {
    auto it = rules_.find(id);
    if (it != rules_.end()) return it->second;
    ReactionRule r;
    if (const Meta* m = meta(id)) {
        r.threshold = m->def_threshold;
        r.outcome.expression = m->def_expression;
    }
    return rules_.emplace(id, r).first->second;
}

ReactionRule ReactionRules::rule(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = rules_.find(id);
    if (it != rules_.end()) return it->second;
    ReactionRule r;
    if (const Meta* m = meta(id)) {
        r.threshold = m->def_threshold;
        r.outcome.expression = m->def_expression;
    }
    return r;
}

void ReactionRules::set_rule(const std::string& id, const ReactionRule& r) {
    std::lock_guard<std::mutex> lk(mtx_);
    rules_[id] = r;
}

// ── Firing ────────────────────────────────────────────────────────────────────

void ReactionRules::fire_locked(const std::string& id, const ReactionRule& r) {
    if (r.outcome.kind == ReactionOutcome::Kind::Gif) {
        // GIFs take over the face and auto-revert on their own timer — no
        // hold bookkeeping here.
        if (act_.play_gif) act_.play_gif(r.outcome.gif_slot);
        if (act_.notify) act_.notify("Reaction", id);
        return;
    }
    // Expression outcome: remember the current face (unless we're already
    // showing a reaction), switch, and hold.
    if (!(active_ && active_id_ == id)) {
        if (!active_ && act_.current_face) restore_face_ = act_.current_face();
        active_ = true;
        active_id_ = id;
        if (act_.set_face && !r.outcome.expression.empty())
            act_.set_face(r.outcome.expression);
        if (act_.notify) act_.notify("Reaction", id);
    }
    latched_   = (r.hold_s <= 0.0);
    hold_left_ = r.hold_s;
}

void ReactionRules::deactivate() {
    std::lock_guard<std::mutex> lk(mtx_);
    deactivate_locked();
}
void ReactionRules::deactivate_locked() {
    if (!active_) return;
    active_ = false;
    active_id_.clear();
    latched_ = false;
    hold_left_ = 0.0;
    if (!restore_face_.empty() && act_.set_face) act_.set_face(restore_face_);
    restore_face_.clear();
}

void ReactionRules::test(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    fire_locked(id, rule_locked(id));
}

bool ReactionRules::active() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_;
}
std::string ReactionRules::active_id() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_id_;
}

// ── Signals + evaluation ──────────────────────────────────────────────────────

void ReactionRules::set_signals(const Signals& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    sig_ = s;
}

void ReactionRules::tick(double dt) {
    std::lock_guard<std::mutex> lk(mtx_);
    clock_s_ += std::max(0.0, dt);

    auto edge_of = [this](const std::string& id) -> EdgeState& { return edge_[id]; };

    // Light edges — ±10% hysteresis; first reading only arms.
    auto light_edge = [&](const std::string& id, bool rising) {
        auto rit = rules_.find(id);
        const ReactionRule& r = (rit != rules_.end()) ? rit->second : rule_locked(id);
        if (!r.enabled || sig_.lux < 0.f) return;
        EdgeState& e = edge_of(id);
        const float band = std::max(1.f, r.threshold * 0.10f);
        int now_side = e.side;
        if      (sig_.lux > r.threshold + band) now_side = +1;
        else if (sig_.lux < r.threshold - band) now_side = -1;
        if (now_side == e.side) return;
        const int prev = e.side;
        e.side = now_side;
        if (prev == 0) return;
        if (( rising && now_side == +1) || (!rising && now_side == -1))
            fire_locked(id, r);
    };
    light_edge("move_bright", true);
    light_edge("move_dark",   false);

    // Dizzy — integrate spin with decay; fire when the accumulator crosses the
    // threshold (degrees of accumulated fast rotation), then reset + cooldown.
    {
        auto rit = rules_.find("dizzy");
        const ReactionRule& r = (rit != rules_.end()) ? rit->second : rule_locked("dizzy");
        EdgeState& e = edge_of("dizzy");
        if (e.cooldown > 0.0) e.cooldown -= dt;
        if (r.enabled) {
            // Only fast rotation counts (ignore slow looking-around < 60 dps).
            const double contrib = (std::fabs(sig_.spin_dps) > 60.0)
                                   ? std::fabs(sig_.spin_dps) * dt : 0.0;
            e.spin_accum = std::max(0.0, e.spin_accum + contrib - 90.0 * dt);  // decay
            if (e.cooldown <= 0.0 && e.spin_accum >= r.threshold) {
                e.spin_accum = 0.0;
                e.cooldown   = 2.0;
                fire_locked("dizzy", r);
            }
        } else {
            e.spin_accum = 0.0;
        }
    }

    // Threshold-crossing reactions (below → fire on entering the "trip" side,
    // re-arm on leaving it): low battery, loud noise, upside-down.
    auto cross = [&](const std::string& id, double value, bool below, bool valid) {
        auto rit = rules_.find(id);
        const ReactionRule& r = (rit != rules_.end()) ? rit->second : rule_locked(id);
        if (!r.enabled || !valid) return;
        EdgeState& e = edge_of(id);
        const double margin = std::max(1.0, r.threshold * 0.06);
        bool tripped = below ? (value < r.threshold)
                             : (value > r.threshold);
        bool cleared = below ? (value > r.threshold + margin)
                             : (value < r.threshold - margin);
        if (tripped && e.side <= 0) { e.side = 1; fire_locked(id, r); }
        else if (cleared && e.side > 0) { e.side = 0; }
    };
    cross("low_battery", sig_.battery_pct, /*below=*/true,  sig_.battery_pct >= 0);
    cross("loud_noise",  sig_.mic,         /*below=*/false, true);
    cross("upside_down", std::fabs(sig_.roll_deg), /*below=*/false, true);

    // Hold expiry for the active expression outcome.
    if (active_ && !latched_) {
        hold_left_ -= dt;
        if (hold_left_ <= 0.0) deactivate_locked();
    }
}

// ── Config ────────────────────────────────────────────────────────────────────

nlohmann::json ReactionRules::to_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [id, r] : rules_) j[id] = r.to_json();
    return j;
}

void ReactionRules::load_json(const nlohmann::json& j) {
    if (!j.is_object()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = j.begin(); it != j.end(); ++it)
        rules_[it.key()] = ReactionRule::from_json(it.value());
}

}  // namespace face

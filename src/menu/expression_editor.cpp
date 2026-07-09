#include "expression_editor.h"

#include <algorithm>
#include <cstdio>

#include "../face/glitch.h"

namespace menu {

namespace {

ImU32 ee_with_alpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24);
}

// Parse "solid:r,g,b" → true + components (for the material swatch).
bool ee_parse_solid(const std::string& spec, int& r, int& g, int& b) {
    return std::sscanf(spec.c_str(), "solid:%d,%d,%d", &r, &g, &b) == 3;
}

const char* kBoopLabels[]    = { "Off", "Snout", "Left Cheek", "Right Cheek", "Both Cheeks" };
const char* kGestureLabels[] = { "Off", "Swipe Up", "Swipe Down", "Swipe Left", "Swipe Right" };
const char* kGestureVals[]   = { "",    "up",       "down",       "left",       "right" };
const char* kMotionLabels[]  = { "Off", "Shake (wake spike)", "Still (drowsy)" };
const char* kMotionVals[]    = { "",    "shake",              "still" };
const char* kLightLabels[]   = { "Off", "Bright (above)", "Dark (below)" };
const char* kLightVals[]     = { "",    "bright",         "dark" };

} // namespace

void ExpressionEditor::open(std::string title, Mode mode,
                            face::CustomExpression initial,
                            std::vector<std::pair<std::string, std::string>> base_faces,
                            std::vector<std::pair<std::string, std::string>> materials,
                            std::vector<std::pair<std::string, nlohmann::json>> effects,
                            CommitFn on_commit, CancelFn on_cancel,
                            PreviewFn on_preview, OpenKbFn open_kb) {
    title_      = std::move(title);
    mode_       = mode;
    initial_    = initial;
    value_      = std::move(initial);
    base_faces_ = std::move(base_faces);
    materials_  = std::move(materials);
    effects_    = std::move(effects);
    on_commit_  = std::move(on_commit);
    on_cancel_  = std::move(on_cancel);
    on_preview_ = std::move(on_preview);
    open_kb_    = std::move(open_kb);

    // Seed field state from the initial value.
    base_idx_ = 0;
    for (size_t i = 0; i < base_faces_.size(); ++i)
        if (base_faces_[i].first == value_.base_expression) { base_idx_ = (int)i; break; }

    material_idx_ = 0;
    custom_material_.clear();
    if (!value_.style.material_spec.empty()) {
        material_idx_ = -1;
        custom_material_ = value_.style.material_spec;
        for (size_t i = 0; i < materials_.size(); ++i)
            if (materials_[i].second == value_.style.material_spec) {
                material_idx_ = (int)i; custom_material_.clear(); break;
            }
    }

    effect_idx_ = 0;
    if (!value_.style.effect_spec.is_null())
        for (size_t i = 0; i < effects_.size(); ++i)
            if (effects_[i].second == value_.style.effect_spec) { effect_idx_ = (int)i; break; }

    // Glitch options: 0 = inherit, 1 = off, 2.. = presets().
    glitch_idx_ = 0;
    if (value_.style.has_glitch) {
        glitch_idx_ = 1;   // explicit off unless a preset matches
        const auto& presets = face::GlitchConfig::presets();
        if (value_.style.glitch.enabled)
            for (size_t i = 0; i < presets.size(); ++i)
                if (presets[i].second.to_json() == value_.style.glitch.to_json()) {
                    glitch_idx_ = (int)i + 2; break;
                }
        if (glitch_idx_ == 1 && value_.style.glitch.enabled)
            glitch_idx_ = 2;   // enabled but unmatched: closest is the first preset
    }

    hold_s_ = value_.hold_s;
    boop_idx_ = gesture_idx_ = motion_idx_ = light_idx_ = 0;
    light_lux_ = 800.f;
    if (const auto* t = value_.trigger(face::CustomTrigger::Type::Boop))
        boop_idx_ = std::clamp(t->boop_zone + 1, 0, 4);
    if (const auto* t = value_.trigger(face::CustomTrigger::Type::Gesture))
        for (int i = 1; i < 5; ++i) if (t->gesture == kGestureVals[i]) gesture_idx_ = i;
    if (const auto* t = value_.trigger(face::CustomTrigger::Type::Motion))
        for (int i = 1; i < 3; ++i) if (t->motion_event == kMotionVals[i]) motion_idx_ = i;
    if (const auto* t = value_.trigger(face::CustomTrigger::Type::Light)) {
        for (int i = 1; i < 3; ++i) if (t->light_edge == kLightVals[i]) light_idx_ = i;
        light_lux_ = t->light_lux;
    }

    focus_ = (mode_ == Mode::Custom) ? RowName : RowMaterial;
    armed_ = false;
    open_  = true;
    emit_detents();
}

void ExpressionEditor::close() {
    open_ = false;
    on_commit_ = nullptr; on_cancel_ = nullptr;
    on_preview_ = nullptr; open_kb_ = nullptr;
}

bool ExpressionEditor::row_skipped(int row) const {
    if (mode_ == Mode::StyleOnly)
        switch (row) {
            case RowName: case RowBaseFace: case RowHold:
            case RowTrigBoop: case RowTrigGesture: case RowTrigMotion:
            case RowTrigLight: case RowTrigLightLux:
                return true;
            default: break;
        }
    if (row == RowTrigLightLux && light_idx_ == 0) return true;
    return false;
}

void ExpressionEditor::rebuild_value() {
    value_.base_expression = base_faces_.empty()
        ? value_.base_expression
        : base_faces_[std::clamp(base_idx_, 0, (int)base_faces_.size() - 1)].first;

    face::ExpressionStyle st;
    if (material_idx_ == -1) st.material_spec = custom_material_;
    else if (!materials_.empty())
        st.material_spec =
            materials_[std::clamp(material_idx_, 0, (int)materials_.size() - 1)].second;
    if (!effects_.empty())
        st.effect_spec =
            effects_[std::clamp(effect_idx_, 0, (int)effects_.size() - 1)].second;
    if (glitch_idx_ == 1) {
        st.has_glitch = true;             // explicit "no glitch" for this expression
        st.glitch = face::GlitchConfig{};
        st.glitch.enabled = false;
    } else if (glitch_idx_ >= 2) {
        const auto& presets = face::GlitchConfig::presets();
        const int pi = std::clamp(glitch_idx_ - 2, 0, (int)presets.size() - 1);
        st.has_glitch = true;
        st.glitch = presets[pi].second;
    }
    value_.style = std::move(st);

    value_.hold_s = hold_s_;
    value_.triggers.clear();
    if (mode_ == Mode::Custom) {
        if (boop_idx_ > 0) {
            face::CustomTrigger t; t.type = face::CustomTrigger::Type::Boop;
            t.boop_zone = boop_idx_ - 1; value_.triggers.push_back(t);
        }
        if (gesture_idx_ > 0) {
            face::CustomTrigger t; t.type = face::CustomTrigger::Type::Gesture;
            t.gesture = kGestureVals[gesture_idx_]; value_.triggers.push_back(t);
        }
        if (motion_idx_ > 0) {
            face::CustomTrigger t; t.type = face::CustomTrigger::Type::Motion;
            t.motion_event = kMotionVals[motion_idx_]; value_.triggers.push_back(t);
        }
        if (light_idx_ > 0) {
            face::CustomTrigger t; t.type = face::CustomTrigger::Type::Light;
            t.light_edge = kLightVals[light_idx_]; t.light_lux = light_lux_;
            value_.triggers.push_back(t);
        }
        // Manual activation always stays available from the menu row.
        face::CustomTrigger t; t.type = face::CustomTrigger::Type::Manual;
        value_.triggers.push_back(t);
    }
}

void ExpressionEditor::apply_preview() {
    rebuild_value();
    if (on_preview_) on_preview_(value_);
}

void ExpressionEditor::emit_detents() {
    if (!detent_cb_) return;
    int n = 0;
    for (int r = 0; r < RowCount; ++r) if (!row_skipped(r)) ++n;
    if (armed_) {
        switch (focus_) {
            case RowBaseFace:    n = std::max(1, (int)base_faces_.size()); break;
            case RowMaterial:    n = std::max(1, (int)materials_.size()); break;
            case RowEffect:      n = std::max(1, (int)effects_.size()); break;
            case RowGlitch:      n = 2 + (int)face::GlitchConfig::presets().size(); break;
            case RowHold:        n = 21; break;   // 0..10 s in 0.5 steps
            case RowTrigBoop:    n = 5;  break;
            case RowTrigGesture: n = 5;  break;
            case RowTrigMotion:  n = 3;  break;
            case RowTrigLight:   n = 3;  break;
            case RowTrigLightLux:n = 41; break;   // 0..2000 in 50 steps
            default: break;
        }
    }
    detent_cb_(n);
}

void ExpressionEditor::adjust(int d) {
    auto cyc = [](int v, int d2, int n) { return n <= 0 ? 0 : ((v + d2) % n + n) % n; };
    switch (focus_) {
        case RowBaseFace: base_idx_ = cyc(base_idx_, d, (int)base_faces_.size()); break;
        case RowMaterial: {
            // Cycling leaves a custom typed spec: walk back into the preset list.
            int n = (int)materials_.size();
            if (material_idx_ == -1) material_idx_ = (d >= 0) ? 0 : n - 1;
            else                     material_idx_ = cyc(material_idx_, d, n);
            custom_material_.clear();
            break;
        }
        case RowEffect:      effect_idx_  = cyc(effect_idx_, d, (int)effects_.size()); break;
        case RowGlitch:
            glitch_idx_ = cyc(glitch_idx_, d, 2 + (int)face::GlitchConfig::presets().size());
            break;
        case RowHold:
            hold_s_ = std::clamp(hold_s_ + 0.5 * d, 0.0, 10.0);
            break;
        case RowTrigBoop:    boop_idx_    = cyc(boop_idx_, d, 5); break;
        case RowTrigGesture: gesture_idx_ = cyc(gesture_idx_, d, 5); break;
        case RowTrigMotion:  motion_idx_  = cyc(motion_idx_, d, 3); break;
        case RowTrigLight:   light_idx_   = cyc(light_idx_, d, 3); emit_detents(); break;
        case RowTrigLightLux:
            light_lux_ = std::clamp(light_lux_ + 50.f * d, 0.f, 2000.f);
            break;
        default: return;
    }
    apply_preview();
}

void ExpressionEditor::step(int d) {
    if (!open_ || d == 0) return;
    if (armed_) { adjust(d); return; }
    const int dir = d > 0 ? 1 : -1;
    for (int i = 0; i != d; i += dir) {
        int next = focus_;
        do { next = ((next + dir) % RowCount + RowCount) % RowCount; }
        while (row_skipped(next) && next != focus_);
        focus_ = next;
    }
}

void ExpressionEditor::move(int dx, int dy) {
    if (!open_) return;
    if (dx != 0) adjust(dx);
    if (dy != 0) {
        if (armed_) adjust(-dy);   // up = increase while armed, like sliders
        else        step(dy);
    }
}

void ExpressionEditor::activate() {
    if (!open_) return;
    switch (focus_) {
        case RowName:        open_name_entry();  return;
        case RowCustomColor: open_color_entry(); return;
        case RowSave:        commit();           return;
        case RowCancel:      cancel();           return;
        default:
            armed_ = !armed_;
            emit_detents();
            return;
    }
}

void ExpressionEditor::back() {
    if (!open_) return;
    if (armed_) { armed_ = false; emit_detents(); return; }
    cancel();
}

void ExpressionEditor::open_name_entry() {
    if (!open_kb_) return;
    open_kb_("Expression Name", value_.name, [this](const std::string& s) {
        if (!open_) return;
        value_.name = s;
        apply_preview();
    });
}

void ExpressionEditor::open_color_entry() {
    if (!open_kb_) return;
    std::string cur;
    int r, g, b;
    if (material_idx_ == -1 && ee_parse_solid(custom_material_, r, g, b)) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%d,%d,%d", r, g, b);
        cur = buf;
    }
    open_kb_("Solid Color: R,G,B or hex RRGGBB", cur, [this](const std::string& s) {
        if (!open_) return;
        int r2 = 0, g2 = 0, b2 = 0;
        bool ok = false;
        if (std::sscanf(s.c_str(), "%d,%d,%d", &r2, &g2, &b2) == 3) {
            ok = true;
        } else {
            unsigned v = 0;
            if (std::sscanf(s.c_str(), "%x", &v) == 1 && s.size() >= 3) {
                r2 = (v >> 16) & 0xFF; g2 = (v >> 8) & 0xFF; b2 = v & 0xFF;
                ok = true;
            }
        }
        if (!ok) return;
        char spec[32];
        std::snprintf(spec, sizeof spec, "solid:%d,%d,%d",
                      std::clamp(r2, 0, 255), std::clamp(g2, 0, 255),
                      std::clamp(b2, 0, 255));
        material_idx_ = -1;
        custom_material_ = spec;
        apply_preview();
    });
}

void ExpressionEditor::commit() {
    rebuild_value();
    if (mode_ == Mode::Custom && value_.name.empty()) {
        // A custom expression needs a name — bounce to the name row instead
        // of silently saving an unnamed slot.
        focus_ = RowName;
        armed_ = false;
        open_name_entry();
        return;
    }
    value_.used = true;
    CommitFn cb = on_commit_;
    face::CustomExpression out = value_;
    close();
    if (cb) cb(out);
}

void ExpressionEditor::cancel() {
    CancelFn cb = on_cancel_;
    close();
    if (cb) cb();
}

std::string ExpressionEditor::material_label() const {
    if (material_idx_ == -1) {
        int r, g, b;
        if (ee_parse_solid(custom_material_, r, g, b)) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "Custom %d,%d,%d", r, g, b);
            return buf;
        }
        return "Custom";
    }
    if (materials_.empty()) return "Default (inherit)";
    return materials_[std::clamp(material_idx_, 0, (int)materials_.size() - 1)].first;
}

std::string ExpressionEditor::glitch_label() const {
    if (glitch_idx_ == 0) return "Default (inherit)";
    if (glitch_idx_ == 1) return "Off";
    const auto& presets = face::GlitchConfig::presets();
    const int pi = std::clamp(glitch_idx_ - 2, 0, (int)presets.size() - 1);
    return presets[pi].first;
}

void ExpressionEditor::draw(ImDrawList* dl, ImFont* font, float fs,
                            float W, float H, ImU32 accent) {
    if (!open_) return;

    // Dim + panel chrome (same look as the other overlays).
    dl->AddRectFilled({ 0.f, 0.f }, { W, H }, IM_COL32(4, 8, 12, 205));
    const float pw = std::min(W * 0.86f, 820.f);
    const float ph = std::min(H * 0.84f, 600.f);
    const ImVec2 pmin{ (W - pw) * 0.5f, (H - ph) * 0.5f };
    const ImVec2 pmax{ pmin.x + pw, pmin.y + ph };
    dl->AddRectFilled(pmin, pmax, IM_COL32(8, 12, 16, 235));
    dl->AddRect      (pmin, pmax, ee_with_alpha(accent, 220), 0.f, 0, 2.f);

    const float pad = 24.f;
    const float x0  = pmin.x + pad;
    const float x1  = pmax.x - pad;

    dl->AddText(font, fs * 1.4f, { x0, pmin.y + 12.f },
                IM_COL32(255, 255, 255, 255), title_.c_str());

    const float top   = pmin.y + 12.f + fs * 1.4f + 14.f;
    const float row_h = fs * 1.55f;
    const ImU32 col_dim = IM_COL32(150, 160, 170, 200);
    const ImU32 col_val = IM_COL32(235, 240, 245, 235);

    char buf[64];
    auto row_value = [&](int r) -> std::string {
        switch (r) {
            case RowName:     return value_.name.empty() ? "(unnamed)" : value_.name;
            case RowBaseFace:
                return base_faces_.empty() ? value_.base_expression
                    : base_faces_[std::clamp(base_idx_, 0, (int)base_faces_.size() - 1)].second;
            case RowMaterial: return material_label();
            case RowCustomColor: return "Enter R,G,B / hex...";
            case RowEffect:
                return effects_.empty() ? "Default (inherit)"
                    : effects_[std::clamp(effect_idx_, 0, (int)effects_.size() - 1)].first;
            case RowGlitch:   return glitch_label();
            case RowHold:
                if (hold_s_ <= 0.0) return "Latch (manual return)";
                std::snprintf(buf, sizeof buf, "%.1f s", hold_s_);
                return buf;
            case RowTrigBoop:    return kBoopLabels[std::clamp(boop_idx_, 0, 4)];
            case RowTrigGesture: return kGestureLabels[std::clamp(gesture_idx_, 0, 4)];
            case RowTrigMotion:  return kMotionLabels[std::clamp(motion_idx_, 0, 2)];
            case RowTrigLight:   return kLightLabels[std::clamp(light_idx_, 0, 2)];
            case RowTrigLightLux:
                std::snprintf(buf, sizeof buf, "%.0f lux", light_lux_);
                return buf;
            case RowSave:   return "";
            case RowCancel: return "";
        }
        return "";
    };
    static const char* kLabels[RowCount] = {
        "Name", "Base Face", "Material", "Custom Color", "Effect", "Glitch",
        "Hold Time", "Trigger: Boop", "Trigger: Gesture", "Trigger: Motion",
        "Trigger: Light", "Light Threshold", "SAVE", "CANCEL",
    };

    float y = top;
    for (int r = 0; r < RowCount; ++r) {
        if (row_skipped(r)) continue;
        const bool sel      = (focus_ == r);
        const bool is_save  = (r == RowSave);
        const bool is_cxl   = (r == RowCancel);
        const bool headerish = (r == RowTrigBoop && mode_ == Mode::Custom);
        if (headerish) {
            dl->AddText(font, fs * 0.85f, { x0, y },
                        ee_with_alpha(accent, 170), "TRIGGERS");
            y += fs * 0.85f + 6.f;
        }
        ImVec2 rmin{ x0 - 8.f, y - 3.f }, rmax{ x1 + 8.f, y + row_h - 5.f };
        if (sel)
            dl->AddRectFilled(rmin, rmax,
                              armed_ ? ee_with_alpha(accent, 90)
                                     : ee_with_alpha(accent, 45), 3.f);
        ImU32 base = is_save ? IM_COL32(90, 220, 130, sel ? 255 : 190)
                   : is_cxl  ? IM_COL32(255, 120, 120, sel ? 255 : 190)
                   : sel     ? IM_COL32(255, 255, 255, 255) : col_dim;
        dl->AddText(font, fs, { x0, y }, base, kLabels[r]);

        const std::string val = row_value(r);
        if (!val.empty()) {
            ImVec2 vsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, val.c_str());
            dl->AddText(font, fs, { x1 - vsz.x, y }, sel ? col_val : col_dim,
                        val.c_str());
            // Material swatch for solid colors.
            if (r == RowMaterial) {
                int cr, cg, cb;
                const std::string& spec = (material_idx_ == -1)
                    ? custom_material_
                    : (materials_.empty() ? std::string()
                       : materials_[std::clamp(material_idx_, 0,
                                               (int)materials_.size() - 1)].second);
                if (ee_parse_solid(spec, cr, cg, cb)) {
                    const float sw = fs * 0.9f;
                    dl->AddRectFilled({ x1 - vsz.x - sw - 10.f, y + 1.f },
                                      { x1 - vsz.x - 10.f, y + 1.f + sw },
                                      IM_COL32(cr, cg, cb, 255), 2.f);
                }
            }
        }
        y += row_h;
    }

    // Hint bar.
    dl->AddText(font, fs * 0.9f, { x0, pmax.y - fs - 10.f },
                ee_with_alpha(accent, 185),
                "UP/DOWN ROW   \xC2\xB7   A/ENTER ARM \xE2\x80\xA2 EDIT   \xC2\xB7   "
                "LEFT/RIGHT ADJUST   \xC2\xB7   B/ESC BACK");
}

} // namespace menu

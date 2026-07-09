#pragma once
// ── expression_editor.h ───────────────────────────────────────────────────────
// Full-screen expression editor overlay — the reusable module behind both
// "Style..." on a built-in expression slot (Mode::StyleOnly: material /
// effect / glitch only) and editing a custom expression (Mode::Custom: name,
// base face, style, hold time, and trigger options). Same overlay pattern as
// the color picker: MenuSystem routes step/move/activate/back while open,
// draw() takes the whole frame, edits preview live, and Back at the top
// level cancels (the caller reverts the preview).
//
// Option vocabularies (base faces, material presets, effect presets) are
// supplied by the caller so the editor stays decoupled from the menu builder
// tables. Typed entry (name, custom solid color) goes through the
// caller-supplied open_kb hook — MenuSystem wires its on-screen keyboard,
// which stacks on top of overlays.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "../face/custom_expression.h"
#include "overlay.h"

namespace menu {

class ExpressionEditor : public IOverlay {
public:
    enum class Mode : uint8_t {
        StyleOnly,   // built-in slot: only the style rows are shown
        Custom,      // full custom expression: name / base / style / triggers
    };

    using CommitFn  = std::function<void(const face::CustomExpression&)>;
    using CancelFn  = std::function<void()>;
    // Fired on every edit with the in-progress value (live preview). The
    // caller typically maps it to set_style_override; it is NOT fired on
    // cancel — CancelFn is where the caller reverts the preview.
    using PreviewFn = std::function<void(const face::CustomExpression&)>;
    using OpenKbFn  = std::function<void(std::string, std::string,
                                         std::function<void(const std::string&)>)>;

    // Option lists: base_faces = (expression stem, display label);
    // materials = (display label, material spec) with entry 0 expected to be
    // "Default (inherit)" / spec ""; effects = (display label, particle spec)
    // with entry 0 = inherit (null json).
    void open(std::string title, Mode mode, face::CustomExpression initial,
              std::vector<std::pair<std::string, std::string>> base_faces,
              std::vector<std::pair<std::string, std::string>> materials,
              std::vector<std::pair<std::string, nlohmann::json>> effects,
              CommitFn on_commit, CancelFn on_cancel = {},
              PreviewFn on_preview = {}, OpenKbFn open_kb = {});

    bool is_open() const override { return open_; }
    void close() override;

    void step(int d) override;              // knob: walk rows / adjust armed row
    void move(int dx, int dy) override;     // d-pad: dx adjusts in-row, dy walks
    void activate() override;               // arm/confirm a row, fire kb entry
    void back() override;                   // disarm, else cancel

    void draw(ImDrawList* dl, ImFont* font, float fs, float W, float H,
              ImU32 accent) override;

    bool adjusting() const { return open_ && armed_; }
    void set_detent_callback(std::function<void(int)> cb) { detent_cb_ = std::move(cb); }

private:
    // Focus rows, top to bottom. Mode::StyleOnly (and trigger-dependent rows)
    // skip via row_skipped().
    enum Row : int { RowName, RowBaseFace, RowMaterial, RowCustomColor,
                     RowEffect, RowGlitch, RowHold,
                     RowTrigBoop, RowTrigGesture, RowTrigMotion,
                     RowTrigLight, RowTrigLightLux,
                     RowSave, RowCancel, RowCount };

    bool row_skipped(int row) const;
    void adjust(int d);                      // armed / dx adjustment
    void apply_preview();                    // rebuild value_ + fire on_preview_
    void open_name_entry();
    void open_color_entry();
    void commit();
    void cancel();
    void emit_detents();
    // Current field values → value_ (style + triggers rebuilt from indices).
    void rebuild_value();
    std::string material_label() const;
    std::string glitch_label() const;

    bool open_ = false;
    Mode mode_ = Mode::StyleOnly;
    std::string title_;

    face::CustomExpression value_;           // the edited result
    face::CustomExpression initial_;         // cancel snapshot

    // Option vocabularies.
    std::vector<std::pair<std::string, std::string>>    base_faces_;
    std::vector<std::pair<std::string, std::string>>    materials_;
    std::vector<std::pair<std::string, nlohmann::json>> effects_;

    // Field state (indices into the vocabularies; -1 material = custom spec).
    int    base_idx_     = 0;
    int    material_idx_ = 0;      // -1 = custom spec in custom_material_
    std::string custom_material_;  // e.g. "solid:220,30,30" from typed entry
    int    effect_idx_   = 0;
    int    glitch_idx_   = 0;      // 0 inherit / 1 off / 2.. = preset index-2
    double hold_s_       = 3.0;
    int    boop_idx_     = 0;      // 0 off / 1 snout / 2 left / 3 right / 4 both
    int    gesture_idx_  = 0;      // 0 off / 1 up / 2 down / 3 left / 4 right
    int    motion_idx_   = 0;      // 0 off / 1 shake / 2 still
    int    light_idx_    = 0;      // 0 off / 1 bright / 2 dark
    float  light_lux_    = 800.f;

    int  focus_ = RowName;
    bool armed_ = false;

    CommitFn  on_commit_;
    CancelFn  on_cancel_;
    PreviewFn on_preview_;
    OpenKbFn  open_kb_;
    std::function<void(int)> detent_cb_;
};

} // namespace menu

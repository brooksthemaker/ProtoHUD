#pragma once
// ── color_picker.h ────────────────────────────────────────────────────────────
// Full-screen Figma-style colour picker overlay — the ONE picker every
// COLOR_PICKER menu item opens (face custom colour, palette slots, gradient
// stops, effect layers, LED zones). SV square + hue strip + R/G/B/HEX fields
// + shared history + presets, drivable by knob (step/activate), d-pad
// (move/step/activate) and mouse.
//
// on_change fires on EVERY adjustment (live preview). Confirm keeps the value
// and pushes it onto the shared history; backing out at the top level reverts
// to the colour the picker opened with (same cancel semantics as the old
// in-row channel editor). Typed entry (hex or "r,g,b") goes through the
// caller-supplied open_kb hook — MenuSystem wires its on-screen keyboard.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "overlay.h"

namespace menu {

class ColorPicker : public IOverlay {
public:
    using ChangeFn = std::function<void(uint8_t r, uint8_t g, uint8_t b)>;
    // open_kb(title, initial_text, on_commit) — modal text entry on top of
    // the picker (MenuSystem::open_keyboard).
    using OpenKbFn = std::function<void(std::string, std::string,
                                        std::function<void(const std::string&)>)>;

    void open(std::string title, uint8_t r0, uint8_t g0, uint8_t b0,
              ChangeFn on_change, OpenKbFn open_kb);

    bool is_open() const override { return open_; }
    void close() override;

    // Input — knob walks rows / adjusts an armed field; d-pad dx moves within
    // the SV square / hue strip / swatch rows; activate arms-confirms a field
    // (or fires HEX entry / CONFIRM / CANCEL); back disarms, then cancels.
    void step(int d) override;
    void move(int dx, int dy) override;
    void activate() override;
    void back() override;

    void draw(ImDrawList* dl, ImFont* font, float fs, float W, float H,
              ImU32 accent) override;

    // True while a field is armed (knob adjusts its value) — input handlers
    // use it to flip Up/Down so Up always increases, like slider editing.
    bool adjusting() const { return open_ && armed_; }

    // Suggested knob detent count, fired whenever input granularity changes
    // (row walk vs armed field). MenuSystem wires its detent override here.
    void set_detent_callback(std::function<void(int)> cb) { detent_cb_ = std::move(cb); }

    // Shared, process-wide history of confirmed colours (0xRRGGBB, newest
    // first, deduped, max 12) — every picker instance reads/feeds the same
    // store. get/set exist for config persistence.
    static std::vector<uint32_t> get_history();
    static void set_history(const std::vector<uint32_t>& h);

private:
    // Focus rows, top to bottom.
    enum Row : int { RowSV, RowHue, RowR, RowG, RowB, RowHex,
                     RowHistory, RowPresets, RowConfirm, RowCancel, RowCount };

    void apply();                          // on_change_(r_, g_, b_)
    void set_rgb(int r, int g, int b);     // clamp + resync HSV + apply
    void rgb_from_hsv();                   // h_/s_/v_ → r_/g_/b_ + apply
    void sync_hsv_from_rgb();              // r_/g_/b_ → h_/s_/v_ (hue-sticky)
    void adjust(int d);                    // armed adjustment (+d = increase)
    void use_history(int idx);             // apply history swatch
    void use_preset(int idx);              // apply preset swatch
    void open_typed_entry();               // HEX row → open_kb_
    void confirm();                        // keep value, push history, close
    void cancel();                         // revert to r0/g0/b0, close
    bool row_skipped(int row) const;       // RowHistory hidden when empty
    void emit_detents();
    static void push_history(uint32_t rgb);

    bool        open_  = false;
    std::string title_;
    int         r_ = 255, g_ = 255, b_ = 255;     // current (applied live)
    uint8_t     r0_ = 255, g0_ = 255, b0_ = 255;  // cancel snapshot
    float       h_ = 0.f, s_ = 0.f, v_ = 1.f;     // canonical for SV/hue rows
    int         focus_      = RowSV;
    bool        armed_      = false;
    int         hist_idx_   = 0;
    int         preset_idx_ = 0;

    ChangeFn on_change_;
    OpenKbFn open_kb_;
    std::function<void(int)> detent_cb_;
};

} // namespace menu

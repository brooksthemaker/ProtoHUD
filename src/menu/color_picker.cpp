#include "color_picker.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace menu {

// ── Shared history (process-wide, persisted via get/set) ─────────────────────

static std::vector<uint32_t> s_history;
static constexpr size_t kHistoryMax = 12;

void ColorPicker::push_history(uint32_t rgb) {
    auto it = std::find(s_history.begin(), s_history.end(), rgb);
    if (it != s_history.end()) s_history.erase(it);   // dedupe: move to front
    s_history.insert(s_history.begin(), rgb);
    if (s_history.size() > kHistoryMax) s_history.resize(kHistoryMax);
}

std::vector<uint32_t> ColorPicker::get_history() { return s_history; }

void ColorPicker::set_history(const std::vector<uint32_t>& h) {
    s_history.assign(h.begin(),
                     h.begin() + std::min(h.size(), kHistoryMax));
}

// ── Presets ──────────────────────────────────────────────────────────────────
// White + the named colours the old preset leaves applied, plus yellow/orange.

namespace {
struct Preset { uint8_t r, g, b; };
constexpr Preset kPresets[] = {
    {255, 255, 255},   // white
    {  0, 220, 180},   // teal
    {  0, 180, 255},   // cyan
    {220,  30,  30},   // red
    { 30, 220,  60},   // green
    {180,  30, 220},   // purple
    {255, 220,  40},   // yellow
    {255, 140,   0},   // orange
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
} // namespace

// ── HSV ↔ RGB (same math the old Layered Effects panel used) ────────────────

static void rgb2hsv(int ir, int ig, int ib, float& h, float& s, float& v) {
    const float r = ir / 255.f, g = ig / 255.f, b = ib / 255.f;
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d  = mx - mn;
    v = mx;
    s = mx > 0.f ? (d / mx) : 0.f;
    if (d < 1e-6f)        h = 0.f;
    else if (mx == r)     h = 60.f * std::fmod((g - b) / d + 6.f, 6.f);
    else if (mx == g)     h = 60.f * ((b - r) / d + 2.f);
    else                  h = 60.f * ((r - g) / d + 4.f);
}

static void hsv2rgb(float h, float s, float v, int& r, int& g, int& b) {
    h = std::fmod(h, 360.f); if (h < 0.f) h += 360.f;
    const float c  = v * s;
    const float hp = h / 60.f;
    const float x  = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
    float rf = 0, gf = 0, bf = 0;
    if      (hp < 1) { rf = c; gf = x; }
    else if (hp < 2) { rf = x; gf = c; }
    else if (hp < 3) { gf = c; bf = x; }
    else if (hp < 4) { gf = x; bf = c; }
    else if (hp < 5) { rf = x; bf = c; }
    else             { rf = c; bf = x; }
    const float m = v - c;
    r = std::clamp(static_cast<int>(std::round((rf + m) * 255.f)), 0, 255);
    g = std::clamp(static_cast<int>(std::round((gf + m) * 255.f)), 0, 255);
    b = std::clamp(static_cast<int>(std::round((bf + m) * 255.f)), 0, 255);
}

// ── Typed-entry parsing (moved from menu_system.cpp) ─────────────────────────

// Parse a hex colour string into 0-255 RGB. Ignores any non-hex characters
// (so "#FF8000", "ff8000" both work) and expands 3-digit shorthand. Returns
// false if fewer than 3 hex digits were found.
static bool cp_parse_hex(const std::string& s, int& r, int& g, int& b) {
    std::string h;
    for (char c : s)
        if (std::isxdigit(static_cast<unsigned char>(c))) h += c;
    if (h.size() == 3) h = {h[0],h[0], h[1],h[1], h[2],h[2]};
    if (h.size() < 6) return false;
    auto hx = [&](size_t i){ return static_cast<int>(
        std::strtol(h.substr(i, 2).c_str(), nullptr, 16)); };
    r = hx(0); g = hx(2); b = hx(4);
    return true;
}

// Parse "r,g,b" (any non-digit separators — comma or space) into 0-255 RGB.
// Returns false unless three numbers were found.
static bool cp_parse_rgb(const std::string& s, int& r, int& g, int& b) {
    int v[3] = {0,0,0}, n = 0;
    for (size_t i = 0; i < s.size() && n < 3; ) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            int val = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                val = val * 10 + (s[i++] - '0');
                if (val > 9999) val = 9999;   // cap before overflow; clamped to 255 below
            }
            v[n++] = std::min(255, val);
        } else ++i;
    }
    if (n < 3) return false;
    r = v[0]; g = v[1]; b = v[2];
    return true;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void ColorPicker::open(std::string title, uint8_t r0, uint8_t g0, uint8_t b0,
                       ChangeFn on_change, OpenKbFn open_kb) {
    title_ = std::move(title);
    r_ = r0_ = r0;
    g_ = g0_ = g0;
    b_ = b0_ = b0;
    rgb2hsv(r_, g_, b_, h_, s_, v_);
    focus_      = RowSV;
    armed_      = false;
    hist_idx_   = 0;
    preset_idx_ = 0;
    on_change_  = std::move(on_change);
    open_kb_    = std::move(open_kb);
    open_       = true;
    emit_detents();
}

void ColorPicker::close() {
    open_       = false;
    armed_      = false;
    on_change_  = nullptr;
    open_kb_    = nullptr;
    detent_cb_  = nullptr;
}

// ── Colour plumbing ──────────────────────────────────────────────────────────

void ColorPicker::apply() {
    if (on_change_)
        on_change_(static_cast<uint8_t>(r_),
                   static_cast<uint8_t>(g_),
                   static_cast<uint8_t>(b_));
}

void ColorPicker::sync_hsv_from_rgb() {
    // Hue-sticky: keep the last meaningful hue (and saturation) when the
    // colour collapses to gray/black, so the SV marker doesn't jump to 0.
    float h, s, v;
    rgb2hsv(r_, g_, b_, h, s, v);
    if (v > 0.f) {
        if (s > 0.f) h_ = h;
        s_ = s;
    }
    v_ = v;
}

void ColorPicker::set_rgb(int r, int g, int b) {
    r_ = std::clamp(r, 0, 255);
    g_ = std::clamp(g, 0, 255);
    b_ = std::clamp(b, 0, 255);
    sync_hsv_from_rgb();
    apply();
}

void ColorPicker::rgb_from_hsv() {
    h_ = std::fmod(h_, 360.f); if (h_ < 0.f) h_ += 360.f;
    s_ = std::clamp(s_, 0.f, 1.f);
    v_ = std::clamp(v_, 0.f, 1.f);
    hsv2rgb(h_, s_, v_, r_, g_, b_);
    apply();
}

void ColorPicker::use_history(int idx) {
    if (s_history.empty()) return;
    const int n = static_cast<int>(s_history.size());
    hist_idx_ = ((idx % n) + n) % n;
    const uint32_t c = s_history[hist_idx_];
    set_rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

void ColorPicker::use_preset(int idx) {
    preset_idx_ = ((idx % kPresetCount) + kPresetCount) % kPresetCount;
    const Preset& p = kPresets[preset_idx_];
    set_rgb(p.r, p.g, p.b);
}

void ColorPicker::confirm() {
    push_history((static_cast<uint32_t>(r_) << 16) |
                 (static_cast<uint32_t>(g_) <<  8) |
                  static_cast<uint32_t>(b_));
    close();   // value is already applied live
}

void ColorPicker::cancel() {
    if (on_change_) on_change_(r0_, g0_, b0_);
    close();
}

void ColorPicker::open_typed_entry() {
    if (!open_kb_) return;
    char cur[16];
    std::snprintf(cur, sizeof(cur), "%02X%02X%02X", r_, g_, b_);
    open_kb_("Color: hex RRGGBB or R,G,B", cur,
        [this](const std::string& s){
            if (!open_) return;   // picker torn down while the keyboard was up
            int r, g, b;
            // "r,g,b" first when a comma is present — "1,2,3" would otherwise
            // hex-parse as shorthand "123". Bad parses keep the picker open
            // so the user can retry.
            const bool ok = (s.find(',') != std::string::npos)
                ? cp_parse_rgb(s, r, g, b)
                : (cp_parse_hex(s, r, g, b) || cp_parse_rgb(s, r, g, b));
            if (ok) set_rgb(r, g, b);
        });
}

// ── Input ────────────────────────────────────────────────────────────────────

bool ColorPicker::row_skipped(int row) const {
    return row == RowHistory && s_history.empty();
}

void ColorPicker::emit_detents() {
    if (!detent_cb_) return;
    int n = RowCount - (row_skipped(RowHistory) ? 1 : 0);
    if (armed_) {
        switch (focus_) {
            case RowR: case RowG: case RowB: n = 256; break;
            case RowHue:     n = 180; break;   // 2° per detent
            case RowSV:      n = 51;  break;   // 2% value per detent
            case RowHistory: n = std::max(1, static_cast<int>(s_history.size())); break;
            case RowPresets: n = kPresetCount; break;
            default: break;
        }
    }
    detent_cb_(n);
}

// Armed adjustment: +d always increases (input handlers flip Up/Down via
// adjusting(), same as slider editing).
void ColorPicker::adjust(int d) {
    switch (focus_) {
        case RowR:   set_rgb(r_ + d, g_, b_); break;
        case RowG:   set_rgb(r_, g_ + d, b_); break;
        case RowB:   set_rgb(r_, g_, b_ + d); break;
        case RowHue: h_ += 2.f * d;    rgb_from_hsv(); break;
        case RowSV:  v_ += 0.02f * d;  rgb_from_hsv(); break;
        case RowHistory: use_history(hist_idx_ + d); break;
        case RowPresets: use_preset(preset_idx_ + d); break;
        default: break;
    }
}

void ColorPicker::step(int d) {
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

void ColorPicker::move(int dx, int dy) {
    if (!open_) return;
    if (dx != 0) {
        // Horizontal: in-row adjustment, coarser than the knob.
        switch (focus_) {
            case RowSV:  s_ += 0.05f * dx; rgb_from_hsv(); break;
            case RowHue: h_ += 6.f * dx;   rgb_from_hsv(); break;
            case RowR:   set_rgb(r_ + 5 * dx, g_, b_); break;
            case RowG:   set_rgb(r_, g_ + 5 * dx, b_); break;
            case RowB:   set_rgb(r_, g_, b_ + 5 * dx); break;
            case RowHistory: use_history(hist_idx_ + dx); break;
            case RowPresets: use_preset(preset_idx_ + dx); break;
            default: break;
        }
    }
    if (dy != 0) {
        if (armed_) adjust(-dy);   // up = increase while a field is armed
        else        step(dy);
    }
}

void ColorPicker::activate() {
    if (!open_) return;
    switch (focus_) {
        case RowHex:     open_typed_entry(); break;
        case RowConfirm: confirm(); return;
        case RowCancel:  cancel();  return;
        case RowHistory:
            if (s_history.empty()) break;
            armed_ = !armed_;
            if (armed_) use_history(hist_idx_);   // preview current swatch on arm
            emit_detents();
            break;
        case RowPresets:
            armed_ = !armed_;
            if (armed_) use_preset(preset_idx_);
            emit_detents();
            break;
        default:         // SV / Hue / R / G / B — arm, knob adjusts; confirm on re-press
            armed_ = !armed_;
            emit_detents();
            break;
    }
}

void ColorPicker::back() {
    if (!open_) return;
    if (armed_) {          // stop adjusting, keep the value
        armed_ = false;
        emit_detents();
        return;
    }
    cancel();              // top level: revert + close
}

// ── Draw ─────────────────────────────────────────────────────────────────────

static ImU32 with_alpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24u);
}

void ColorPicker::draw(ImDrawList* dl, ImFont* font, float fs,
                       float W, float H, ImU32 accent) {
    if (!open_) return;

    const ImVec2 mp           = ImGui::GetMousePos();
    const bool   mouse_click  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool   mouse_down   = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    auto hit = [&](float x0, float y0, float x1, float y1) {
        return mp.x >= x0 && mp.x < x1 && mp.y >= y0 && mp.y < y1;
    };

    // Dim + panel chrome (same look as the other overlays).
    dl->AddRectFilled({0.f, 0.f}, {W, H}, IM_COL32(4, 8, 12, 205));
    const float pw = std::min(W * 0.88f, 880.f);
    const float ph = std::min(H * 0.82f, 560.f);
    const ImVec2 pmin{ (W - pw) * 0.5f, (H - ph) * 0.5f };
    const ImVec2 pmax{ pmin.x + pw, pmin.y + ph };
    dl->AddRectFilled(pmin, pmax, IM_COL32(8, 12, 16, 235));
    dl->AddRect      (pmin, pmax, with_alpha(accent, 220), 0.f, 0, 2.f);

    const float pad = 24.f;
    const float x0  = pmin.x + pad;
    const float x1  = pmax.x - pad;

    // Title.
    dl->AddText(font, fs * 1.4f, { x0, pmin.y + 12.f },
                IM_COL32(255, 255, 255, 255), title_.c_str());

    const float top = pmin.y + 12.f + fs * 1.4f + 14.f;
    const float bot = pmax.y - fs - 22.f;   // hint bar space

    const ImU32 col_dim   = IM_COL32(150, 160, 170, 200);
    const ImU32 col_sel   = IM_COL32(255, 255, 255, 255);
    const ImU32 col_frame = IM_COL32(80, 90, 100, 255);

    // ── Left column: SV square + hue strip + swatch/hex readout ─────────────
    const float strip_h  = 16.f;
    const float swatch_h = fs * 1.7f;
    float sv = std::min((x1 - x0) * 0.46f,
                        (bot - top) - strip_h - swatch_h - 26.f);
    if (sv < 60.f) sv = 60.f;
    const float sv_x = x0, sv_y = top;

    // Saturation × Value square: white → pure hue across, → black down.
    int hr, hg, hb;
    hsv2rgb(h_, 1.f, 1.f, hr, hg, hb);
    dl->AddRectFilledMultiColor({sv_x, sv_y}, {sv_x + sv, sv_y + sv},
                                IM_COL32(255, 255, 255, 255),
                                IM_COL32(hr, hg, hb, 255),
                                IM_COL32(0, 0, 0, 255),
                                IM_COL32(0, 0, 0, 255));
    dl->AddRect({sv_x, sv_y}, {sv_x + sv, sv_y + sv}, col_frame, 0.f, 0, 1.f);
    if (focus_ == RowSV)
        dl->AddRect({sv_x - 3.f, sv_y - 3.f}, {sv_x + sv + 3.f, sv_y + sv + 3.f},
                    with_alpha(accent, armed_ ? 255 : 170), 2.f, 0, armed_ ? 2.5f : 1.5f);
    // SV marker at (S, 1-V).
    {
        const float mx = sv_x + s_ * sv;
        const float my = sv_y + (1.f - v_) * sv;
        dl->AddCircle({mx, my}, 6.f, IM_COL32(0, 0, 0, 230), 0, 2.5f);
        dl->AddCircle({mx, my}, 6.f, IM_COL32(255, 255, 255, 230), 0, 1.f);
    }
    if (mouse_down && hit(sv_x, sv_y, sv_x + sv, sv_y + sv)) {
        focus_ = RowSV;
        s_ = (mp.x - sv_x) / sv;
        v_ = 1.f - (mp.y - sv_y) / sv;
        rgb_from_hsv();
    }

    // Hue strip — 12 bilinearly-shaded rainbow segments.
    const float hs_x = sv_x, hs_y = sv_y + sv + 10.f, hs_w = sv;
    constexpr int kSegs = 12;
    for (int i = 0; i < kSegs; ++i) {
        int r0, g0, b0, r1, g1, b1;
        hsv2rgb(360.f * i       / kSegs, 1.f, 1.f, r0, g0, b0);
        hsv2rgb(360.f * (i + 1) / kSegs, 1.f, 1.f, r1, g1, b1);
        const float sx0 = hs_x + hs_w * i       / kSegs;
        const float sx1 = hs_x + hs_w * (i + 1) / kSegs;
        dl->AddRectFilledMultiColor({sx0, hs_y}, {sx1, hs_y + strip_h},
                                    IM_COL32(r0, g0, b0, 255), IM_COL32(r1, g1, b1, 255),
                                    IM_COL32(r1, g1, b1, 255), IM_COL32(r0, g0, b0, 255));
    }
    dl->AddRect({hs_x, hs_y}, {hs_x + hs_w, hs_y + strip_h}, col_frame, 0.f, 0, 1.f);
    if (focus_ == RowHue)
        dl->AddRect({hs_x - 3.f, hs_y - 3.f}, {hs_x + hs_w + 3.f, hs_y + strip_h + 3.f},
                    with_alpha(accent, armed_ ? 255 : 170), 2.f, 0, armed_ ? 2.5f : 1.5f);
    {
        const float hmx = hs_x + (h_ / 360.f) * hs_w;
        dl->AddLine({hmx, hs_y - 2.f}, {hmx, hs_y + strip_h + 2.f}, IM_COL32(0, 0, 0, 230), 3.f);
        dl->AddLine({hmx, hs_y - 2.f}, {hmx, hs_y + strip_h + 2.f}, IM_COL32(255, 255, 255, 230), 1.f);
    }
    if (mouse_down && hit(hs_x, hs_y, hs_x + hs_w, hs_y + strip_h)) {
        focus_ = RowHue;
        h_ = std::clamp((mp.x - hs_x) / hs_w, 0.f, 1.f) * 360.f;
        rgb_from_hsv();
    }

    // Current swatch + hex readout.
    {
        const float sw_y = hs_y + strip_h + 10.f;
        const float sw_w = sv * 0.45f;
        dl->AddRectFilled({sv_x, sw_y}, {sv_x + sw_w, sw_y + swatch_h},
                          IM_COL32(r_, g_, b_, 255), 4.f);
        dl->AddRect({sv_x, sw_y}, {sv_x + sw_w, sw_y + swatch_h}, col_frame, 4.f, 0, 1.f);
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r_, g_, b_);
        dl->AddText(font, fs * 1.1f,
                    { sv_x + sw_w + 12.f, sw_y + (swatch_h - fs * 1.1f) * 0.5f },
                    IM_COL32(230, 235, 240, 255), hex);
    }

    // ── Right column: focus rows ─────────────────────────────────────────────
    const float rx0 = x0 + sv + 30.f;
    const float rx1 = x1;
    const float rh  = std::min(fs * 2.4f, (bot - top) / 8.f);

    auto row_y = [&](int i) { return top + i * rh; };   // R=0 … CANCEL=7
    auto row_chrome = [&](int row, int i) {
        const float ry = row_y(i);
        if (focus_ == row) {
            dl->AddRectFilled({rx0 - 10.f, ry}, {rx1, ry + rh - 4.f},
                              with_alpha(accent, 40), 3.f);
            dl->AddRectFilled({rx0 - 10.f, ry}, {rx0 - 6.f, ry + rh - 4.f}, accent);
            if (armed_)
                dl->AddRect({rx0 - 10.f, ry}, {rx1, ry + rh - 4.f},
                            with_alpha(accent, 230), 3.f, 0, 1.5f);
        }
        return ry;
    };

    // R / G / B field rows: label, bar, value.
    const ImU32 ch_cols[3] = { IM_COL32(220, 60, 60, 220),
                               IM_COL32(60, 200, 60, 220),
                               IM_COL32(60, 80, 220, 220) };
    const char* ch_names[3] = { "R", "G", "B" };
    const int   ch_vals[3]  = { r_, g_, b_ };
    for (int c = 0; c < 3; ++c) {
        const int   row = RowR + c;
        const float ry  = row_chrome(row, c);
        const float ty  = ry + (rh - 4.f - fs) * 0.5f;
        dl->AddText(font, fs, { rx0, ty },
                    focus_ == row ? col_sel : col_dim, ch_names[c]);
        const float bx = rx0 + fs * 1.6f;
        const float bw = (rx1 - bx) - fs * 3.2f;
        const float bh = fs * 0.8f;
        const float by = ry + (rh - 4.f - bh) * 0.5f;
        dl->AddRectFilled({bx, by}, {bx + bw, by + bh}, with_alpha(accent, 50), 2.f);
        dl->AddRectFilled({bx, by}, {bx + bw * (ch_vals[c] / 255.f), by + bh},
                          ch_cols[c], 2.f);
        char vb[8];
        std::snprintf(vb, sizeof(vb), "%d", ch_vals[c]);
        ImVec2 vsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, vb);
        dl->AddText(font, fs, { rx1 - vsz.x - 4.f, ty },
                    focus_ == row ? col_sel : col_dim, vb);
        // Mouse: drag the bar like a slider; click elsewhere in the row focuses.
        if (mouse_down && hit(bx, ry, bx + bw, ry + rh - 4.f)) {
            focus_ = row;
            const int nv = static_cast<int>(std::round(
                std::clamp((mp.x - bx) / bw, 0.f, 1.f) * 255.f));
            if      (c == 0) set_rgb(nv, g_, b_);
            else if (c == 1) set_rgb(r_, nv, b_);
            else             set_rgb(r_, g_, nv);
        } else if (mouse_click && hit(rx0 - 10.f, ry, rx1, ry + rh - 4.f)) {
            focus_ = row;
        }
    }

    // HEX typed-entry row.
    {
        const float ry = row_chrome(RowHex, 3);
        const float ty = ry + (rh - 4.f - fs) * 0.5f;
        const ImU32 tc = focus_ == RowHex ? col_sel : col_dim;
        dl->AddText(font, fs, { rx0, ty }, tc, "HEX");
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%02X%02X%02X", r_, g_, b_);
        dl->AddText(font, fs, { rx0 + fs * 3.4f, ty }, tc, hex);
        const char* type_hint = "type \xE2\x80\xA6";
        ImVec2 hsz = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.f, type_hint);
        dl->AddText(font, fs * 0.9f, { rx1 - hsz.x - 4.f, ty },
                    with_alpha(accent, 150), type_hint);
        if (mouse_click && hit(rx0 - 10.f, ry, rx1, ry + rh - 4.f)) {
            focus_ = RowHex;
            armed_ = false;
            open_typed_entry();
        }
    }

    // Swatch rows: shared history + presets.
    auto swatch_row = [&](int row, int i, const char* label,
                          int count, int sel_idx,
                          auto color_of, auto on_pick) {
        const float ry = row_chrome(row, i);
        const float ty = ry + (rh - 4.f - fs) * 0.5f;
        const bool  fr = (focus_ == row);
        dl->AddText(font, fs * 0.85f, { rx0, ty + fs * 0.1f },
                    fr ? col_sel : col_dim, label);
        if (count <= 0) {
            dl->AddText(font, fs * 0.85f, { rx0 + fs * 4.4f, ty + fs * 0.1f },
                        IM_COL32(120, 128, 136, 160), "(empty)");
            return;
        }
        const float sx0  = rx0 + fs * 4.4f;
        const float gap  = 5.f;
        float side = std::min(rh - 10.f, (rx1 - sx0 - gap * (count - 1)) / count);
        side = std::max(8.f, side);
        const float sy = ry + (rh - 4.f - side) * 0.5f;
        for (int k = 0; k < count; ++k) {
            const float kx = sx0 + k * (side + gap);
            const ImU32 cc = color_of(k);
            dl->AddRectFilled({kx, sy}, {kx + side, sy + side}, cc, 2.f);
            dl->AddRect({kx, sy}, {kx + side, sy + side},
                        (fr && k == sel_idx) ? with_alpha(accent, 255) : col_frame,
                        2.f, 0, (fr && k == sel_idx) ? 2.f : 1.f);
            if (mouse_click && hit(kx, sy, kx + side, sy + side)) {
                focus_ = row;
                on_pick(k);
            }
        }
    };
    swatch_row(RowHistory, 4, "RECENT",
               static_cast<int>(s_history.size()), hist_idx_,
               [&](int k){ const uint32_t c = s_history[k];
                           return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255); },
               [&](int k){ use_history(k); });
    swatch_row(RowPresets, 5, "PRESET",
               kPresetCount, preset_idx_,
               [&](int k){ const Preset& p = kPresets[k];
                           return IM_COL32(p.r, p.g, p.b, 255); },
               [&](int k){ use_preset(k); });

    // CONFIRM / CANCEL buttons.
    auto button_row = [&](int row, int i, const char* label, ImU32 base) {
        const float ry = row_chrome(row, i);
        const ImVec2 bmin{ rx0, ry + 2.f }, bmax{ rx1, ry + rh - 6.f };
        const bool fr = (focus_ == row);
        dl->AddRectFilled(bmin, bmax, fr ? IM_COL32(255, 255, 255, 235) : base, 3.f);
        if (fr) dl->AddRect(bmin, bmax, with_alpha(accent, 230), 3.f, 0, 1.5f);
        ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
        dl->AddText(font, fs,
                    { (bmin.x + bmax.x - tsz.x) * 0.5f,
                      (bmin.y + bmax.y - fs) * 0.5f },
                    fr ? IM_COL32(10, 12, 14, 255) : IM_COL32(230, 235, 240, 220),
                    label);
        return mouse_click && hit(bmin.x, bmin.y, bmax.x, bmax.y);
    };
    const bool clicked_ok     = button_row(RowConfirm, 6, "CONFIRM", IM_COL32(40, 110, 60, 150));
    const bool clicked_cancel = button_row(RowCancel,  7, "CANCEL",  IM_COL32(120, 50, 50, 150));

    // Hint bar.
    dl->AddText(font, fs * 0.9f, { x0, pmax.y - fs - 10.f },
                with_alpha(accent, 185),
                armed_
                  ? "ROTATE / \xE2\x86\x90\xE2\x86\x92 ADJUST   \xC2\xB7   SELECT DONE   \xC2\xB7   BACK STOP"
                  : "ROTATE ROW   \xC2\xB7   \xE2\x86\x90\xE2\x86\x92 ADJUST   \xC2\xB7   SELECT EDIT/USE   \xC2\xB7   BACK CANCEL");

    // Deferred button actions (confirm/cancel tear the picker down — do it
    // after every rect above has been emitted).
    if      (clicked_ok)     confirm();
    else if (clicked_cancel) cancel();
}

} // namespace menu

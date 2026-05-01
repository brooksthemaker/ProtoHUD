#include "menu_system.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

static bool s_menu_glow = true;

static std::string to_upper(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (unsigned char c : s) r += static_cast<char>(::toupper(c));
    return r;
}

// Derive alpha-variant of an ImU32 (format ABGR, alpha in high byte).
static ImU32 menu_with_alpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24u);
}

// Convert ImU32 to ImVec4, optionally overriding alpha.
static ImVec4 col_to_vec4(ImU32 col, float alpha_override = -1.f) {
    float r = ((col >>  0) & 0xFF) / 255.f;
    float g = ((col >>  8) & 0xFF) / 255.f;
    float b = ((col >> 16) & 0xFF) / 255.f;
    float a = alpha_override >= 0.f ? alpha_override : ((col >> 24) & 0xFF) / 255.f;
    return {r, g, b, a};
}

// Draw text with a glow outline using the supplied accent color.
static void draw_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                            bool selected, ImU32 accent_col) {
    const ImU32 fill_sel = IM_COL32(255, 255, 255, 255);
    const ImU32 fill_dim = IM_COL32(255, 255, 255, 160);
    const ImU32 fill     = selected ? fill_sel : fill_dim;

    if (s_menu_glow) {
        const ImU32 glow     = selected ? menu_with_alpha(accent_col, 72) : menu_with_alpha(accent_col, 22);
        const ImU32 glow_far = menu_with_alpha(accent_col, 28);
        constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
        constexpr int D2[4][2] = {{-2,0},{2,0},{0,-2},{0,2}};
        for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
        if (selected)
            for (auto& o : D2) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow_far, text);
    }
    dl->AddText(pos, fill, text);
}

static void format_slider_value(char* buf, size_t bufsz,
                                float val, float min, float max,
                                const std::string& unit)
{
    if (unit == "%") {
        // "percentage of max" scaling: face brightness 0-255 → 0%-100%
        float pct = (max > min) ? (val - min) / (max - min) * 100.f : 0.f;
        std::snprintf(buf, bufsz, "%.0f%%", pct);
    } else if (!unit.empty()) {
        // literal suffix: " %", " EV", etc.
        if (unit == " EV" && val > 0.f)
            std::snprintf(buf, bufsz, "+%.1f%s", val, unit.c_str());
        else
            std::snprintf(buf, bufsz, "%.1f%s", val, unit.c_str());
    } else {
        std::snprintf(buf, bufsz, "%.0f", val);
    }
    (void)min;
}

MenuSystem::MenuSystem(std::vector<MenuItem> root)
    : root_items_(std::move(root)) {}

void MenuSystem::push_level(const std::vector<MenuItem>& items) {
    if (items.empty()) return;
    std::vector<MenuItem> level = items;

    // Append navigation item: "Close Menu" at root, "< Back" in submenus.
    // When selected, back() pops the level (or closes at root depth=1).
    MenuItem nav;
    nav.type   = MenuItemType::LEAF;
    nav.label  = stack_.empty() ? "Close Menu" : "< Back";
    nav.action = [this] { this->back(); };
    level.push_back(nav);

    stack_.push_back({ level });
    cursor_ = 0;
    emit_detents();
}

void MenuSystem::pop_level() {
    if (stack_.size() > 1) {
        stack_.pop_back();
        cursor_ = 0;
        emit_detents();
    } else {
        close();
    }
}

void MenuSystem::emit_detents() {
    if (detent_cb_ && !stack_.empty())
        detent_cb_(static_cast<int>(stack_.back().items.size()));
}

void MenuSystem::emit_detents_override(int count) {
    if (detent_cb_) detent_cb_(count);
}

// ── navigate ──────────────────────────────────────────────────────────────────

void MenuSystem::navigate(int direction) {
    if (!open_ || stack_.empty()) return;

    if (in_edit_mode_) {
        auto& item = stack_.back().items[cursor_];

        if (item.type == MenuItemType::COLOR_PICKER) {
            if (in_channel_edit_) {
                float* ch = (edit_channel_ == 0) ? &edit_r_
                          : (edit_channel_ == 1) ? &edit_g_
                          :                        &edit_b_;
                *ch = std::clamp(*ch + static_cast<float>(direction), 0.f, 255.f);
                // Live preview — apply immediately so the user sees/feels the change
                if (item.color.set_color)
                    item.color.set_color(static_cast<uint8_t>(edit_r_),
                                         static_cast<uint8_t>(edit_g_),
                                         static_cast<uint8_t>(edit_b_));
            } else {
                edit_channel_ = ((edit_channel_ + direction) % 3 + 3) % 3;
            }
        } else if (item.type == MenuItemType::SLIDER) {
            edit_float_ = std::clamp(
                edit_float_ + static_cast<float>(direction) * item.slider.step,
                item.slider.min, item.slider.max);
            // Live preview — apply immediately so the user hears/sees the change
            if (item.slider.set_value) item.slider.set_value(edit_float_);
        }
        return;
    }

    int n = static_cast<int>(stack_.back().items.size());
    cursor_ = ((cursor_ + direction) % n + n) % n;
}

// ── select ────────────────────────────────────────────────────────────────────

void MenuSystem::select() {
    if (!open_ || stack_.empty()) return;
    auto& items = stack_.back().items;
    if (cursor_ >= static_cast<int>(items.size())) return;
    auto& item = items[cursor_];

    switch (item.type) {
    case MenuItemType::SUBMENU:
        if (!item.children.empty())
            push_level(item.children);
        break;

    case MenuItemType::LEAF:
        if (item.action) { item.action(); }
        // Menu stays open — only "Close Menu" / "< Back" items call close()/back()
        break;

    case MenuItemType::TOGGLE:
        if (item.get_toggle && item.set_toggle)
            item.set_toggle(!item.get_toggle());
        // stay open — no close(), no push
        break;

    case MenuItemType::SLIDER:
        if (!in_edit_mode_) {
            edit_float_   = item.slider.get_value ? item.slider.get_value() : item.slider.min;
            orig_float_   = edit_float_;   // snapshot for cancel/restore
            in_edit_mode_ = true;
            int steps = (item.slider.step > 0.f)
                ? static_cast<int>((item.slider.max - item.slider.min) / item.slider.step) + 1
                : 64;
            emit_detents_override(steps);
        } else {
            if (item.slider.set_value) item.slider.set_value(edit_float_);
            in_edit_mode_ = false;
            emit_detents();
        }
        break;

    case MenuItemType::COLOR_PICKER:
        if (!in_edit_mode_) {
            if (item.color.get_color) {
                auto [r, g, b] = item.color.get_color();
                edit_r_ = static_cast<float>(r);
                edit_g_ = static_cast<float>(g);
                edit_b_ = static_cast<float>(b);
            } else {
                edit_r_ = edit_g_ = edit_b_ = 128.f;
            }
            orig_r_ = edit_r_; orig_g_ = edit_g_; orig_b_ = edit_b_;  // snapshot for cancel
            edit_channel_    = 0;
            in_channel_edit_ = false;
            in_edit_mode_    = true;
            emit_detents_override(3);
        } else if (!in_channel_edit_) {
            in_channel_edit_ = true;
            emit_detents_override(256);
        } else {
            in_channel_edit_ = false;
            edit_channel_    = (edit_channel_ + 1) % 3;
            if (edit_channel_ == 0) {
                if (item.color.set_color)
                    item.color.set_color(
                        static_cast<uint8_t>(edit_r_),
                        static_cast<uint8_t>(edit_g_),
                        static_cast<uint8_t>(edit_b_));
                in_edit_mode_ = false;
                emit_detents();
            } else {
                emit_detents_override(3);
            }
        }
        break;
    }
}

// ── back ──────────────────────────────────────────────────────────────────────

void MenuSystem::back() {
    if (!stack_.empty() && cursor_ < static_cast<int>(stack_.back().items.size())) {
        auto& item = stack_.back().items[cursor_];

        if (in_channel_edit_) {
            // Restore original color, reset working copies, exit channel-edit
            if (item.color.set_color)
                item.color.set_color(static_cast<uint8_t>(orig_r_),
                                     static_cast<uint8_t>(orig_g_),
                                     static_cast<uint8_t>(orig_b_));
            edit_r_ = orig_r_; edit_g_ = orig_g_; edit_b_ = orig_b_;
            in_channel_edit_ = false;
            emit_detents_override(3);
            return;
        }
        if (in_edit_mode_) {
            if (item.type == MenuItemType::SLIDER) {
                if (item.slider.set_value) item.slider.set_value(orig_float_);
            } else if (item.type == MenuItemType::COLOR_PICKER) {
                if (item.color.set_color)
                    item.color.set_color(static_cast<uint8_t>(orig_r_),
                                         static_cast<uint8_t>(orig_g_),
                                         static_cast<uint8_t>(orig_b_));
            }
            in_edit_mode_ = false;
            emit_detents();
            return;
        }
    }
    pop_level();
}

// ── current_label ─────────────────────────────────────────────────────────────

const std::string& MenuSystem::current_label() const {
    static std::string empty;
    if (stack_.empty()) return empty;
    const auto& items = stack_.back().items;
    if (cursor_ < static_cast<int>(items.size()))
        return items[cursor_].label;
    return empty;
}

// ── draw ──────────────────────────────────────────────────────────────────────

void MenuSystem::draw(int screen_w, int screen_h) {
    if (!open_ || stack_.empty()) return;
    (void)screen_w;
    s_menu_glow = glow_enabled_;

    const auto& items  = stack_.back().items;
    const float item_h = 38.f;
    const float pad_x  = 18.f;
    const float pad_y  = 14.f;
    const float width  = 380.f;

    // Extra height for expanded editing rows
    float extra = 0.f;
    if (in_edit_mode_ && cursor_ < static_cast<int>(items.size())) {
        const auto& sel = items[cursor_];
        if (sel.type == MenuItemType::SLIDER)       extra = 30.f;
        if (sel.type == MenuItemType::COLOR_PICKER) extra = 96.f;
    }

    const float total_h = pad_y * 2.f
                        + item_h * static_cast<float>(items.size())
                        + extra;
    const float x = 48.f;
    const float y = 48.f;   // top-anchored: fixed distance from screen top

    const bool   filled_row  = (selection_style_ == SelectionStyle::FILLED_ROW);
    const ImU32  COL_SEP     = menu_with_alpha(accent_color_, 45);
    const ImU32  COL_SEP_EFF = filled_row ? IM_COL32(255, 255, 255, 60) : COL_SEP;

    ImGui::SetNextWindowPos ({ x, y }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ width, total_h }, ImGuiCond_Always);
    if (!bg_enabled_) ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoFocusOnAppearing;

    // Window bg is transparent — drawn manually below as a chamfered shape.
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        col_to_vec4(accent_color_, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Header,        col_to_vec4(accent_color_, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col_to_vec4(accent_color_, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  col_to_vec4(accent_color_, 0.32f));
    // Suppress Selectable's own text — we draw it manually via DrawList
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);  // manual border below
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0.f, 0.f));

    ImGui::Begin("##menu", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Chamfered background + border ────────────────────────────────────────
    {
        constexpr float C = 8.f;  // chamfer distance (corner cut)
        // GAP keeps exactly 3px of transparent strip between the inner edge of
        // the border line and the outer edge of the bg fill, regardless of
        // border thickness (border line is centered on the window edge, so its
        // inner edge is at thickness/2 inward; bg fill is inset by GAP).
        const float GAP = 3.f + border_thickness_ * 0.5f;

        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();

        // Background fill inset by GAP — chamfered octagon
        const ImVec2 bmin = {wp.x + GAP,        wp.y + GAP};
        const ImVec2 bmax = {wp.x + ws.x - GAP, wp.y + ws.y - GAP};
        const ImVec2 bg_pts[8] = {
            {bmin.x + C, bmin.y}, {bmax.x - C, bmin.y},
            {bmax.x, bmin.y + C}, {bmax.x, bmax.y - C},
            {bmax.x - C, bmax.y}, {bmin.x + C, bmax.y},
            {bmin.x, bmax.y - C}, {bmin.x, bmin.y + C},
        };
        const ImU32 bg_col = bg_enabled_ ? bg_color_ : IM_COL32(0, 0, 0, 0);
        dl->AddConvexPolyFilled(bg_pts, 8, bg_col);

        // Border at window edge — chamfered polyline
        if (border_enabled_) {
            const ImVec2 emin = {wp.x,        wp.y};
            const ImVec2 emax = {wp.x + ws.x, wp.y + ws.y};
            const ImVec2 bdr_pts[8] = {
                {emin.x + C, emin.y}, {emax.x - C, emin.y},
                {emax.x, emin.y + C}, {emax.x, emax.y - C},
                {emax.x - C, emax.y}, {emin.x + C, emax.y},
                {emin.x, emax.y - C}, {emin.x, emin.y + C},
            };
            dl->AddPolyline(bdr_pts, 8, menu_with_alpha(border_color_, 220),
                            ImDrawFlags_Closed, border_thickness_);
        }
    }

    const float line_h = ImGui::GetTextLineHeight();

    // Text drawing helper: FILLED_ROW selected rows use bold-style black text,
    // all others use the standard glow system.
    auto draw_item_text = [&](ImVec2 pos, const char* text, bool sel) {
        if (filled_row && sel) {
            dl->AddText({pos.x - 0.6f, pos.y}, IM_COL32(0, 0, 0, 255), text);
            dl->AddText({pos.x + 0.6f, pos.y}, IM_COL32(0, 0, 0, 255), text);
            dl->AddText(pos,                   IM_COL32(0, 0, 0, 255), text);
        } else {
            draw_glow_text(dl, pos, text, sel, accent_color_);
        }
    };

    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        bool selected = (i == cursor_);
        const auto& item = items[i];

        // Row height: expanded for the selected item in edit mode
        float row_h = item_h - 1.f;
        if (selected && in_edit_mode_) {
            if (item.type == MenuItemType::SLIDER)       row_h = item_h + 25.f;
            if (item.type == MenuItemType::COLOR_PICKER) row_h = item_h + 95.f;
        }

        char id[32]; snprintf(id, sizeof(id), "##item%d", i);
        if (ImGui::Selectable(id, selected, 0, ImVec2(0.f, row_h))) {
            cursor_ = i;
            select();
        }

        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();

        // Selection highlight: filled white row (Halo) or left accent bar (default)
        if (selected) {
            if (filled_row)
                dl->AddRectFilled({rmin.x - pad_x, rmin.y},
                                  {rmax.x + pad_x, rmax.y}, IM_COL32(255, 255, 255, 235));
            else
                dl->AddRectFilled({rmin.x - pad_x,       rmin.y},
                                  {rmin.x - pad_x + 4.f, rmax.y}, accent_color_);
        }

        // Text Y position (vertically centered in the base item_h row)
        float ty = rmin.y + (item_h - line_h) * 0.5f - 0.5f;

        // ── TOGGLE ────────────────────────────────────────────────────────────
        if (item.type == MenuItemType::TOGGLE) {
            bool on = item.get_toggle ? item.get_toggle() : false;

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item.label).c_str(), selected);

            // Radio-style circle + " ON" / " OFF" text, both using accent_color_.
            const char* state_str = on ? " ON" : " OFF";
            ImVec2      state_sz  = ImGui::CalcTextSize(state_str);
            constexpr float dot_r = 5.f;
            const float text_x    = rmax.x - state_sz.x - 6.f;
            const float dot_cx    = text_x - dot_r - 4.f;
            const float dot_cy    = rmin.y + item_h * 0.5f;

            const ImU32 dot_col   = (filled_row && selected) ? IM_COL32(0,0,0,200) : accent_color_;
            if (on) {
                dl->AddCircleFilled({dot_cx, dot_cy}, dot_r,   dot_col);
                dl->AddCircleFilled({dot_cx, dot_cy}, 2.5f,    IM_COL32(255, 255, 255, 200));
            } else {
                dl->AddCircle({dot_cx, dot_cy}, dot_r,
                              menu_with_alpha(dot_col, 100), 0, 1.5f);
            }
            const ImU32 text_col = (filled_row && selected)
                ? IM_COL32(0, 0, 0, 200)
                : (on ? accent_color_ : menu_with_alpha(accent_color_, 120));
            dl->AddText({text_x, ty}, text_col, state_str);

        // ── SLIDER ────────────────────────────────────────────────────────────
        } else if (item.type == MenuItemType::SLIDER) {
            bool  editing = selected && in_edit_mode_;
            float val = editing
                ? edit_float_
                : (item.slider.get_value ? item.slider.get_value() : item.slider.min);
            float range = item.slider.max - item.slider.min;
            float fill  = (range > 0.f)
                ? std::clamp((val - item.slider.min) / range, 0.f, 1.f) : 0.f;

            char val_str[32];
            format_slider_value(val_str, sizeof(val_str),
                                val, item.slider.min, item.slider.max, item.slider.unit);

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item.label).c_str(), selected);

            if (editing) {
                float bx = rmin.x + 4.f;
                float by = rmin.y + item_h - 2.f;
                float bw = (rmax.x - rmin.x) - 64.f;
                float bh = 10.f;
                dl->AddRectFilled({bx, by}, {bx + bw, by + bh},
                                  menu_with_alpha(accent_color_, 60), 3.f);
                dl->AddRectFilled({bx, by}, {bx + bw * fill, by + bh},
                                  menu_with_alpha(accent_color_, 220), 3.f);
                float tick_x = bx + bw * fill;
                dl->AddLine({tick_x, by - 2.f}, {tick_x, by + bh + 2.f},
                            IM_COL32(255, 255, 255, 200), 2.f);
                dl->AddText({bx + bw + 6.f, by}, IM_COL32(255, 255, 255, 255), val_str);
                dl->AddText({bx, by - 14.f},
                            menu_with_alpha(accent_color_, 180),
                            "knob  \xC2\xB7  select=confirm  \xC2\xB7  back=cancel");
            } else {
                float win_w = rmax.x - rmin.x;
                float bx = rmin.x + win_w * 0.46f;
                float by = ty + 1.f;
                float bw = win_w * 0.30f;
                float bh = 7.f;
                dl->AddRectFilled({bx, by}, {bx + bw, by + bh},
                                  menu_with_alpha(accent_color_, 50), 2.f);
                dl->AddRectFilled({bx, by}, {bx + bw * fill, by + bh},
                                  menu_with_alpha(accent_color_, 180), 2.f);
                ImVec2 vsz = ImGui::CalcTextSize(val_str);
                dl->AddText({rmax.x - vsz.x - 4.f, by - 1.f},
                            menu_with_alpha(accent_color_, 200), val_str);
            }

        // ── COLOR_PICKER ──────────────────────────────────────────────────────
        } else if (item.type == MenuItemType::COLOR_PICKER) {
            bool editing = selected && in_edit_mode_;

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item.label).c_str(), selected);

            if (!editing) {
                float sw_x = rmax.x - 36.f;
                float sw_y = ty;
                uint8_t pr = 128, pg = 128, pb = 128;
                if (item.color.get_color) {
                    auto [r, g, b] = item.color.get_color();
                    pr = r; pg = g; pb = b;
                }
                dl->AddRectFilled({sw_x, sw_y}, {sw_x + 28.f, sw_y + 14.f},
                                  IM_COL32(pr, pg, pb, 255), 2.f);
                dl->AddRect({sw_x, sw_y}, {sw_x + 28.f, sw_y + 14.f},
                            menu_with_alpha(accent_color_, 140), 2.f);
            } else {
                const float ch_vals[3] = { edit_r_, edit_g_, edit_b_ };
                const char* ch_names[3] = { "R", "G", "B" };
                const ImU32 ch_cols[3]  = {
                    IM_COL32(220, 60,  60,  200),
                    IM_COL32(60,  200, 60,  200),
                    IM_COL32(60,  80,  220, 200),
                };
                float bx = rmin.x + 4.f;
                float bw = (rmax.x - rmin.x) - 56.f;

                for (int c = 0; c < 3; c++) {
                    float by     = rmin.y + item_h + static_cast<float>(c) * 28.f;
                    float fill_c = ch_vals[c] / 255.f;
                    bool  is_sel    = (c == edit_channel_);
                    bool  is_active = is_sel && in_channel_edit_;
                    ImU32 text_col  = is_sel ? IM_COL32(255, 255, 255, 255)
                                             : IM_COL32(140, 170, 160, 200);
                    dl->AddText({bx, by + 5.f}, text_col, ch_names[c]);
                    float rx = bx + 16.f, rw = bw - 16.f;
                    dl->AddRectFilled({rx, by + 4.f}, {rx + rw, by + 16.f},
                                      menu_with_alpha(accent_color_, 50), 2.f);
                    ImU32 fill_col = is_active
                        ? (ch_cols[c] & 0x00FFFFFFu) | 0xFF000000u
                        : ch_cols[c];
                    dl->AddRectFilled({rx, by + 4.f}, {rx + rw * fill_c, by + 16.f},
                                      fill_col);
                    if (is_sel)
                        dl->AddRect({rx - 1.f, by + 3.f}, {rx + rw + 1.f, by + 17.f},
                                    menu_with_alpha(accent_color_, 200), 2.f);
                    char cv[8]; snprintf(cv, sizeof(cv), "%.0f", ch_vals[c]);
                    ImVec2 vsz = ImGui::CalcTextSize(cv);
                    dl->AddText({rmax.x - vsz.x - 6.f, by + 4.f}, text_col, cv);
                }

                float hint_y = rmin.y + item_h + 3 * 28.f + 2.f;
                const char* hint = !in_channel_edit_
                    ? "knob=channel  \xC2\xB7  select=edit  \xC2\xB7  back=cancel"
                    : "knob adjusts  \xC2\xB7  select=next  \xC2\xB7  back=cancel";
                dl->AddText({bx, hint_y}, menu_with_alpha(accent_color_, 180), hint);

                float sw_y = hint_y + 16.f;
                dl->AddRectFilled({bx, sw_y}, {bx + 52.f, sw_y + 16.f},
                                  IM_COL32(static_cast<uint8_t>(edit_r_),
                                           static_cast<uint8_t>(edit_g_),
                                           static_cast<uint8_t>(edit_b_), 255), 3.f);
                dl->AddRect({bx, sw_y}, {bx + 52.f, sw_y + 16.f},
                            menu_with_alpha(accent_color_, 150), 3.f);
            }

        // ── LEAF / SUBMENU ────────────────────────────────────────────────────
        } else {
            std::string label = to_upper(item.label);
            if (item.type == MenuItemType::SUBMENU || !item.children.empty())
                label += "   >";
            draw_item_text({rmin.x + 4.f, ty}, label.c_str(), selected);

            // Legacy radio indicator for items that still carry get_state
            if (item.get_state) {
                bool on = item.get_state();
                const float r  = 5.f;
                const float cx = rmax.x - 10.f;
                const float cy = rmin.y + item_h * 0.5f;
                if (on) {
                    dl->AddCircleFilled({cx, cy}, r,    accent_color_);
                    dl->AddCircleFilled({cx, cy}, 2.5f, IM_COL32(255, 255, 255, 255));
                } else {
                    dl->AddCircle({cx, cy}, r, menu_with_alpha(accent_color_, 60), 0, 1.5f);
                }
            }
        }

        // Thin bottom separator
        dl->AddLine({rmin.x - pad_x, rmax.y - 1.f},
                    {rmax.x + pad_x, rmax.y - 1.f}, COL_SEP_EFF, 1.f);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
}

#include "menu_system.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <utility>

static bool s_menu_glow = true;

static std::string to_upper(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (unsigned char c : s) r += static_cast<char>(::toupper(c));
    return r;
}

// Rendered label for an item: dynamic label_fn() if present, else the static
// label.  Lets profile rows show live names without rebuilding the menu tree.
static std::string item_label(const MenuItem& it) {
    if (it.label_fn) {
        std::string s = it.label_fn();
        if (!s.empty()) return s;
    }
    return it.label;
}

// On-screen keyboard layout (variable-width rows). Special keys live on the last
// row. Shared by the input (move/activate) and draw paths so they stay in sync.
static const std::vector<std::vector<std::string>>& osk_rows() {
    static const std::vector<std::vector<std::string>> rows = {
        {"1","2","3","4","5","6","7","8","9","0",","},
        {"Q","W","E","R","T","Y","U","I","O","P"},
        {"A","S","D","F","G","H","J","K","L"},
        {"Z","X","C","V","B","N","M"},
        {"SPACE","DEL","SAVE","CANCEL"},
    };
    return rows;
}

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

void MenuSystem::push_level(const std::vector<MenuItem>& items,
                             std::string panel_title,
                             MenuContextPanelDraw panel_draw) {
    if (items.empty()) return;
    std::vector<MenuItem> level = items;

    // Append navigation item: "Close Menu" at root, "< Back" in submenus.
    // When selected, back() pops the level (or closes at root depth=1).
    MenuItem nav;
    nav.type   = MenuItemType::LEAF;
    nav.label  = stack_.empty() ? "Close Menu" : "< Back";
    nav.action = [this] { this->back(); };
    level.push_back(nav);

    if (!stack_.empty())
        stack_.back().cursor = cursor_;  // remember where we were on the parent page

    // Start the cursor on the currently-selected option (so option lists open
    // highlighting the active choice), else on the first item.
    int init_cursor = 0;
    for (int i = 0; i < static_cast<int>(level.size()); ++i)
        if (level[i].get_state && level[i].get_state()) { init_cursor = i; break; }

    stack_.push_back(Level{ std::move(level), init_cursor,
                            std::move(panel_title),
                            std::move(panel_draw) });
    cursor_ = init_cursor;
    list_scroll_ = 0;        // start a fresh page at the top
    radial_prev_sel_ = -1;   // snap the radial spin to the new level
    emit_detents();
}

void MenuSystem::pop_level() {
    if (stack_.size() > 1) {
        stack_.pop_back();
        cursor_ = stack_.back().cursor;  // restore cursor to where user was on this page
        list_scroll_ = 0;                // recomputed from the cursor on next draw
        radial_prev_sel_ = -1;           // snap the radial spin to the restored level
        emit_detents();
    } else if (deep_open_) {
        close_deep();
    } else {
        close();
    }
}

// ── Deep (full-screen) menu management ──────────────────────────────────────────

void MenuSystem::build_deep_tabs() {
    deep_tabs_.clear();
    std::vector<MenuItem> general;
    for (const auto& it : root_items_) {
        if (it.type == MenuItemType::SUBMENU && !it.children.empty())
            deep_tabs_.emplace_back(it.label, it.children);
        else
            general.push_back(it);
    }
    if (!general.empty())
        deep_tabs_.emplace_back(std::string("General"), std::move(general));
}

void MenuSystem::load_tab(int idx) {
    if (deep_tabs_.empty()) return;
    int n = static_cast<int>(deep_tabs_.size());
    tab_index_ = ((idx % n) + n) % n;
    in_edit_mode_    = false;
    in_channel_edit_ = false;
    stack_.clear();
    cursor_ = 0;
    push_level(deep_tabs_[tab_index_].second);
}

void MenuSystem::open_deep() {
    build_deep_tabs();
    if (deep_tabs_.empty()) return;
    deep_open_ = true;
    open_      = true;     // so navigate/select/back operate on the stack
    load_tab(0);
}

void MenuSystem::close_deep() {
    deep_open_       = false;
    open_            = false;
    in_edit_mode_    = false;
    in_channel_edit_ = false;
    osk_active_      = false;
    osk_commit_      = nullptr;
    stack_.clear();
}

void MenuSystem::next_tab() {
    // Switch tabs from anywhere in the deep menu — load_tab() drops back to the
    // tab's base level and cancels any edit mode.
    if (!deep_open_) return;
    load_tab(tab_index_ + 1);
}

void MenuSystem::prev_tab() {
    if (!deep_open_) return;
    load_tab(tab_index_ - 1);
}

// ── On-screen keyboard ──────────────────────────────────────────────────────────

void MenuSystem::open_keyboard(std::string title, std::string initial,
                               KeyboardCommit on_commit) {
    osk_title_  = std::move(title);
    osk_text_   = std::move(initial);
    osk_commit_ = std::move(on_commit);
    osk_row_    = 0;
    osk_col_    = 0;
    osk_active_ = true;
}

void MenuSystem::close_keyboard() {
    osk_active_ = false;
    osk_commit_ = nullptr;
    osk_text_.clear();
}

void MenuSystem::osk_move(int dx, int dy) {
    if (!osk_active_) return;
    const auto& rows = osk_rows();
    int nrows = static_cast<int>(rows.size());
    if (nrows == 0) return;
    osk_row_ = std::clamp(osk_row_ + dy, 0, nrows - 1);
    int ncols = static_cast<int>(rows[osk_row_].size());
    osk_col_ = std::clamp(osk_col_ + dx, 0, std::max(0, ncols - 1));
}

void MenuSystem::osk_step(int d) {
    if (!osk_active_) return;
    const auto& rows = osk_rows();
    // Flatten current (row,col) to a linear index, step with wrap, unflatten.
    int flat = 0, total = 0;
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        if (r < osk_row_) flat += static_cast<int>(rows[r].size());
        total += static_cast<int>(rows[r].size());
    }
    flat += std::min(osk_col_, static_cast<int>(rows[osk_row_].size()) - 1);
    if (total == 0) return;
    flat = ((flat + d) % total + total) % total;
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        int sz = static_cast<int>(rows[r].size());
        if (flat < sz) { osk_row_ = r; osk_col_ = flat; return; }
        flat -= sz;
    }
}

void MenuSystem::osk_input_char(unsigned int c) {
    if (!osk_active_) return;
    if (c == ' ' || c == '-' || c == '_' || c == ',' || c == '#' ||
        std::isalnum(static_cast<int>(c))) {
        if (osk_text_.size() < 40) osk_text_ += static_cast<char>(c);
    }
}

void MenuSystem::osk_activate() {
    if (!osk_active_) return;
    const auto& rows = osk_rows();
    if (osk_row_ < 0 || osk_row_ >= static_cast<int>(rows.size())) return;
    const auto& row = rows[osk_row_];
    if (osk_col_ < 0 || osk_col_ >= static_cast<int>(row.size())) return;
    const std::string& k = row[osk_col_];
    if      (k == "SPACE")  { if (osk_text_.size() < 40) osk_text_ += ' '; }
    else if (k == "DEL")    osk_backspace();
    else if (k == "SAVE")   osk_commit();
    else if (k == "CANCEL") osk_cancel();
    else if (k.size() == 1) { if (osk_text_.size() < 40) osk_text_ += k[0]; }
}

void MenuSystem::osk_backspace() {
    if (!osk_active_) return;
    if (!osk_text_.empty()) osk_text_.pop_back();
    else                    osk_cancel();   // backspace on empty = cancel out
}

void MenuSystem::osk_commit() {
    if (!osk_active_) return;
    // trim surrounding whitespace
    std::string t = osk_text_;
    size_t b = t.find_first_not_of(' ');
    size_t e = t.find_last_not_of(' ');
    std::string name = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
    KeyboardCommit cb = osk_commit_;   // copy before close clears it
    close_keyboard();
    if (!name.empty() && cb) cb(name);
}

void MenuSystem::osk_cancel() {
    close_keyboard();
}

// ── File picker ───────────────────────────────────────────────────────────────

void MenuSystem::open_file_picker(std::string title,
                                  std::string start_dir,
                                  std::vector<std::string> extensions,
                                  std::function<void(const std::string&)> on_commit,
                                  std::function<void()> on_cancel) {
    // Make sure mutually-exclusive overlays don't stack.
    if (osk_active_) close_keyboard();
    file_picker_.open(std::move(title), std::move(start_dir),
                      std::move(extensions),
                      std::move(on_commit), std::move(on_cancel));
}

void MenuSystem::close_file_picker() {
    file_picker_.cancel();
}

// ── Face editor ───────────────────────────────────────────────────────────────

void MenuSystem::open_face_editor(std::string title,
                                  std::string abs_path,
                                  int canvas_w, int canvas_h,
                                  std::vector<cv::Rect> covered_regions,
                                  std::vector<std::string> covered_labels,
                                  int mirror_axis_x,
                                  menu::FaceEditor::Mode mode,
                                  std::vector<uint32_t> palette,
                                  menu::FaceEditor::CommitFn on_commit,
                                  menu::FaceEditor::CancelFn on_cancel,
                                  menu::FaceEditor::PreviewFn on_preview,
                                  menu::FaceEditor::LiveFrameFn live_frame,
                                  double preview_duration_s) {
    // Mutually-exclusive overlays — bring others down first.
    if (osk_active_)              close_keyboard();
    if (file_picker_.is_open())   file_picker_.cancel();
    face_editor_.open(std::move(title), std::move(abs_path),
                      canvas_w, canvas_h,
                      std::move(covered_regions),
                      std::move(covered_labels),
                      mirror_axis_x,
                      mode, std::move(palette),
                      std::move(on_commit), std::move(on_cancel),
                      std::move(on_preview), std::move(live_frame),
                      preview_duration_s);
}

void MenuSystem::close_face_editor() {
    face_editor_.back();
}

void MenuSystem::emit_detents() {
    if (detent_cb_ && !stack_.empty()) {
        int count = 0;
        for (const auto& it : stack_.back().items)
            if (!it.visible_fn || it.visible_fn()) ++count;
        detent_cb_(count);
    }
}

void MenuSystem::emit_detents_override(int count) {
    if (detent_cb_) detent_cb_(count);
}

// ── navigate ──────────────────────────────────────────────────────────────────

void MenuSystem::navigate(int direction) {
    if (face_editor_.is_open()) { face_editor_.cursor_step(0, direction); return; }
    if (file_picker_.is_open()) { file_picker_.step(direction); return; }
    if (osk_active_) { osk_step(direction); return; }
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
                // 0/1/2 = R/G/B dial bars, 3 = Hex text entry, 4 = RGB text entry.
                edit_channel_ = ((edit_channel_ + direction) % 5 + 5) % 5;
            }
        } else if (item.type == MenuItemType::SLIDER) {
            edit_float_ = std::clamp(
                edit_float_ + static_cast<float>(direction) * item.slider.step,
                item.slider.min, item.slider.max);
            // Live preview — apply immediately so the user hears/sees the change
            if (item.slider.set_value) item.slider.set_value(edit_float_);
        } else if (item.type == MenuItemType::FACE_PICKER) {
            int n   = item.face_picker.face_count;
            int val = ((static_cast<int>(edit_float_) + direction) % n + n) % n;
            edit_float_ = static_cast<float>(val);
            if (item.face_picker.set_face) item.face_picker.set_face(val);
        }
        return;
    }

    const auto& items = stack_.back().items;
    int n = static_cast<int>(items.size());
    if (n == 0) return;

    // Advance cursor, skipping invisible items (up to n steps to avoid infinite loop).
    int next = ((cursor_ + direction) % n + n) % n;
    for (int tries = 0; tries < n; ++tries) {
        const auto& it = items[next];
        if (!it.visible_fn || it.visible_fn()) break;
        next = ((next + direction) % n + n) % n;
    }
    cursor_ = next;

    // Live preview: apply the newly-highlighted option's effect as the user tabs.
    if (items[cursor_].on_highlight) items[cursor_].on_highlight();
}

// ── select ────────────────────────────────────────────────────────────────────

void MenuSystem::select() {
    if (face_editor_.is_open()) { face_editor_.primary(); return; }
    if (file_picker_.is_open()) { file_picker_.activate(); return; }
    if (osk_active_) { osk_activate(); return; }
    if (!open_ || stack_.empty()) return;
    auto& items = stack_.back().items;
    if (cursor_ >= static_cast<int>(items.size())) return;
    auto& item = items[cursor_];

    switch (item.type) {
    case MenuItemType::SUBMENU:
        if (!item.children.empty()) {
            if (item.action) item.action();   // optional "on enter" hook (e.g. lock focus)
            push_level(item.children,
                       item.context_panel_title,
                       item.context_panel_draw);
        }
        break;

    case MenuItemType::LEAF:
        // Copy the callable before invoking: an action may pop its own level
        // (e.g. the GPIO pin picker calls back() on select), which would destroy
        // the items vector that owns this std::function mid-call. The local copy
        // keeps it alive for the duration of the call.
        if (item.action) { auto act = item.action; act(); }
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

    case MenuItemType::FACE_PICKER:
        if (!in_edit_mode_) {
            edit_float_   = item.face_picker.get_face
                            ? static_cast<float>(item.face_picker.get_face()) : 0.f;
            orig_float_   = edit_float_;
            in_edit_mode_ = true;
            emit_detents_override(item.face_picker.face_count);
        } else {
            if (item.face_picker.set_face)
                item.face_picker.set_face(static_cast<int>(edit_float_));
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
            emit_detents_override(5);   // R, G, B, Hex, RGB
        } else if (!in_channel_edit_) {
            if (edit_channel_ >= 3) {
                // Hex / RGB rows → open the on-screen keyboard for text entry.
                // Capture the set_color callback by value so it survives while
                // the keyboard is up; the commit parses the typed value, updates
                // the working channels, and applies the colour live.
                auto setc = item.color.set_color;
                // On a valid parse: apply, finalise (so a later "back" doesn't
                // revert it via the orig snapshot), and leave edit mode. On a
                // bad parse: keep the picker open so the user can retry.
                auto commit_typed = [this, setc](int r, int g, int b){
                    edit_r_ = (float)r; edit_g_ = (float)g; edit_b_ = (float)b;
                    orig_r_ = edit_r_;  orig_g_ = edit_g_;  orig_b_ = edit_b_;
                    if (setc) setc((uint8_t)r, (uint8_t)g, (uint8_t)b);
                    in_channel_edit_ = false;
                    in_edit_mode_    = false;
                    emit_detents();
                };
                char cur[24];
                if (edit_channel_ == 3) {
                    std::snprintf(cur, sizeof(cur), "%02X%02X%02X",
                                  static_cast<int>(edit_r_),
                                  static_cast<int>(edit_g_),
                                  static_cast<int>(edit_b_));
                    open_keyboard("Hex Color (RRGGBB)", cur,
                        [commit_typed](const std::string& s){
                            int r, g, b;
                            if (cp_parse_hex(s, r, g, b)) commit_typed(r, g, b);
                        });
                } else {
                    std::snprintf(cur, sizeof(cur), "%d,%d,%d",
                                  static_cast<int>(edit_r_),
                                  static_cast<int>(edit_g_),
                                  static_cast<int>(edit_b_));
                    open_keyboard("Color R,G,B (0-255)", cur,
                        [commit_typed](const std::string& s){
                            int r, g, b;
                            if (cp_parse_rgb(s, r, g, b)) commit_typed(r, g, b);
                        });
                }
            } else {
                in_channel_edit_ = true;
                emit_detents_override(256);
            }
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
                emit_detents_override(5);
            }
        }
        break;

    case MenuItemType::NOTIF_LOG:
        if (item.notif_log.queue) {
            if (item.notif_log.filter) {
                // Filtered browser: clear only the matching notifications.
                for (auto& n : item.notif_log.queue->items)
                    if (!n.dismissed && item.notif_log.filter(n)) { n.dismissed = true; n.read = true; }
            } else {
                item.notif_log.queue->dismiss_all();
            }
        }
        break;
    }
}

// ── back ──────────────────────────────────────────────────────────────────────

void MenuSystem::back() {
    if (face_editor_.is_open()) { face_editor_.back(); return; }
    if (file_picker_.is_open()) { file_picker_.back(); return; }
    if (osk_active_) { osk_backspace(); return; }
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
            } else if (item.type == MenuItemType::FACE_PICKER) {
                if (item.face_picker.set_face)
                    item.face_picker.set_face(static_cast<int>(orig_float_));
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

bool MenuSystem::editing_value() const {
    if (!in_edit_mode_ || stack_.empty()) return false;
    const auto& items = stack_.back().items;
    if (cursor_ < 0 || cursor_ >= static_cast<int>(items.size())) return false;
    switch (items[cursor_].type) {
        case MenuItemType::SLIDER:       return true;
        case MenuItemType::FACE_PICKER:  return true;
        case MenuItemType::COLOR_PICKER: return in_channel_edit_;  // only while a channel is being adjusted
        default:                         return false;
    }
}

// ── draw ──────────────────────────────────────────────────────────────────────

void MenuSystem::draw(int screen_w, int screen_h) {
    if (!open_ || stack_.empty()) return;
    s_menu_glow = glow_enabled_;

    const auto& items  = stack_.back().items;
    const float item_h = 38.f;
    const float pad_x  = 18.f;
    const float pad_y  = 14.f;
    const float width  = 380.f;

    // Snap cursor off any newly-hidden item before doing any rendering.
    {
        int n = static_cast<int>(items.size());
        if (n > 0) {
            const auto& cur_item = items[cursor_];
            if (cur_item.visible_fn && !cur_item.visible_fn()) {
                for (int tries = 0; tries < n; ++tries) {
                    cursor_ = (cursor_ + 1) % n;
                    const auto& t = items[cursor_];
                    if (!t.visible_fn || t.visible_fn()) break;
                }
            }
        }
    }

    // Count only visible items for layout, and locate the cursor among them.
    int visible_count = 0, vis_cursor = 0;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (items[i].visible_fn && !items[i].visible_fn()) continue;
        if (i == cursor_) vis_cursor = visible_count;
        ++visible_count;
    }

    // Extra height for expanded editing rows
    float extra = 0.f;
    if (in_edit_mode_ && cursor_ < static_cast<int>(items.size())) {
        const auto& sel = items[cursor_];
        if (sel.type == MenuItemType::SLIDER)       extra = 30.f;
        if (sel.type == MenuItemType::FACE_PICKER)  extra = 90.f;
        if (sel.type == MenuItemType::COLOR_PICKER) extra = 152.f;
        if (sel.type == MenuItemType::NOTIF_LOG)    extra = 200.f;
    }

    // ── Vertical scrolling when a level overflows the screen ─────────────────
    // Fit as many rows as the viewport (screen minus margins + the expanded
    // editing row's extra height) allows, then window the rows around the
    // cursor. list_scroll_ is the first visible row (in visible-item space).
    const float margin  = 48.f;
    const float avail_h = static_cast<float>(screen_h) - margin * 2.f - pad_y * 2.f;
    int max_rows = static_cast<int>((avail_h - extra) / item_h);
    if (max_rows < 1) max_rows = 1;
    const bool scrolling = visible_count > max_rows;
    const int  shown     = scrolling ? max_rows : visible_count;
    if (scrolling) {
        if (vis_cursor < list_scroll_)             list_scroll_ = vis_cursor;
        if (vis_cursor >= list_scroll_ + max_rows) list_scroll_ = vis_cursor - max_rows + 1;
        if (list_scroll_ > visible_count - max_rows) list_scroll_ = visible_count - max_rows;
        if (list_scroll_ < 0)                      list_scroll_ = 0;
    } else {
        list_scroll_ = 0;
    }

    const float total_h = pad_y * 2.f
                        + item_h * static_cast<float>(shown)
                        + extra;
    float x, y;
    switch (anchor_) {
        default:
        case MenuAnchor::TopLeft:     x = margin;                          y = margin;                          break;
        case MenuAnchor::TopRight:    x = screen_w - width - margin;       y = margin;                          break;
        case MenuAnchor::BottomLeft:  x = margin;                          y = screen_h - total_h - margin;     break;
        case MenuAnchor::BottomRight: x = screen_w - width - margin;       y = screen_h - total_h - margin;     break;
    }

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
    // wp/ws/GAP hoisted so the item loop can use them for highlight clipping.
    const ImVec2 wp  = ImGui::GetWindowPos();
    const ImVec2 ws  = ImGui::GetWindowSize();
    const float  GAP = 3.f + border_thickness_ * 0.5f;
    {
        constexpr float C = 8.f;  // chamfer distance (corner cut)

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
    const bool bold_text = bold_text_ || !bg_enabled_;
    auto draw_item_text = [&](ImVec2 pos, const char* text, bool sel) {
        if (filled_row && sel) {
            dl->AddText({pos.x - 0.6f, pos.y}, IM_COL32(0, 0, 0, 255), text);
            dl->AddText({pos.x + 0.6f, pos.y}, IM_COL32(0, 0, 0, 255), text);
            dl->AddText(pos,                   IM_COL32(0, 0, 0, 255), text);
        } else {
            if (bold_text) {
                // Extra shifted pass simulates bold when there is no bg for contrast
                const ImU32 fill = sel ? IM_COL32(255,255,255,255) : IM_COL32(255,255,255,160);
                dl->AddText({pos.x + 0.7f, pos.y}, fill, text);
            }
            draw_glow_text(dl, pos, text, sel, accent_color_);
        }
    };

    int vi = -1;        // running index among visible items
    int drawn = 0;      // rows actually emitted this frame
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        const auto& item = items[i];
        if (item.visible_fn && !item.visible_fn()) continue;
        ++vi;
        if (vi < list_scroll_) continue;   // scrolled above the viewport
        if (drawn >= shown)    break;      // past the bottom of the viewport
        ++drawn;

        bool selected = (i == cursor_);

        // Row height: expanded for the selected item in edit mode
        float row_h = item_h - 1.f;
        if (selected && in_edit_mode_) {
            if (item.type == MenuItemType::SLIDER)       row_h = item_h + 25.f;
            if (item.type == MenuItemType::FACE_PICKER)  row_h = item_h + 85.f;
            if (item.type == MenuItemType::COLOR_PICKER) row_h = item_h + 151.f;
            if (item.type == MenuItemType::NOTIF_LOG)    row_h = item_h + 195.f;
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
                dl->AddRectFilled({wp.x + GAP,        rmin.y},
                                  {wp.x + ws.x - GAP, rmax.y}, IM_COL32(255, 255, 255, 235));
            else
                dl->AddRectFilled({rmin.x - pad_x,       rmin.y},
                                  {rmin.x - pad_x + 4.f, rmax.y}, accent_color_);
        }

        // Text Y position (vertically centered in the base item_h row)
        float ty = rmin.y + (item_h - line_h) * 0.5f - 0.5f;

        // ── TOGGLE ────────────────────────────────────────────────────────────
        if (item.type == MenuItemType::TOGGLE) {
            bool on = item.get_toggle ? item.get_toggle() : false;

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item_label(item)).c_str(), selected);

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

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item_label(item)).c_str(), selected);

            const bool inv = (filled_row && selected);

            if (editing) {
                float bx = rmin.x + 4.f;
                float by = rmin.y + item_h - 2.f;
                float bw = (rmax.x - rmin.x) - 64.f;
                float bh = 10.f;
                dl->AddRectFilled({bx, by}, {bx + bw, by + bh},
                                  inv ? IM_COL32(0,0,0,60) : menu_with_alpha(accent_color_, 60), 3.f);
                dl->AddRectFilled({bx, by}, {bx + bw * fill, by + bh},
                                  inv ? IM_COL32(0,0,0,220) : menu_with_alpha(accent_color_, 220), 3.f);
                float tick_x = bx + bw * fill;
                dl->AddLine({tick_x, by - 2.f}, {tick_x, by + bh + 2.f},
                            inv ? IM_COL32(0,0,0,200) : IM_COL32(255, 255, 255, 200), 2.f);
                dl->AddText({bx + bw + 6.f, by},
                            inv ? IM_COL32(0,0,0,255) : IM_COL32(255, 255, 255, 255), val_str);
                dl->AddText({bx, by - 14.f},
                            inv ? IM_COL32(0,0,0,180) : menu_with_alpha(accent_color_, 180),
                            "knob  \xC2\xB7  select=confirm  \xC2\xB7  back=cancel");
            } else {
                float win_w = rmax.x - rmin.x;
                float bx = rmin.x + win_w * 0.46f;
                float by = ty + 1.f;
                float bw = win_w * 0.30f;
                float bh = 7.f;
                dl->AddRectFilled({bx, by}, {bx + bw, by + bh},
                                  inv ? IM_COL32(0,0,0,50) : menu_with_alpha(accent_color_, 50), 2.f);
                dl->AddRectFilled({bx, by}, {bx + bw * fill, by + bh},
                                  inv ? IM_COL32(0,0,0,180) : menu_with_alpha(accent_color_, 180), 2.f);
                ImVec2 vsz = ImGui::CalcTextSize(val_str);
                dl->AddText({rmax.x - vsz.x - 4.f, by - 1.f},
                            inv ? IM_COL32(0,0,0,200) : menu_with_alpha(accent_color_, 200), val_str);
            }

        // ── FACE_PICKER ───────────────────────────────────────────────────────
        } else if (item.type == MenuItemType::FACE_PICKER) {
            bool editing  = selected && in_edit_mode_;
            int  cur_face = editing
                ? static_cast<int>(edit_float_)
                : (item.face_picker.get_face ? item.face_picker.get_face() : 0);

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item_label(item)).c_str(), selected);

            const bool inv = (filled_row && selected);

            if (!editing) {
                char val_str[8]; snprintf(val_str, sizeof(val_str), "%d", cur_face);
                ImVec2 vsz = ImGui::CalcTextSize(val_str);
                dl->AddText({rmax.x - vsz.x - 4.f, ty},
                            inv ? IM_COL32(0,0,0,200) : menu_with_alpha(accent_color_, 200), val_str);
            } else {
                int   n   = item.face_picker.face_count;
                float cx  = rmin.x + (rmax.x - rmin.x) * 0.5f;
                float cy  = rmin.y + item_h + 36.f;
                float rad = std::min(30.f, (rmax.x - rmin.x) * 0.5f - 22.f);

                for (int f = 0; f < n; f++) {
                    float angle  = -static_cast<float>(M_PI) * 0.5f
                                   + (2.f * static_cast<float>(M_PI) * f) / static_cast<float>(n);
                    float fx     = cx + rad * std::cos(angle);
                    float fy     = cy + rad * std::sin(angle);
                    bool  is_sel = (f == cur_face);
                    if (is_sel) {
                        dl->AddCircleFilled({fx, fy}, 7.f, menu_with_alpha(accent_color_, 220));
                        dl->AddCircleFilled({fx, fy}, 2.5f, IM_COL32(255, 255, 255, 220));
                    } else {
                        dl->AddCircle({fx, fy}, 4.5f, menu_with_alpha(accent_color_, 110), 0, 1.5f);
                    }
                }

                // Current face index in the center of the ring
                char cnum[4]; snprintf(cnum, sizeof(cnum), "%d", cur_face);
                ImVec2 csz = ImGui::CalcTextSize(cnum);
                dl->AddText({cx - csz.x * 0.5f, cy - csz.y * 0.5f},
                            IM_COL32(255, 255, 255, 200), cnum);

                float hint_y = cy + rad + 10.f;
                dl->AddText({rmin.x + 4.f, hint_y},
                            menu_with_alpha(accent_color_, 180),
                            "knob  \xC2\xB7  select=confirm  \xC2\xB7  back=cancel");
            }

        // ── COLOR_PICKER ──────────────────────────────────────────────────────
        } else if (item.type == MenuItemType::COLOR_PICKER) {
            bool editing = selected && in_edit_mode_;

            draw_item_text({rmin.x + 4.f, ty}, to_upper(item_label(item)).c_str(), selected);

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

                // Text-entry rows: HEX / RGB — select to open the keyboard.
                char trow_val[2][24];
                snprintf(trow_val[0], sizeof(trow_val[0]), "%02X%02X%02X",
                         static_cast<int>(edit_r_), static_cast<int>(edit_g_),
                         static_cast<int>(edit_b_));
                snprintf(trow_val[1], sizeof(trow_val[1]), "%d,%d,%d",
                         static_cast<int>(edit_r_), static_cast<int>(edit_g_),
                         static_cast<int>(edit_b_));
                const char* trow_name[2] = { "HEX", "RGB" };
                for (int t = 0; t < 2; t++) {
                    float by      = rmin.y + item_h + static_cast<float>(3 + t) * 28.f;
                    bool  is_sel  = (edit_channel_ == 3 + t);
                    ImU32 tcol    = is_sel ? IM_COL32(255, 255, 255, 255)
                                           : IM_COL32(140, 170, 160, 200);
                    dl->AddText({bx, by + 5.f}, tcol, trow_name[t]);
                    dl->AddText({bx + 40.f, by + 5.f}, tcol, trow_val[t]);
                    if (is_sel)
                        dl->AddRect({bx - 1.f, by + 3.f}, {rmax.x - 6.f, by + 17.f},
                                    menu_with_alpha(accent_color_, 200), 2.f);
                }

                float hint_y = rmin.y + item_h + 5 * 28.f + 2.f;
                const char* hint = !in_channel_edit_
                    ? "knob=row  \xC2\xB7  select=edit/type  \xC2\xB7  back=cancel"
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

        // ── NOTIF_LOG ─────────────────────────────────────────────────────────
        } else if (item.type == MenuItemType::NOTIF_LOG) {
            // Label row (unread badge)
            NotificationQueue* q = item.notif_log.queue;
            int unread = q ? q->unread_count() : 0;
            std::string lbl = to_upper(item_label(item));
            if (unread > 0) { char badge[16]; snprintf(badge, sizeof(badge), " (%d)", unread); lbl += badge; }
            draw_item_text({rmin.x + 4.f, ty}, lbl.c_str(), selected);

            if (selected && q) {
                // Scrollable notification list below the label row
                float ly = rmin.y + item_h + 4.f;
                const float list_h = 190.f;
                const float lh_row = 36.f;
                constexpr int kMaxShow = 5;
                int shown = 0;
                for (auto& n : q->items) {
                    if (n.dismissed) continue;
                    if (item.notif_log.filter && !item.notif_log.filter(n)) continue;
                    if (shown >= kMaxShow) break;
                    if (ly + lh_row > rmin.y + item_h + list_h) break;

                    // Type badge color
                    ImU32 tc;
                    switch (n.type) {
                        case NotifType::Alarm: tc = IM_COL32(255, 60, 60, 255); break;
                        case NotifType::Timer: tc = IM_COL32(255,160, 32, 255); break;
                        case NotifType::LoRa:  tc = IM_COL32(  0,180,255, 255); break;
                        default:               tc = IM_COL32( 80,140,255, 255); break;
                    }
                    // Row bg
                    dl->AddRectFilled({rmin.x + 2.f, ly}, {rmax.x - 2.f, ly + lh_row - 2.f},
                                      IM_COL32(15, 20, 25, 200), 3.f);
                    dl->AddRectFilled({rmin.x + 2.f, ly}, {rmin.x + 4.f, ly + lh_row - 2.f}, tc, 0.f);
                    // Title + time
                    char ts[8]; time_t t = (time_t)n.timestamp;
                    struct tm* tm = localtime(&t);
                    strftime(ts, sizeof(ts), "%H:%M", tm);
                    dl->AddText({rmin.x + 8.f, ly + 3.f}, IM_COL32(255,255,255,220), n.title.c_str());
                    ImVec2 tsz = ImGui::CalcTextSize(ts);
                    dl->AddText({rmax.x - tsz.x - 4.f, ly + 3.f}, IM_COL32(160,160,160,180), ts);
                    // Body
                    if (!n.body.empty())
                        dl->AddText({rmin.x + 8.f, ly + 18.f}, IM_COL32(180,180,180,180), n.body.substr(0,40).c_str());
                    ly += lh_row;
                    ++shown;
                    n.read = true;
                }
                if (shown == 0) {
                    dl->AddText({rmin.x + 8.f, rmin.y + item_h + 10.f},
                                menu_with_alpha(accent_color_, 120), "No notifications");
                }
                // "Clear All" hint
                dl->AddText({rmin.x + 4.f, rmin.y + item_h + list_h + 2.f},
                            menu_with_alpha(accent_color_, 120),
                            "select = clear all");
            }

        // ── LEAF / SUBMENU ────────────────────────────────────────────────────
        } else {
            std::string label = to_upper(item_label(item));
            if (item.type == MenuItemType::SUBMENU || !item.children.empty())
                label += "   >";
            // Warning rows (e.g. a GPIO slot/pin conflict) render in red instead
            // of the usual glow/accent text so the problem stands out.
            if (item.warn_fn && item.warn_fn()) {
                const ImU32 rc = (filled_row && selected) ? IM_COL32(185, 25, 15, 255)
                                                          : IM_COL32(240, 95, 80, 255);
                if (bold_text) dl->AddText({rmin.x + 4.7f, ty}, rc, label.c_str());
                dl->AddText({rmin.x + 4.f, ty}, rc, label.c_str());
            } else {
                draw_item_text({rmin.x + 4.f, ty}, label.c_str(), selected);
            }

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

    // ── Scroll chevrons ──────────────────────────────────────────────────────
    // Drawn in the top / bottom padding bands when more rows exist off-screen.
    if (scrolling) {
        const float cx  = wp.x + ws.x * 0.5f;
        const ImU32 col = menu_with_alpha(accent_color_, 210);
        if (list_scroll_ > 0) {
            const float yc = wp.y + pad_y * 0.5f;
            dl->AddTriangleFilled({cx - 6.f, yc + 3.f}, {cx + 6.f, yc + 3.f},
                                  {cx, yc - 3.f}, col);
        }
        if (list_scroll_ + shown < visible_count) {
            const float yc = wp.y + ws.y - pad_y * 0.5f;
            dl->AddTriangleFilled({cx - 6.f, yc - 3.f}, {cx + 6.f, yc - 3.f},
                                  {cx, yc + 3.f}, col);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);

    // Side-panel: rendered as a separate window mirroring the menu anchor.
    // Skipped when the current level didn't register one.
    if (!stack_.empty() && stack_.back().panel_draw)
        draw_context_panel(stack_.back(), screen_w, screen_h, x, y, width, total_h);
}

// ── draw_context_panel ────────────────────────────────────────────────────────

void MenuSystem::draw_context_panel(const Level& lvl,
                                     int screen_w, int screen_h,
                                     float menu_x, float menu_y,
                                     float menu_w, float menu_h) {
    constexpr float kPanelW = 340.f;
    constexpr float kPanelH = 240.f;
    constexpr float kGap    = 18.f;
    constexpr float kPadX   = 12.f;
    constexpr float kPadY   = 28.f;   // extra top padding for title

    // Place panel on the opposite horizontal side of the menu, aligned to the
    // menu's top edge.  Clamp inside the screen so it never falls off-edge.
    float px;
    switch (anchor_) {
        default:
        case MenuAnchor::TopLeft:
        case MenuAnchor::BottomLeft:
            px = menu_x + menu_w + kGap;
            if (px + kPanelW > screen_w - 16.f)
                px = screen_w - kPanelW - 16.f;
            break;
        case MenuAnchor::TopRight:
        case MenuAnchor::BottomRight:
            px = menu_x - kPanelW - kGap;
            if (px < 16.f) px = 16.f;
            break;
    }
    float py = menu_y;
    if (py + kPanelH > screen_h - 16.f)
        py = screen_h - kPanelH - 16.f;
    (void)menu_h;

    ImGui::SetNextWindowPos ({ px, py }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ kPanelW, kPanelH }, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.f, 0.f));

    ImGui::Begin("##menu_panel", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();

    // Chamfered background + border, same visual language as the main menu.
    {
        constexpr float C   = 8.f;
        const float GAP     = 3.f + border_thickness_ * 0.5f;
        const ImVec2 bmin   = { wp.x + GAP,        wp.y + GAP };
        const ImVec2 bmax   = { wp.x + ws.x - GAP, wp.y + ws.y - GAP };
        const ImVec2 bg_pts[8] = {
            {bmin.x + C, bmin.y}, {bmax.x - C, bmin.y},
            {bmax.x, bmin.y + C}, {bmax.x, bmax.y - C},
            {bmax.x - C, bmax.y}, {bmin.x + C, bmax.y},
            {bmin.x, bmax.y - C}, {bmin.x, bmin.y + C},
        };
        const ImU32 bg_col = bg_enabled_ ? bg_color_ : IM_COL32(0, 0, 0, 0);
        dl->AddConvexPolyFilled(bg_pts, 8, bg_col);

        if (border_enabled_) {
            const ImVec2 emin = { wp.x,        wp.y };
            const ImVec2 emax = { wp.x + ws.x, wp.y + ws.y };
            const ImVec2 bdr_pts[8] = {
                {emin.x + C, emin.y}, {emax.x - C, emin.y},
                {emax.x, emin.y + C}, {emax.x, emax.y - C},
                {emax.x - C, emax.y}, {emin.x + C, emax.y},
                {emin.x, emax.y - C}, {emin.x, emin.y + C},
            };
            dl->AddPolyline(bdr_pts, 8,
                            (border_color_ & 0x00FFFFFFu) | (220u << 24),
                            ImDrawFlags_Closed, border_thickness_);
        }
    }

    // Title bar
    if (!lvl.panel_title.empty()) {
        const std::string upper = to_upper(lvl.panel_title);
        dl->AddText({ wp.x + kPadX, wp.y + 6.f },
                    IM_COL32(255, 255, 255, 230), upper.c_str());
        dl->AddLine({ wp.x + kPadX,           wp.y + 22.f },
                    { wp.x + ws.x - kPadX,    wp.y + 22.f },
                    (accent_color_ & 0x00FFFFFFu) | (80u << 24), 1.f);
    }

    // Content area, post-padding.
    const ImVec2 origin = { wp.x + kPadX, wp.y + kPadY };
    const ImVec2 size   = { ws.x - 2 * kPadX, ws.y - kPadY - 8.f };
    if (lvl.panel_draw) lvl.panel_draw(dl, origin, size);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(1);
}

// ── draw_fullscreen (deep menu) ─────────────────────────────────────────────────

void MenuSystem::draw_fullscreen(int screen_w, int screen_h) {
    // Also render when only the on-screen keyboard is up (it takes over the
    // screen below), so text entry works from the corner/radial quick menu too.
    if ((!deep_open_ && !osk_active_) || stack_.empty()) return;
    s_menu_glow = glow_enabled_;

    const float W = static_cast<float>(screen_w);
    const float H = static_cast<float>(screen_h);

    ImGui::SetNextWindowPos ({ 0.f, 0.f }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ W, H },    ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.f, 0.f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration        |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoSavedSettings     |
        ImGuiWindowFlags_NoFocusOnAppearing  |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoInputs;

    ImGui::Begin("##deepmenu", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font   = ImGui::GetFont();
    // Base font size scaled by the theme's UI scale — everything in this method
    // derives from fs, so this resizes the whole deep menu (text + spacing).
    const float fs = ImGui::GetFontSize() * ui_scale_;

    // On-screen keyboard takes over the whole screen when active.
    if (osk_active_) {
        draw_keyboard(dl, font, fs, W, H);
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(1);
        return;
    }

    // Face editor overlays the deep menu. Drawn ahead of the picker so a
    // background "Edit…" doesn't hide while the picker is open (the cases
    // are mutually exclusive in practice, but the order keeps it
    // deterministic). Keyboard shortcuts are sampled from ImGui state here
    // so the editor can react to S / Z / X / Y / [ / ] without separate
    // input plumbing — same pattern the file picker leans on.
    if (face_editor_.is_open()) {
        // Vertical cursor motion already comes in through menu.navigate()
        // when the deep menu is open (route in MenuSystem::navigate forwards
        // to face_editor_.cursor_step). Horizontal cursor + tool/palette
        // shortcuts are editor-only and not in the menu's input set, so we
        // poll them directly here.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  face_editor_.cursor_step(-1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) face_editor_.cursor_step(+1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Space))      face_editor_.primary();
        if (ImGui::IsKeyPressed(ImGuiKey_X))          face_editor_.secondary();
        if (ImGui::IsKeyPressed(ImGuiKey_Y) ||
            ImGui::IsKeyPressed(ImGuiKey_M))          face_editor_.tertiary();
        if (ImGui::IsKeyPressed(ImGuiKey_Z))          face_editor_.undo();
        if (ImGui::IsKeyPressed(ImGuiKey_V))          face_editor_.preview();
        if (ImGui::IsKeyPressed(ImGuiKey_T))          face_editor_.toggle_live();
        if (ImGui::IsKeyPressed(ImGuiKey_S))          face_editor_.save();
        // Direct tool selection. Number keys 1-6 map to the six tools in
        // declaration order (Pencil/Eraser/Bucket/Eyedrop/Line/Rect); the
        // letter shortcuts (P/E/B/I/L/R) are kept as mnemonics. The deep
        // menu's number-key handlers and main's camera-PiP toggles are
        // both gated by is_face_editor_open(), so 1-6 are exclusive here.
        if (ImGui::IsKeyPressed(ImGuiKey_1))          face_editor_.set_tool(menu::FaceEditor::Tool::Pencil);
        if (ImGui::IsKeyPressed(ImGuiKey_2))          face_editor_.set_tool(menu::FaceEditor::Tool::Eraser);
        if (ImGui::IsKeyPressed(ImGuiKey_3))          face_editor_.set_tool(menu::FaceEditor::Tool::Bucket);
        if (ImGui::IsKeyPressed(ImGuiKey_4))          face_editor_.set_tool(menu::FaceEditor::Tool::Eyedrop);
        if (ImGui::IsKeyPressed(ImGuiKey_5))          face_editor_.set_tool(menu::FaceEditor::Tool::Line);
        if (ImGui::IsKeyPressed(ImGuiKey_6))          face_editor_.set_tool(menu::FaceEditor::Tool::Rect);
        if (ImGui::IsKeyPressed(ImGuiKey_P))          face_editor_.set_tool(menu::FaceEditor::Tool::Pencil);
        if (ImGui::IsKeyPressed(ImGuiKey_E))          face_editor_.set_tool(menu::FaceEditor::Tool::Eraser);
        if (ImGui::IsKeyPressed(ImGuiKey_B))          face_editor_.set_tool(menu::FaceEditor::Tool::Bucket);
        if (ImGui::IsKeyPressed(ImGuiKey_I))          face_editor_.set_tool(menu::FaceEditor::Tool::Eyedrop);
        if (ImGui::IsKeyPressed(ImGuiKey_L))          face_editor_.set_tool(menu::FaceEditor::Tool::Line);
        if (ImGui::IsKeyPressed(ImGuiKey_R))          face_editor_.set_tool(menu::FaceEditor::Tool::Rect);
        // Brush size — Minus shrinks, Equals/Plus grows. Clamped 0..2 by
        // the setter itself, so we just pass current ± 1.
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            face_editor_.set_brush_size(face_editor_.brush_size() - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
            face_editor_.set_brush_size(face_editor_.brush_size() + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  face_editor_.cycle_palette(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) face_editor_.cycle_palette(+1);
        const ImVec2 mp = ImGui::GetMousePos();
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) face_editor_.mouse_down(mp.x, mp.y);
        else                                            face_editor_.mouse_move(mp.x, mp.y);

        face_editor_.draw(dl, font, fs, W, H, accent_color_);
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(1);
        return;
    }

    // File picker overlays the deep menu, same as OSK.
    if (file_picker_.is_open()) {
        file_picker_.draw(dl, font, fs, W, H, accent_color_);
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(1);
        return;
    }

    // Dim the live feeds (still visible behind ~35%).
    dl->AddRectFilled({0.f, 0.f}, {W, H}, IM_COL32(4, 8, 12, 165));

    // Panel
    const float mx = W * 0.07f, my = H * 0.09f;
    const ImVec2 pmin{ mx, my }, pmax{ W - mx, H - my };
    dl->AddRectFilled(pmin, pmax, IM_COL32(8, 12, 16, 200));
    if (border_enabled_)
        dl->AddRect(pmin, pmax, menu_with_alpha(border_color_, 210), 0.f, 0, 2.f);

    const float pad = 30.f;
    const float cx0 = pmin.x + pad;
    const float cx1 = pmax.x - pad;

    // Title
    dl->AddText(font, fs * 1.7f, { cx0, pmin.y + 14.f }, IM_COL32(255, 255, 255, 255), "SETTINGS");

    // Tab bar
    const float tab_y = pmin.y + 16.f + fs * 1.7f + 10.f;
    float tx = cx0;
    for (int t = 0; t < static_cast<int>(deep_tabs_.size()); ++t) {
        std::string lbl = to_upper(deep_tabs_[t].first);
        ImVec2 sz = font->CalcTextSizeA(fs * 1.2f, FLT_MAX, 0.f, lbl.c_str());
        bool active = (t == tab_index_);
        if (active) {
            dl->AddRectFilled({ tx - 8.f, tab_y - 5.f }, { tx + sz.x + 8.f, tab_y + sz.y + 5.f },
                              menu_with_alpha(accent_color_, 45), 3.f);
            dl->AddRect({ tx - 8.f, tab_y - 5.f }, { tx + sz.x + 8.f, tab_y + sz.y + 5.f },
                        menu_with_alpha(accent_color_, 210), 3.f, 0, 1.5f);
        }
        ImU32 col = active ? IM_COL32(255, 255, 255, 255) : menu_with_alpha(accent_color_, 150);
        dl->AddText(font, fs * 1.2f, { tx, tab_y }, col, lbl.c_str());
        tx += sz.x + 30.f;
    }
    const float tabs_bottom = tab_y + fs * 1.2f + 14.f;
    dl->AddLine({ cx0, tabs_bottom }, { cx1, tabs_bottom }, menu_with_alpha(accent_color_, 90), 1.f);

    // Content split
    const float cy0 = tabs_bottom + 20.f;
    const float cy1 = pmax.y - 48.f;
    const float split_x = pmin.x + (pmax.x - pmin.x) * 0.42f;
    dl->AddLine({ split_x, cy0 }, { split_x, cy1 }, menu_with_alpha(accent_color_, 60), 1.f);

    const auto& items = stack_.back().items;

    // Keep the cursor on a visible item.
    {
        int n = static_cast<int>(items.size());
        if (n > 0 && cursor_ < n) {
            const auto& cur = items[cursor_];
            if (cur.visible_fn && !cur.visible_fn()) {
                for (int tries = 0; tries < n; ++tries) {
                    cursor_ = (cursor_ + 1) % n;
                    const auto& t = items[cursor_];
                    if (!t.visible_fn || t.visible_fn()) break;
                }
            }
        }
    }

    // ── Left list ────────────────────────────────────────────────────────────
    auto value_summary = [](const MenuItem& it) -> std::string {
        switch (it.type) {
            case MenuItemType::TOGGLE:
                return (it.get_toggle && it.get_toggle()) ? "ON" : "OFF";
            case MenuItemType::SLIDER: {
                char b[32];
                float v = it.slider.get_value ? it.slider.get_value() : it.slider.min;
                format_slider_value(b, sizeof(b), v, it.slider.min, it.slider.max, it.slider.unit);
                return b;
            }
            case MenuItemType::FACE_PICKER: {
                char b[8];
                std::snprintf(b, sizeof(b), "%d", it.face_picker.get_face ? it.face_picker.get_face() : 0);
                return b;
            }
            case MenuItemType::SUBMENU:
                return ">";
            default:
                return std::string();
        }
    };

    const float row_h  = fs * 1.15f + 18.f;
    const float lx0    = cx0;
    const float lx1    = split_x - 20.f;
    float ly = cy0;

    // ── Vertical scrolling for the left list when it overflows the column ─────
    int vcount_fs = 0, vcur_fs = 0;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (items[i].visible_fn && !items[i].visible_fn()) continue;
        if (i == cursor_) vcur_fs = vcount_fs;
        ++vcount_fs;
    }
    int max_rows_fs = static_cast<int>((cy1 - cy0) / row_h);
    if (max_rows_fs < 1) max_rows_fs = 1;
    const bool scroll_fs = vcount_fs > max_rows_fs;
    if (scroll_fs) {
        if (vcur_fs < list_scroll_)                 list_scroll_ = vcur_fs;
        if (vcur_fs >= list_scroll_ + max_rows_fs)  list_scroll_ = vcur_fs - max_rows_fs + 1;
        if (list_scroll_ > vcount_fs - max_rows_fs) list_scroll_ = vcount_fs - max_rows_fs;
        if (list_scroll_ < 0)                       list_scroll_ = 0;
    } else {
        list_scroll_ = 0;
    }

    int vi_fs = -1;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& it = items[i];
        if (it.visible_fn && !it.visible_fn()) continue;
        ++vi_fs;
        if (vi_fs < list_scroll_) continue;   // scrolled above the column
        if (ly + row_h > cy1) break;          // column full

        bool sel = (i == cursor_);
        ImVec2 rmin{ lx0, ly }, rmax{ lx1, ly + row_h };
        if (sel) {
            dl->AddRectFilled(rmin, rmax, menu_with_alpha(accent_color_, 45), 3.f);
            dl->AddRectFilled({ rmin.x, rmin.y }, { rmin.x + 4.f, rmax.y }, accent_color_);
        }
        ImU32 tcol = sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 220, 226, 175);
        if (it.warn_fn && it.warn_fn())   // flag conflicts (e.g. GPIO pin clash) in red
            tcol = sel ? IM_COL32(255, 120, 105, 255) : IM_COL32(235, 95, 80, 230);
        float ty = ly + (row_h - fs * 1.15f) * 0.5f;
        dl->AddText(font, fs * 1.15f, { lx0 + 14.f, ty }, tcol, to_upper(item_label(it)).c_str());

        if (it.type == MenuItemType::COLOR_PICKER && it.color.get_color) {
            auto [r, g, b] = it.color.get_color();
            dl->AddRectFilled({ lx1 - 36.f, ty + 1.f }, { lx1 - 8.f, ty + fs * 0.9f },
                              IM_COL32(r, g, b, 255), 2.f);
        } else if (it.get_state) {
            // Theme-matching "currently selected" indicator for option lists.
            float cyd = ly + row_h * 0.5f;
            if (it.get_state()) {
                dl->AddCircleFilled({ lx1 - 14.f, cyd }, 5.f, accent_color_);
                dl->AddCircleFilled({ lx1 - 14.f, cyd }, 2.f, IM_COL32(255, 255, 255, 220));
            } else {
                dl->AddCircle({ lx1 - 14.f, cyd }, 5.f, menu_with_alpha(accent_color_, 90), 0, 1.5f);
            }
        } else {
            std::string val = value_summary(it);
            if (!val.empty()) {
                ImVec2 vsz = font->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0.f, val.c_str());
                dl->AddText(font, fs * 1.05f, { lx1 - vsz.x - 10.f, ty },
                            menu_with_alpha(accent_color_, 205), val.c_str());
            }
        }
        ly += row_h;
    }

    // Scroll chevrons (centered at the top / bottom of the list column).
    if (scroll_fs) {
        const float lcx = (lx0 + lx1) * 0.5f;
        const ImU32 c   = menu_with_alpha(accent_color_, 210);
        if (list_scroll_ > 0)
            dl->AddTriangleFilled({lcx - 7.f, cy0 + 8.f}, {lcx + 7.f, cy0 + 8.f},
                                  {lcx, cy0 + 1.f}, c);
        if (list_scroll_ + max_rows_fs < vcount_fs)
            dl->AddTriangleFilled({lcx - 7.f, cy1 - 8.f}, {lcx + 7.f, cy1 - 8.f},
                                  {lcx, cy1 - 1.f}, c);
    }

    // ── Right pane: description + editor ────────────────────────────────────────
    const float rx0 = split_x + 24.f;
    const float rx1 = cx1;
    if (cursor_ < static_cast<int>(items.size())) {
        const auto& sel = items[cursor_];
        dl->AddText(font, fs * 1.5f, { rx0, cy0 }, IM_COL32(255, 255, 255, 255),
                    to_upper(item_label(sel)).c_str());

        float ey = cy0 + fs * 1.5f + 16.f;
        if (!sel.description.empty()) {
            // wrap_width overload: AddText(font, size, pos, col, begin, end, wrap_width)
            dl->AddText(font, fs * 1.0f, { rx0, ey }, IM_COL32(200, 205, 210, 205),
                        sel.description.c_str(), nullptr, rx1 - rx0);
            ey += fs * 1.0f * 3.0f;
        }

        const bool editing = in_edit_mode_;

        // Applicable context preview: the highlighted submenu's own panel, else
        // walk up the stack so it persists through sub-levels (Position / Crop
        // Center / Zoom / etc.). Computed once so it's available while editing too.
        MenuContextPanelDraw panel;
        if (sel.type == MenuItemType::SUBMENU && sel.context_panel_draw) {
            panel = sel.context_panel_draw;
        } else {
            for (auto lvl = stack_.rbegin(); lvl != stack_.rend(); ++lvl)
                if (lvl->panel_draw) { panel = lvl->panel_draw; break; }
        }

        if (editing && sel.type == MenuItemType::SLIDER) {
            // Keep the preview visible while adjusting a slider that affects it
            // (e.g. Inner Bias): preview on top, a compact slider bar below.
            float by;
            if (panel) {
                const float strip_h = 46.f;
                panel(dl, { rx0, ey + 6.f }, { rx1 - rx0, (cy1 - strip_h) - (ey + 6.f) });
                by = cy1 - strip_h + 16.f;
            } else {
                by = ey + 8.f;
            }
            float range = sel.slider.max - sel.slider.min;
            float fill  = (range > 0.f) ? std::clamp((edit_float_ - sel.slider.min) / range, 0.f, 1.f) : 0.f;
            float bw = rx1 - rx0, bh = 12.f;
            dl->AddRectFilled({ rx0, by }, { rx0 + bw, by + bh }, menu_with_alpha(accent_color_, 55), 3.f);
            dl->AddRectFilled({ rx0, by }, { rx0 + bw * fill, by + bh }, menu_with_alpha(accent_color_, 230), 3.f);
            char vb[32]; format_slider_value(vb, sizeof(vb), edit_float_, sel.slider.min, sel.slider.max, sel.slider.unit);
            ImVec2 vsz = font->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0.f, vb);
            dl->AddText(font, fs * 1.1f, { rx1 - vsz.x, by - fs * 1.1f - 2.f },
                        IM_COL32(255, 255, 255, 255), vb);
        } else if (editing && sel.type == MenuItemType::COLOR_PICKER) {
            const float chv[3] = { edit_r_, edit_g_, edit_b_ };
            const char* chn[3] = { "R", "G", "B" };
            const ImU32 chc[3] = { IM_COL32(220,60,60,220), IM_COL32(60,200,60,220), IM_COL32(60,80,220,220) };
            for (int c = 0; c < 3; ++c) {
                float by = ey + 6.f + c * 30.f;
                bool is_sel = (c == edit_channel_);
                dl->AddText(font, fs, { rx0, by }, is_sel ? IM_COL32(255,255,255,255) : IM_COL32(150,160,170,200), chn[c]);
                float bx = rx0 + 22.f, bw = (rx1 - bx) - 48.f;
                dl->AddRectFilled({ bx, by + 2.f }, { bx + bw, by + fs }, menu_with_alpha(accent_color_, 50), 2.f);
                dl->AddRectFilled({ bx, by + 2.f }, { bx + bw * (chv[c] / 255.f), by + fs }, chc[c], 2.f);
                if (is_sel) dl->AddRect({ bx - 1.f, by + 1.f }, { bx + bw + 1.f, by + fs + 1.f }, menu_with_alpha(accent_color_, 220), 2.f, 0, 1.5f);
                char cv[8]; std::snprintf(cv, sizeof(cv), "%.0f", chv[c]);
                dl->AddText(font, fs, { rx1 - 40.f, by }, IM_COL32(220,225,230,220), cv);
            }
            // Text-entry rows: HEX / RGB — select to type a value via the keyboard.
            char tval[2][24];
            std::snprintf(tval[0], sizeof(tval[0]), "%02X%02X%02X",
                          (int)edit_r_, (int)edit_g_, (int)edit_b_);
            std::snprintf(tval[1], sizeof(tval[1]), "%d,%d,%d",
                          (int)edit_r_, (int)edit_g_, (int)edit_b_);
            const char* tnm[2] = { "HEX", "RGB" };
            for (int t = 0; t < 2; ++t) {
                float by = ey + 6.f + (3 + t) * 30.f;
                bool is_sel = (edit_channel_ == 3 + t);
                ImU32 tc = is_sel ? IM_COL32(255,255,255,255) : IM_COL32(150,160,170,200);
                dl->AddText(font, fs, { rx0, by }, tc, tnm[t]);
                dl->AddText(font, fs, { rx0 + 50.f, by }, tc, tval[t]);
                if (is_sel)
                    dl->AddRect({ rx0 - 1.f, by - 1.f }, { rx1 - 6.f, by + fs + 1.f },
                                menu_with_alpha(accent_color_, 220), 2.f, 0, 1.5f);
            }
            dl->AddText(font, fs * 0.9f, { rx0, ey + 6.f + 5 * 30.f },
                        menu_with_alpha(accent_color_, 170),
                        in_channel_edit_ ? "knob adjusts value"
                                         : "knob = row  \xC2\xB7  select = edit / type");
        } else if (editing && sel.type == MenuItemType::FACE_PICKER) {
            int n = sel.face_picker.face_count;
            int cur = static_cast<int>(edit_float_);
            float ccx = (rx0 + rx1) * 0.5f, ccy = ey + 70.f, rad = 56.f;
            for (int f = 0; f < n; ++f) {
                float a = -static_cast<float>(M_PI) * 0.5f + (2.f * static_cast<float>(M_PI) * f) / n;
                float fx = ccx + rad * std::cos(a), fy = ccy + rad * std::sin(a);
                if (f == cur) { dl->AddCircleFilled({fx,fy}, 8.f, menu_with_alpha(accent_color_,230)); dl->AddCircleFilled({fx,fy},3.f,IM_COL32(255,255,255,230)); }
                else dl->AddCircle({fx,fy}, 5.f, menu_with_alpha(accent_color_,120), 0, 1.5f);
            }
        } else {
            // Not editing: show the applicable live context panel (computed
            // above), else a hint for editable items.
            if (panel) {
                panel(dl, { rx0, ey + 6.f }, { rx1 - rx0, cy1 - (ey + 6.f) });
            } else if (sel.type == MenuItemType::SLIDER ||
                       sel.type == MenuItemType::COLOR_PICKER ||
                       sel.type == MenuItemType::FACE_PICKER) {
                dl->AddText(font, fs, { rx0, ey + 6.f }, menu_with_alpha(accent_color_, 170),
                            "Press Enter / A to edit");
            }
        }
    }

    // Bottom hint bar
    dl->AddText(font, fs * 0.95f, { cx0, pmax.y - 32.f }, menu_with_alpha(accent_color_, 185),
                "ENTER/A SELECT   \xC2\xB7   BACKSPACE/B BACK   \xC2\xB7   TAB / LB-RB TABS   \xC2\xB7   F1 / START CLOSE");

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(1);
}

// ── draw_keyboard (on-screen text entry) ────────────────────────────────────────
// Drawn inside draw_fullscreen's window (so it inherits the deep-menu draw list).
void MenuSystem::draw_keyboard(ImDrawList* dl, ImFont* font, float fs,
                               float W, float H) {
    // Dim everything behind.
    dl->AddRectFilled({0.f, 0.f}, {W, H}, IM_COL32(4, 8, 12, 205));

    // Centered panel.
    const float pw = std::min(W * 0.86f, 760.f * ui_scale_);
    const float ph = std::min(H * 0.80f, 460.f * ui_scale_);
    const ImVec2 pmin{ (W - pw) * 0.5f, (H - ph) * 0.5f };
    const ImVec2 pmax{ pmin.x + pw,     pmin.y + ph };
    dl->AddRectFilled(pmin, pmax, IM_COL32(8, 12, 16, 235));
    if (border_enabled_)
        dl->AddRect(pmin, pmax, menu_with_alpha(border_color_, 220), 0.f, 0, 2.f);

    const float pad = 26.f;
    const float x0  = pmin.x + pad;
    const float x1  = pmax.x - pad;

    // Title.
    std::string title = osk_title_.empty() ? std::string("ENTER NAME") : to_upper(osk_title_);
    dl->AddText(font, fs * 1.4f, { x0, pmin.y + 14.f }, IM_COL32(255, 255, 255, 255), title.c_str());

    // Text field.
    const float fy = pmin.y + 14.f + fs * 1.4f + 12.f;
    const float fh = fs * 1.5f + 12.f;
    dl->AddRectFilled({ x0, fy }, { x1, fy + fh }, IM_COL32(0, 0, 0, 160), 3.f);
    dl->AddRect({ x0, fy }, { x1, fy + fh }, menu_with_alpha(accent_color_, 200), 3.f, 0, 1.5f);
    std::string shown = osk_text_;
    // blinking caret
    if (static_cast<int>(ImGui::GetTime() * 2.0) & 1) shown += "_";
    dl->AddText(font, fs * 1.2f, { x0 + 10.f, fy + (fh - fs * 1.2f) * 0.5f },
                IM_COL32(255, 255, 255, 240), shown.c_str());

    // Key grid.
    const auto& rows = osk_rows();
    const float grid_top = fy + fh + 22.f;
    const float grid_bot = pmax.y - 40.f;
    const int   nrows    = static_cast<int>(rows.size());
    const float key_h    = (grid_bot - grid_top) / static_cast<float>(nrows) - 8.f;
    const float gap      = 8.f;

    for (int r = 0; r < nrows; ++r) {
        const auto& row = rows[r];
        int ncols = static_cast<int>(row.size());
        float ky  = grid_top + r * (key_h + 8.f);
        // Letter/number rows are evenly divided across the full width; the last
        // (special-key) row sizes each key by weight.
        bool special = (r == nrows - 1);
        if (!special) {
            float kw = (x1 - x0 - gap * (ncols - 1)) / static_cast<float>(ncols);
            for (int c = 0; c < ncols; ++c) {
                float kx = x0 + c * (kw + gap);
                bool sel = (r == osk_row_ && c == osk_col_);
                ImVec2 kmin{ kx, ky }, kmax{ kx + kw, ky + key_h };
                dl->AddRectFilled(kmin, kmax,
                    sel ? IM_COL32(255, 255, 255, 235) : menu_with_alpha(accent_color_, 40), 3.f);
                if (sel) dl->AddRect(kmin, kmax, menu_with_alpha(accent_color_, 230), 3.f, 0, 1.5f);
                ImVec2 tsz = font->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0.f, row[c].c_str());
                dl->AddText(font, fs * 1.1f,
                            { kx + (kw - tsz.x) * 0.5f, ky + (key_h - fs * 1.1f) * 0.5f },
                            sel ? IM_COL32(10, 12, 14, 255) : IM_COL32(230, 235, 240, 220),
                            row[c].c_str());
            }
        } else {
            // proportional widths for SPACE/DEL/SAVE/CANCEL
            float total_w = x1 - x0 - gap * (ncols - 1);
            float weights[8] = { 1,1,1,1,1,1,1,1 };
            for (int c = 0; c < ncols; ++c) if (row[c] == "SPACE") weights[c] = 2.2f;
            float wsum = 0.f; for (int c = 0; c < ncols; ++c) wsum += weights[c];
            float kx = x0;
            for (int c = 0; c < ncols; ++c) {
                float kw = total_w * (weights[c] / wsum);
                bool sel = (r == osk_row_ && c == osk_col_);
                ImVec2 kmin{ kx, ky }, kmax{ kx + kw, ky + key_h };
                bool is_save   = (row[c] == "SAVE");
                bool is_cancel = (row[c] == "CANCEL");
                ImU32 base = is_save   ? IM_COL32(40, 110, 60, 150)
                           : is_cancel ? IM_COL32(120, 50, 50, 150)
                           :             menu_with_alpha(accent_color_, 40);
                dl->AddRectFilled(kmin, kmax, sel ? IM_COL32(255, 255, 255, 235) : base, 3.f);
                if (sel) dl->AddRect(kmin, kmax, menu_with_alpha(accent_color_, 230), 3.f, 0, 1.5f);
                ImVec2 tsz = font->CalcTextSizeA(fs * 1.0f, FLT_MAX, 0.f, row[c].c_str());
                dl->AddText(font, fs * 1.0f,
                            { kx + (kw - tsz.x) * 0.5f, ky + (key_h - fs) * 0.5f },
                            sel ? IM_COL32(10, 12, 14, 255) : IM_COL32(230, 235, 240, 220),
                            row[c].c_str());
                kx += kw + gap;
            }
        }
    }

    // Hint bar.
    dl->AddText(font, fs * 0.9f, { x0, pmax.y - 26.f }, menu_with_alpha(accent_color_, 185),
                "ARROWS/STICK MOVE   \xC2\xB7   A/ENTER KEY   \xC2\xB7   B/BKSP DELETE   \xC2\xB7   TYPE ON KEYBOARD");
}

// ── draw_radial (quick menu around the minimap) ─────────────────────────────────
// Drawn via the foreground draw list in display pixel coords (the same space the
// nvg minimap uses) so the wheel locks around the minimap. Each nav-stack level is
// a concentric ring of annular-sector wedges (inner = root, outer = active submenu),
// each wedge with an icon slot + label. focus_angle is the screen-space direction the
// "primary" item points; rotate_to_selected spins the selected wedge to that angle.
// A helmet-style perspective tilt (radial_tilt_) foreshortens the top so the wheel
// reads as curving inward like a visor rather than lying flat on glass.
// Draw text centred at `center`, rotated by `angle` radians about it. ImGui's
// AddText has no rotation, so we rotate the glyph vertices it just emitted.
static void dl_text_rot(ImDrawList* dl, ImFont* font, float size, ImVec2 center,
                        ImU32 col, const char* text, float angle) {
    const ImVec2 tsz = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, { center.x - tsz.x * 0.5f, center.y - tsz.y * 0.5f }, col, text);
    const int v1 = dl->VtxBuffer.Size;
    const float s = std::sin(angle), c = std::cos(angle);
    for (int i = v0; i < v1; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float dx = v.pos.x - center.x, dy = v.pos.y - center.y;
        v.pos.x = center.x + dx * c - dy * s;
        v.pos.y = center.y + dx * s + dy * c;
    }
}

void MenuSystem::draw_radial(float cx, float cy, float inner_r,
                             float focus_angle, bool rotate_to_selected) {
    (void)rotate_to_selected;   // the wheel always rotates the selection to focus now
    if (!open_ || stack_.empty()) return;
    s_menu_glow = glow_enabled_;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font   = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * ui_scale_;
    const ImU32  accent = accent_color_;

    const float ring_gap   = 84.f * ui_scale_;   // minimap edge → first ring (clears the compass + gauge)
    const float ring_thick = 60.f * ui_scale_;   // radial thickness of each wedge band
    const float ring_pad   = 6.f  * ui_scale_;   // gap between concentric rings
    const float wedge_gap  = 0.045f;             // angular gap between wedges (rad)

    const int depth = static_cast<int>(stack_.size());

    // ── Helmet-tilt perspective ────────────────────────────────────────────────
    // Model the wheel on a plane tilted about the horizontal axis through the
    // centre; project with a simple pinhole. Top (oy<0) recedes & shrinks, bottom
    // comes forward — a subtle "inside of a visor" curve. tilt 0 → flat.
    // DISABLED for now: keep the wheel planar + concentric with the minimap.
    // (radial_tilt_ / the "Menu Tilt" slider stay wired; set this back to
    //  radial_tilt_ * 0.9f to re-enable.)
    const float ang_t  = 0.f;
    const float sin_t  = std::sin(ang_t), cos_t = std::cos(ang_t);
    const float r_max  = inner_r + ring_gap + depth * (ring_thick + ring_pad) + ring_thick;
    const float focal  = std::max(220.f, r_max * 2.8f);
    auto project = [&](float ox, float oy) -> ImVec2 {
        float z = oy * sin_t;
        float s = focal / (focal - z);
        return ImVec2{ cx + ox * s, cy + oy * cos_t * s };
    };
    auto PR = [&](float ang, float r) -> ImVec2 {
        return project(std::cos(ang) * r, std::sin(ang) * r);
    };

    auto fill_sector = [&](float r0, float r1, float a0, float a1, ImU32 col) {
        int seg = std::max(2, static_cast<int>(std::ceil((a1 - a0) / 0.10f)));
        for (int i = 0; i < seg; ++i) {
            float t0 = a0 + (a1 - a0) * (float)i / seg;
            float t1 = a0 + (a1 - a0) * (float)(i + 1) / seg;
            ImVec2 q[4] = { PR(t0, r0), PR(t0, r1), PR(t1, r1), PR(t1, r0) };
            dl->AddConvexPolyFilled(q, 4, col);
        }
    };
    auto stroke_sector = [&](float r0, float r1, float a0, float a1, ImU32 col, float th) {
        int seg = std::max(2, static_cast<int>(std::ceil((a1 - a0) / 0.10f)));
        std::vector<ImVec2> pts;
        for (int i = 0; i <= seg; ++i) pts.push_back(PR(a0 + (a1 - a0) * (float)i / seg, r1));
        for (int i = 0; i <= seg; ++i) pts.push_back(PR(a1 - (a1 - a0) * (float)i / seg, r0));
        dl->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_Closed, th);
    };
    auto draw_icon = [&](const MenuItem& it, ImVec2 p, float g, ImU32 col) {
        switch (it.type) {
            case MenuItemType::TOGGLE: {
                bool on = it.get_toggle && it.get_toggle();
                if (on) { dl->AddCircleFilled(p, g * 0.5f, col);
                          dl->AddCircleFilled(p, g * 0.22f, IM_COL32(20,22,26,255)); }
                else      dl->AddCircle(p, g * 0.5f, col, 0, 1.6f);
                break;
            }
            case MenuItemType::SLIDER:
                dl->AddRectFilled({ p.x - g*0.55f, p.y - 1.6f }, { p.x + g*0.55f, p.y + 1.6f }, col, 1.f);
                dl->AddCircleFilled({ p.x + g*0.18f, p.y }, g*0.26f, col);
                break;
            case MenuItemType::SUBMENU:
                dl->AddTriangleFilled({ p.x - g*0.28f, p.y - g*0.42f },
                                      { p.x - g*0.28f, p.y + g*0.42f },
                                      { p.x + g*0.40f, p.y }, col);
                break;
            default:
                dl->AddCircleFilled(p, g * 0.30f, col);
                break;
        }
    };

    auto value_summary = [](const MenuItem& it) -> std::string {
        switch (it.type) {
            case MenuItemType::TOGGLE:
                return (it.get_toggle && it.get_toggle()) ? "ON" : "OFF";
            case MenuItemType::SLIDER: {
                char b[32];
                float v = it.slider.get_value ? it.slider.get_value() : it.slider.min;
                format_slider_value(b, sizeof(b), v, it.slider.min, it.slider.max, it.slider.unit);
                return b;
            }
            default: return std::string();
        }
    };

    for (int L = 0; L < depth; ++L) {
        const Level& lvl  = stack_[L];
        const bool  active = (L == depth - 1);
        const float r0 = inner_r + ring_gap + L * (ring_thick + ring_pad);
        const float r1 = r0 + ring_thick;
        const int   sel_idx = active ? cursor_ : lvl.cursor;

        std::vector<int> vis;
        for (int i = 0; i < static_cast<int>(lvl.items.size()); ++i)
            if (!lvl.items[i].visible_fn || lvl.items[i].visible_fn()) vis.push_back(i);
        const int N = static_cast<int>(vis.size());
        if (N == 0) continue;

        int sel_vis = 0;
        for (int k = 0; k < N; ++k) if (vis[k] == sel_idx) { sel_vis = k; break; }

        const float step = 2.f * static_cast<float>(M_PI) / static_cast<float>(N);

        // Active ring eases its selected wedge to the focus angle (smooth spin);
        // inner rings snap to their chosen item.
        float anim_sel = static_cast<float>(sel_vis);
        if (active) {
            if (radial_prev_sel_ < 0) {
                radial_anim_   = static_cast<float>(sel_vis);
                radial_target_ = static_cast<float>(sel_vis);
            } else if (sel_vis != radial_prev_sel_) {
                int d = sel_vis - radial_prev_sel_;       // shortest signed step (ring of N)
                while (d >  N / 2) d -= N;
                while (d < -N / 2) d += N;
                radial_target_ += static_cast<float>(d);
            }
            radial_prev_sel_ = sel_vis;
            float dt = ImGui::GetIO().DeltaTime;
            if (dt <= 0.f || dt > 0.1f) dt = 0.016f;
            radial_anim_ += (radial_target_ - radial_anim_) * (1.f - std::exp(-dt * 13.f));
            anim_sel = radial_anim_;
        }

        for (int k = 0; k < N; ++k) {
            const MenuItem& it = lvl.items[vis[k]];
            const bool primary = (vis[k] == sel_idx);
            float ac = focus_angle + (static_cast<float>(k) - anim_sel) * step;
            float a0 = ac - step * 0.5f + wedge_gap * 0.5f;
            float a1 = ac + step * 0.5f - wedge_gap * 0.5f;

            // Wedge fill: selected = accent; others = faint dark, dimmer on inner rings.
            ImU32 fill = primary
                ? (active ? menu_with_alpha(accent, 210) : menu_with_alpha(accent, 110))
                : (active ? IM_COL32(18, 24, 30, 180) : IM_COL32(14, 18, 24, 130));
            fill_sector(r0, r1, a0, a1, fill);
            stroke_sector(r0, r1, a0, a1,
                          primary ? menu_with_alpha(accent, 235) : menu_with_alpha(accent, 70),
                          primary ? 2.0f : 1.0f);

            // Only annotate the active ring and the chosen item on inner rings.
            if (!active && !primary) continue;

            const ImU32 icon_col = primary ? IM_COL32(20, 22, 26, 255)
                                           : menu_with_alpha(accent, 220);
            const ImU32 text_col = primary ? IM_COL32(20, 22, 26, 255)
                                  : active  ? IM_COL32(230, 235, 240, 220)
                                  :           menu_with_alpha(accent, 170);

            // Icon slot (inner band) — kept upright.
            draw_icon(it, PR(ac, r0 + ring_thick * 0.30f), 16.f * ui_scale_, icon_col);

            // Label + value follow the arc (rotated to the tangent; flipped on the
            // bottom half so they never read upside-down).
            float ta = ac + static_cast<float>(M_PI) * 0.5f;
            if (std::sin(ac) > 0.f) ta += static_cast<float>(M_PI);

            std::string label = to_upper(item_label(it));
            std::string val   = value_summary(it);
            const float lsz = primary ? fs * 0.98f : fs * 0.86f;
            ImVec2 lp = PR(ac, r0 + ring_thick * 0.62f);
            dl_text_rot(dl, font, lsz, { lp.x + 1.f, lp.y + 1.f }, IM_COL32(0, 0, 0, 170), label.c_str(), ta);
            dl_text_rot(dl, font, lsz, lp, text_col, label.c_str(), ta);
            if (!val.empty()) {
                ImVec2 vp = PR(ac, r0 + ring_thick * 0.88f);
                ImU32 vc = primary ? IM_COL32(20, 22, 26, 230) : menu_with_alpha(accent, 200);
                dl_text_rot(dl, font, fs * 0.78f, vp, vc, val.c_str(), ta);
            }
        }
    }

    // ── Value sub-ring (editing a slider / face value) ──────────────────────────
    // Selecting Zoom/Focus opens a half-height outer ring: Up/Down adjust the value,
    // Enter confirms, Left returns. Shown as an arc gauge + the live value.
    if (in_edit_mode_ && !stack_.empty()) {
        const auto& items = stack_.back().items;
        if (cursor_ >= 0 && cursor_ < static_cast<int>(items.size())) {
            const MenuItem& it = items[cursor_];
            const bool is_val = (it.type == MenuItemType::SLIDER ||
                                 it.type == MenuItemType::FACE_PICKER);
            if (is_val) {
                const float vrm = inner_r + ring_gap + depth * (ring_thick + ring_pad)
                                  + ring_thick * 0.25f;        // centre of a half-height band
                const float vth = ring_thick * 0.5f;
                float frac = 0.f; char vbuf[32] = {0};
                if (it.type == MenuItemType::SLIDER) {
                    const float range = it.slider.max - it.slider.min;
                    frac = (range > 0.f)
                        ? std::clamp((edit_float_ - it.slider.min) / range, 0.f, 1.f) : 0.f;
                    format_slider_value(vbuf, sizeof(vbuf), edit_float_,
                                        it.slider.min, it.slider.max, it.slider.unit);
                } else {
                    const int n = std::max(1, it.face_picker.face_count);
                    frac = (n > 1) ? static_cast<float>((int)edit_float_) / (n - 1) : 0.f;
                    std::snprintf(vbuf, sizeof(vbuf), "%d", (int)edit_float_);
                }

                // Partial arc centred on the focus direction (which already points
                // into the view), so the gauge stays on-screen even when the minimap
                // is anchored in a corner. Fill grows from the start toward the end.
                const float SPAN = 2.4f;                       // ~138° gauge
                const float aS = focus_angle - SPAN * 0.5f;
                dl->PathArcTo({ cx, cy }, vrm, aS, aS + SPAN, 64);
                dl->PathStroke(IM_COL32(18, 24, 30, 220), 0, vth);
                dl->PathArcTo({ cx, cy }, vrm, aS, aS + frac * SPAN, 64);
                dl->PathStroke(menu_with_alpha(accent, 235), 0, vth);

                // Big live value at the focus position (centre of the arc).
                ImVec2 vp = PR(focus_angle, vrm);
                const float vsz = fs * 1.35f;
                ImVec2 ts = font->CalcTextSizeA(vsz, FLT_MAX, 0.f, vbuf);
                dl->AddText(font, vsz, { vp.x - ts.x*0.5f + 1.f, vp.y - ts.y*0.5f + 1.f },
                            IM_COL32(0, 0, 0, 210), vbuf);
                dl->AddText(font, vsz, { vp.x - ts.x*0.5f, vp.y - ts.y*0.5f },
                            IM_COL32(255, 255, 255, 255), vbuf);

                // Hint, placed further along the focus direction (toward the view
                // centre → always on-screen).
                const char* hint = "UP/DN ADJUST   \xC2\xB7   ENTER OK   \xC2\xB7   \xE2\x86\x90 BACK";
                ImVec2 hp = PR(focus_angle, vrm + vth * 0.5f + 12.f);
                ImVec2 hs = font->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.f, hint);
                dl->AddText(font, fs * 0.72f, { hp.x - hs.x*0.5f + 1.f, hp.y - hs.y*0.5f + 1.f },
                            IM_COL32(0, 0, 0, 200), hint);
                dl->AddText(font, fs * 0.72f, { hp.x - hs.x*0.5f, hp.y - hs.y*0.5f },
                            menu_with_alpha(accent, 210), hint);
            }
        }
    }
}

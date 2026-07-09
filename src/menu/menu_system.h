#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <tuple>
#include <cstdint>
#include <imgui.h>
#include "../app_state.h"
#include "overlay.h"
#include "file_picker.h"
#include "face_editor.h"
#include "color_picker.h"

// ── Item types ────────────────────────────────────────────────────────────────

enum class MenuItemType {
    LEAF,          // fires action() and closes menu
    SUBMENU,       // descends into children
    TOGGLE,        // flips a bool; menu stays open; shows ON/OFF indicator
    SLIDER,        // enters numeric edit mode; knob adjusts value
    COLOR_PICKER,  // opens the unified color-picker overlay (menu::ColorPicker)
    FACE_PICKER,   // radial dot selector for face index
    NOTIF_LOG,     // scrollable notification history list
};

// ── Slider config ─────────────────────────────────────────────────────────────

struct SliderConfig {
    float min  = 0.f;
    float max  = 1.f;
    float step = 1.f;
    // Unit suffix appended to rendered value.
    //   ""     → raw integer  e.g. "5"
    //   " %"   → literal %    e.g. "75 %"  (value stored as 25–150)
    //   "%"    → scaled %     e.g. "75%"   (value/max * 100, for 0–255 raw)
    //   " EV"  → EV suffix    e.g. "+1.0 EV"
    std::string unit;
    std::function<float()>     get_value;
    std::function<void(float)> set_value;
};

// ── Face picker config ────────────────────────────────────────────────────────

struct FacePickerConfig {
    int face_count = 10;
    std::function<int()>     get_face;
    std::function<void(int)> set_face;
};

// ── Color picker config ───────────────────────────────────────────────────────

struct ColorPickerConfig {
    std::function<void(uint8_t r, uint8_t g, uint8_t b)>            set_color;
    std::function<std::tuple<uint8_t, uint8_t, uint8_t>()>          get_color; // optional seed
};

// ── Notification log config ───────────────────────────────────────────────────

struct NotifLogConfig {
    NotificationQueue* queue = nullptr;   // pointer into AppState::notifs
    // Optional filter — when set, only notifications for which it returns true
    // are listed (used by the type/sender notification browser).
    std::function<bool(const Notification&)> filter;
    // When true, list retained-but-dismissed notifications too (a real history
    // browser) instead of only the currently-active ones.
    bool show_history = false;
};

// ── Context panel ─────────────────────────────────────────────────────────────
// A side-panel that opens next to the menu while a particular submenu level is
// active.  Reusable for visualisations (crop preview, audio meters) or for
// pure info text.  Renders every frame the level is open — reads live state.
//
// Callback contract:
//   dl     — ImDrawList of the panel window (already opened/sized by MenuSystem).
//   origin — top-left of the content rect (post-padding).
//   size   — content rect dimensions.
using MenuContextPanelDraw =
    std::function<void(ImDrawList* dl, ImVec2 origin, ImVec2 size)>;

// ── Menu item ─────────────────────────────────────────────────────────────────

struct MenuItem {
    std::string   label;
    MenuItemType  type = MenuItemType::LEAF;

    // Optional dynamic label — when set, its return value is rendered instead of
    // `label`. Lets a row reflect runtime data (e.g. a saved-profile name) without
    // rebuilding the menu tree. `label` is still used as a fallback / id.
    std::function<std::string()> label_fn;

    // Optional help/description shown in the deep menu's right-hand pane.
    // Falls back to the label when empty.
    std::string   description;

    // LEAF
    std::function<void()> action;
    // Optional "secondary" action fired on Ctrl+Select. If secondary_children is
    // non-empty it's pushed as a submenu (e.g. an effect's settings); otherwise
    // secondary_action() runs. Lets a row "apply on select, open settings on
    // Ctrl+select".
    std::function<void()> secondary_action;
    std::vector<MenuItem> secondary_children;

    // SUBMENU
    std::vector<MenuItem> children;
    // Optional context panel that renders next to the menu while this submenu
    // is the active level.  Only meaningful when type == SUBMENU.
    std::string           context_panel_title;
    MenuContextPanelDraw  context_panel_draw;

    // TOGGLE
    std::function<bool()>     get_toggle;
    std::function<void(bool)> set_toggle;
    std::function<bool()>     get_state;  // legacy radio indicator (checked in draw)

    // SLIDER
    SliderConfig slider;

    // FACE_PICKER
    FacePickerConfig face_picker;

    // COLOR_PICKER
    ColorPickerConfig color;

    // NOTIF_LOG
    NotifLogConfig notif_log;

    // Optional visibility predicate — item is hidden when this returns false.
    // When unset, the item is always visible.
    std::function<bool()> visible_fn;

    // Optional "live preview" callback fired when this item becomes highlighted
    // (cursor lands on it), without selecting. Used so zoom/crop/position option
    // lists apply their effect as the user tabs through them.
    std::function<void()> on_highlight;

    // Optional warning predicate — when it returns true the row is rendered in a
    // warning colour (red) to flag a problem the user should fix (e.g. a GPIO
    // slot whose pin collides with another slot or a hardware peripheral).
    std::function<bool()> warn_fn;
};

// ── Menu anchor ───────────────────────────────────────────────────────────────
enum class MenuAnchor { TopLeft, TopRight, BottomLeft, BottomRight };

// ── Selection style ───────────────────────────────────────────────────────────
enum class SelectionStyle {
    ACCENT_BAR,  // left colored bar + subtle header fill (default)
    FILLED_ROW,  // opaque white row, black text, no glow (Halo theme)
};

// ── Quick-menu style ────────────────────────────────────────────────────────────
enum class QuickStyle {
    List,    // legacy compact corner list
    Radial,  // ring(s) encircling the round minimap
};

// ── Menu system ───────────────────────────────────────────────────────────────
// Stack-based menu driven by SmartKnob detents.
// The HUD calls navigate() on knob events and draw() each frame.
// When the selected menu changes, send_detents() callback fires with
// the new detent count so the caller can update the knob haptics.
class MenuSystem {
public:
    using DetentCallback = std::function<void(int count)>;

    explicit MenuSystem(std::vector<MenuItem> root);

    void set_detent_callback(DetentCallback cb) { detent_cb_ = std::move(cb); }

    // Runtime style setters — take effect immediately on the next draw() call.
    void set_accent_color(ImU32 c)        { accent_color_     = c; }
    void set_bg_enabled(bool e)           { bg_enabled_       = e; }
    void set_bg_color(ImU32 c)            { bg_color_         = c; }
    void set_glow_enabled(bool e)         { glow_enabled_     = e; }
    void set_bold_text(bool b)            { bold_text_        = b; }
    void set_border_enabled(bool e)       { border_enabled_   = e; }
    void set_border_color(ImU32 c)        { border_color_     = c; }
    void set_border_thickness(float t)    { border_thickness_ = t; }
    void set_selection_style(SelectionStyle s) { selection_style_ = s; }
    void set_anchor(MenuAnchor a)              { anchor_          = a; }
    // Overall UI scale for the full-screen deep menu (and landing page) — lets a
    // theme change the size/feel. 1.0 = default; ~0.8–1.6 sensible.
    void  set_ui_scale(float s)                { ui_scale_ = (s > 0.1f) ? s : 1.f; }
    float ui_scale() const                     { return ui_scale_; }

    // Runtime style getters — for persisting user changes to config on exit.
    ImU32          accent_color()     const { return accent_color_;     }
    ImU32          bg_color()         const { return bg_color_;         }
    bool           bg_enabled()       const { return bg_enabled_;       }
    bool           border_enabled()   const { return border_enabled_;   }
    ImU32          border_color()     const { return border_color_;     }
    float          border_thickness() const { return border_thickness_; }
    SelectionStyle selection_style()  const { return selection_style_;  }
    MenuAnchor     anchor()           const { return anchor_;           }

    // Drive from knob events
    void navigate(int direction);   // +1 = next, -1 = prev (or adjust value in edit mode)
    void select();                  // confirm / toggle / enter edit mode
    void secondary();               // Ctrl+Select: run secondary_action / push secondary_children
    void back();                    // pop menu level (or exit edit mode)

    // Render the compact corner "quick menu" overlay using Dear ImGui windows
    void draw(int screen_w, int screen_h);

    // Render the quick menu as a radial wheel encircling the round minimap.
    // (center_x, center_y, inner_radius) are the minimap's geometry in the SAME
    // framebuffer pixel space ImGui draws in (display coords). Submenu levels are
    // drawn as concentric outer rings. When rotate_to_selected is true (minimap
    // anchored near a screen edge) the wheel spins so the selected item sits at the
    // top; otherwise the ring is static and the highlight moves.
    // dock_top: the minimap (and thus this wheel) is pinned to the TOP half of
    // the screen — curved wedge labels flip as one run so they still read
    // right-way-up. Bottom-docked (default) labels curve without flipping.
    void draw_radial(float center_x, float center_y, float inner_radius,
                     float focus_angle, bool rotate_to_selected,
                     bool dock_top = false);

    // Quick-menu style (corner list vs. radial-around-minimap).
    void      set_quick_style(QuickStyle s) { quick_style_ = s; }
    QuickStyle quick_style() const          { return quick_style_; }

    // Radial "helmet tilt": 0 = flat, ~0.35 = subtle inward tilt, up to 0.8.
    void  set_radial_tilt(float t) { radial_tilt_ = t < 0.f ? 0.f : (t > 0.8f ? 0.8f : t); }
    float radial_tilt() const      { return radial_tilt_; }

    // Render the full-screen, tabbed "deep menu" (game-style settings screen)
    // over the live feeds. Reuses the same MenuItem tree + nav/edit state.
    void draw_fullscreen(int screen_w, int screen_h);

    // The corner "quick menu" shows a separate curated tree (set via
    // set_quick_items); the full-screen deep menu always uses root_items_. If no
    // quick tree is set, the quick menu falls back to the full tree.
    void set_quick_items(std::vector<MenuItem> items) { quick_items_ = std::move(items); }

    bool is_open()    const { return open_; }
    void open()             { deep_open_ = false; stack_.clear(); open_ = true;
                              push_level(quick_items_.empty() ? root_items_ : quick_items_); }
    void close() {
        open_            = false;
        deep_open_       = false;
        in_edit_mode_    = false;
        osk_active_      = false;
        osk_commit_      = nullptr;
        file_picker_.close();
        face_editor_.close();
        color_picker_.close();
        overlay_ = nullptr;
        stack_.clear();
    }

    // ── Deep (full-screen) menu ─────────────────────────────────────────────────
    bool is_deep_open() const { return deep_open_; }
    void open_deep();           // build tabs, show full-screen menu
    // Open the deep menu directly on a nested page: path[0] = tab label,
    // the rest = SUBMENU labels to descend (e.g. {"System","Software",
    // "Updates"}). Stops at the deepest matching page.
    void open_deep_at(const std::vector<std::string>& path);
    // F-row tab jumps: tab metadata + open-on-tab. deep_tab_count() builds
    // the tab list on first use (cheap after that).
    int  deep_tab_count()       { build_deep_tabs(); return static_cast<int>(deep_tabs_.size()); }
    int  deep_tab_index() const { return tab_index_; }
    void open_deep_tab(int idx) { open_deep(); if (deep_open_) load_tab(idx); }
    void close_deep();          // hide full-screen menu
    void next_tab();            // switch to the next/prev top-level tab (at tab base only)
    void prev_tab();

    // ── On-screen keyboard ──────────────────────────────────────────────────────
    // A full-screen text-entry overlay (drawn by draw_fullscreen while the deep
    // menu is open). Used to name profiles, etc. On commit it fires the callback
    // with the entered string. Driven by the same input devices via the osk_*
    // routing methods below (route to these from your input handlers when
    // is_keyboard_open() is true, so the knob/gamepad/keyboard all work).
    using KeyboardCommit = std::function<void(const std::string&)>;
    void open_keyboard(std::string title, std::string initial, KeyboardCommit on_commit,
                       size_t max_len = 40);
    void close_keyboard();
    bool is_keyboard_open() const { return osk_active_; }
    const std::string& keyboard_text() const { return osk_text_; }

    void osk_move(int dx, int dy);   // 2D grid move (gamepad d-pad / arrows);
                                     // Up past the top row focuses the text field,
                                     // where Left/Right move the text caret
    void osk_step(int d);            // linear walk across the grid (knob rotate)
    void osk_activate();             // press the focused key
    void osk_backspace();            // delete before the caret (or cancel if empty)
    void osk_commit();               // confirm + fire callback
    void osk_cancel();               // discard + close
    void osk_input_char(unsigned int c);  // insert a physically-typed character

    // ── File picker ─────────────────────────────────────────────────────────────
    // Full-screen overlay for browsing the filesystem (media import). Drawn in
    // place of the deep menu while is_file_picker_open() is true. Input routing
    // mirrors OSK: navigate/select/back forward to the picker until it closes.
    void open_file_picker(std::string title,
                          std::string start_dir,
                          std::vector<std::string> extensions,
                          std::function<void(const std::string&)> on_commit,
                          std::function<void()> on_cancel = {});
    void close_file_picker();
    bool is_file_picker_open() const { return file_picker_.is_open(); }
    const std::string& file_picker_dir() const { return file_picker_.current_dir(); }

    // ── Face editor ─────────────────────────────────────────────────────────────
    // Full-screen pixel editor for face PNGs (MAX7219 / RGB matrix backends).
    // Same overlay + input-routing pattern as the file picker. The on_commit
    // callback receives the new RGBA canvas + the abs_path to save to; main.cpp
    // typically writes via cv::imwrite and triggers a face reload.
    void open_face_editor(std::string title,
                          std::string abs_path,
                          int canvas_w, int canvas_h,
                          std::vector<cv::Rect> covered_regions,
                          std::vector<std::string> covered_labels,
                          int mirror_axis_x,
                          menu::FaceEditor::Mode mode,
                          std::vector<uint32_t> palette,
                          std::vector<menu::FaceEditor::EyePoly> eye_polys,
                          menu::FaceEditor::CommitFn on_commit,
                          menu::FaceEditor::CancelFn on_cancel = {},
                          menu::FaceEditor::PreviewFn on_preview = {},
                          menu::FaceEditor::LiveFrameFn live_frame = {},
                          double preview_duration_s = 10.0);
    void close_face_editor();
    bool is_face_editor_open() const { return face_editor_.is_open(); }

    menu::FaceEditor& face_editor() { return face_editor_; }

    // ── Color picker ────────────────────────────────────────────────────────────
    // Selecting any COLOR_PICKER item opens the unified picker overlay (see
    // color_picker.h). Same routing as the other overlays; input handlers use
    // overlay_move() to forward horizontal d-pad / arrow presses (SV square,
    // hue strip, swatch rows) while it's open.
    bool is_color_picker_open() const { return color_picker_.is_open(); }
    void overlay_move(int dx, int dy) {
        if (overlay_ && overlay_->is_open()) overlay_->move(dx, dy);
    }

    int  current_index() const { return cursor_; }
    int  menu_depth()    const { return static_cast<int>(stack_.size()); }
    const std::string& current_label() const;

    // True when navigate() adjusts a numeric value (slider value, face index, or a
    // color channel being edited) rather than moving a cursor. Input handlers use
    // this to flip Up/Down so Up always *increases* the value (and Down decreases),
    // while plain list scrolling keeps Up = previous / Down = next.
    bool editing_value() const;

private:
    // A page references the stable menu tree instead of deep-copying it —
    // MenuItem owns its children by value, so the old per-level copy cloned
    // every nested subtree (thousands of string/std::function allocations =
    // a visible hitch on every open/descend). The tree (root_items_ /
    // quick_items_ / deep tabs) is structurally immutable after build_menu;
    // dynamic rows read live data through their label_fn/visible_fn hooks,
    // so the pointer stays valid for the life of the level. `nav` is the
    // synthesized "Close Menu"/"< Back" row appended as the last index.
    struct Level {
        const std::vector<MenuItem>* src = nullptr;
        MenuItem              nav;
        int                   cursor = 0;
        std::string           panel_title;
        MenuContextPanelDraw  panel_draw;

        int size() const { return src ? static_cast<int>(src->size()) + 1 : 0; }
        const MenuItem& at(int i) const {
            return i < static_cast<int>(src->size()) ? (*src)[i] : nav;
        }
    };
    // Adapter so nav/draw code keeps its `items[i]` / `items.size()` shape.
    struct LevelView {
        const Level& lv;
        int size() const { return lv.size(); }
        const MenuItem& operator[](int i) const { return lv.at(i); }
    };

    void push_level(const std::vector<MenuItem>& items,
                    std::string panel_title = std::string(),
                    MenuContextPanelDraw panel_draw = nullptr);
    void pop_level();
    void draw_context_panel(const Level& lvl, int screen_w, int screen_h,
                            float menu_x, float menu_y,
                            float menu_w, float menu_h);
    void emit_detents();
    void emit_detents_override(int count);

    // ── edit-mode state (SLIDER / FACE_PICKER; colors use the overlay) ────────
    bool  in_edit_mode_    = false;
    float edit_float_      = 0.f;     // working copy for SLIDER
    float orig_float_ = 0.f;          // pre-edit value for cancel/restore

    // ── deep (full-screen) menu state ───────────────────────────────────────────
    void build_deep_tabs();     // derive tabs from root_items_
    void load_tab(int idx);     // reset stack_ to the given tab's items
    bool deep_open_  = false;
    int  tab_index_  = 0;
    // Tab pages point into root_items_'s submenu children; rows that aren't
    // submenus get an owned home in deep_general_. Built once — see
    // build_deep_tabs().
    std::vector<std::pair<std::string, const std::vector<MenuItem>*>> deep_tabs_;
    std::vector<MenuItem> deep_general_;

    // ── on-screen keyboard state ────────────────────────────────────────────────
    void draw_keyboard(ImDrawList* dl, ImFont* font, float fs, float W, float H);
    void osk_insert(char c);         // insert at caret, respecting osk_max_len_
    bool           osk_active_ = false;
    std::string    osk_title_;
    std::string    osk_text_;
    int            osk_row_ = 0;     // -1 = text field focused (caret editing)
    int            osk_col_ = 0;
    int            osk_caret_ = 0;   // insertion index into osk_text_
    int            osk_page_ = 0;    // 0 = letters, 1 = symbols
    size_t         osk_max_len_ = 40;
    KeyboardCommit osk_commit_;

    // File picker overlay (media import) — same input-routing pattern as OSK.
    menu::FilePicker file_picker_;

    // Face editor overlay (pixel-art authoring) — same overlay pattern.
    menu::FaceEditor face_editor_;

    // Unified color picker overlay — opened by any COLOR_PICKER item.
    menu::ColorPicker color_picker_;


    // Active full-screen overlay (one of the members above), or nullptr.
    // navigate/select/back/draw_fullscreen dispatch through this instead of
    // per-class if-chains; the OSK is still checked first, before the overlay.
    menu::IOverlay* overlay_ = nullptr;

    std::vector<MenuItem>  root_items_;
    std::vector<MenuItem>  quick_items_;   // curated corner "quick menu" tree
    std::vector<Level>     stack_;
    int                    cursor_ = 0;
    int                    list_scroll_ = 0;   // first visible row when a level overflows the screen
    bool                   open_   = false;
    DetentCallback         detent_cb_;

    // Runtime style
    ImU32          accent_color_     = IM_COL32(255, 255, 255, 255);  // Halo default: white
    bool           bg_enabled_       = true;
    ImU32          bg_color_         = IM_COL32( 10,  15,  20, 225);
    bool           glow_enabled_     = false;   // Halo default: no glow
    bool           bold_text_        = true;    // Halo default: bold
    bool           border_enabled_   = true;
    ImU32          border_color_     = IM_COL32(255, 255, 255, 255);  // Halo default: white
    float          border_thickness_ = 5.0f;                           // Halo default
    SelectionStyle selection_style_  = SelectionStyle::FILLED_ROW;    // Halo default
    MenuAnchor     anchor_           = MenuAnchor::TopLeft;
    float          ui_scale_         = 1.0f;   // deep menu / landing page size
    QuickStyle     quick_style_      = QuickStyle::Radial;  // corner list vs radial
    float          radial_tilt_      = 0.35f;  // helmet-style inward perspective

    // Radial-wheel spin animation: the active ring eases its selected wedge to the
    // focus angle instead of snapping. radial_anim_ is the displayed fractional
    // index, radial_target_ the (wrap-accumulated) goal, radial_prev_sel_ the last
    // selected visible index (-1 = reset on a level change).
    float          radial_anim_      = 0.f;
    float          radial_target_    = 0.f;
    int            radial_prev_sel_  = -1;
};

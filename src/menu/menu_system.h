#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <tuple>
#include <cstdint>
#include <imgui.h>
#include "../app_state.h"

// ── Item types ────────────────────────────────────────────────────────────────

enum class MenuItemType {
    LEAF,          // fires action() and closes menu
    SUBMENU,       // descends into children
    TOGGLE,        // flips a bool; menu stays open; shows ON/OFF indicator
    SLIDER,        // enters numeric edit mode; knob adjusts value
    COLOR_PICKER,  // enters R/G/B channel edit mode
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
    void back();                    // pop menu level (or exit edit mode)

    // Render the compact corner "quick menu" overlay using Dear ImGui windows
    void draw(int screen_w, int screen_h);

    // Render the quick menu as a radial wheel encircling the round minimap.
    // (center_x, center_y, inner_radius) are the minimap's geometry in the SAME
    // framebuffer pixel space ImGui draws in (display coords). Submenu levels are
    // drawn as concentric outer rings. When rotate_to_selected is true (minimap
    // anchored near a screen edge) the wheel spins so the selected item sits at the
    // top; otherwise the ring is static and the highlight moves.
    void draw_radial(float center_x, float center_y, float inner_radius,
                     float focus_angle, bool rotate_to_selected);

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
        in_channel_edit_ = false;
        osk_active_      = false;
        osk_commit_      = nullptr;
        stack_.clear();
    }

    // ── Deep (full-screen) menu ─────────────────────────────────────────────────
    bool is_deep_open() const { return deep_open_; }
    void open_deep();           // build tabs, show full-screen menu
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
    void open_keyboard(std::string title, std::string initial, KeyboardCommit on_commit);
    void close_keyboard();
    bool is_keyboard_open() const { return osk_active_; }
    const std::string& keyboard_text() const { return osk_text_; }

    void osk_move(int dx, int dy);   // 2D grid move (gamepad d-pad / arrows)
    void osk_step(int d);            // linear walk across the grid (knob rotate)
    void osk_activate();             // press the focused key
    void osk_backspace();            // delete last char (or cancel if empty)
    void osk_commit();               // confirm + fire callback
    void osk_cancel();               // discard + close
    void osk_input_char(unsigned int c);  // append a physically-typed character

    int  current_index() const { return cursor_; }
    int  menu_depth()    const { return static_cast<int>(stack_.size()); }
    const std::string& current_label() const;

    // True when navigate() adjusts a numeric value (slider value, face index, or a
    // color channel being edited) rather than moving a cursor. Input handlers use
    // this to flip Up/Down so Up always *increases* the value (and Down decreases),
    // while plain list scrolling keeps Up = previous / Down = next.
    bool editing_value() const;

private:
    struct Level {
        std::vector<MenuItem> items;
        int                   cursor = 0;
        std::string           panel_title;
        MenuContextPanelDraw  panel_draw;
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

    // ── edit-mode state ───────────────────────────────────────────────────────
    bool  in_edit_mode_    = false;
    float edit_float_      = 0.f;     // working copy for SLIDER
    int   edit_channel_    = 0;       // 0=R 1=G 2=B for COLOR_PICKER
    bool  in_channel_edit_ = false;   // true when knob adjusts channel value
    float edit_r_ = 0.f, edit_g_ = 0.f, edit_b_ = 0.f;
    float orig_float_ = 0.f;                              // pre-edit value for SLIDER cancel/restore
    float orig_r_ = 0.f, orig_g_ = 0.f, orig_b_ = 0.f;  // pre-edit RGB for COLOR_PICKER cancel/restore

    // ── deep (full-screen) menu state ───────────────────────────────────────────
    void build_deep_tabs();     // derive tabs from root_items_
    void load_tab(int idx);     // reset stack_ to the given tab's items
    bool deep_open_  = false;
    int  tab_index_  = 0;
    std::vector<std::pair<std::string, std::vector<MenuItem>>> deep_tabs_;

    // ── on-screen keyboard state ────────────────────────────────────────────────
    void draw_keyboard(ImDrawList* dl, ImFont* font, float fs, float W, float H);
    bool           osk_active_ = false;
    std::string    osk_title_;
    std::string    osk_text_;
    int            osk_row_ = 0;
    int            osk_col_ = 0;
    KeyboardCommit osk_commit_;

    std::vector<MenuItem>  root_items_;
    std::vector<MenuItem>  quick_items_;   // curated corner "quick menu" tree
    std::vector<Level>     stack_;
    int                    cursor_ = 0;
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

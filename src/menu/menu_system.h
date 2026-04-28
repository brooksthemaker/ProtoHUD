#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <tuple>
#include <cstdint>
#include <imgui.h>

// ── Item types ────────────────────────────────────────────────────────────────

enum class MenuItemType {
    LEAF,          // fires action() and closes menu
    SUBMENU,       // descends into children
    TOGGLE,        // flips a bool; menu stays open; shows ON/OFF indicator
    SLIDER,        // enters numeric edit mode; knob adjusts value
    COLOR_PICKER,  // enters R/G/B channel edit mode
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

// ── Color picker config ───────────────────────────────────────────────────────

struct ColorPickerConfig {
    std::function<void(uint8_t r, uint8_t g, uint8_t b)>            set_color;
    std::function<std::tuple<uint8_t, uint8_t, uint8_t>()>          get_color; // optional seed
};

// ── Menu item ─────────────────────────────────────────────────────────────────

struct MenuItem {
    std::string   label;
    MenuItemType  type = MenuItemType::LEAF;

    // LEAF
    std::function<void()> action;

    // SUBMENU
    std::vector<MenuItem> children;

    // TOGGLE
    std::function<bool()>     get_toggle;
    std::function<void(bool)> set_toggle;
    std::function<bool()>     get_state;  // legacy radio indicator (checked in draw)

    // SLIDER
    SliderConfig slider;

    // COLOR_PICKER
    ColorPickerConfig color;
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
    void set_accent_color(ImU32 c) { accent_color_ = c; }
    void set_bg_enabled(bool e)    { bg_enabled_   = e; }
    void set_bg_color(ImU32 c)     { bg_color_     = c; }

    // Runtime style getters — for persisting user changes to config on exit.
    ImU32 accent_color() const { return accent_color_; }
    ImU32 bg_color()     const { return bg_color_;     }
    bool  bg_enabled()   const { return bg_enabled_;   }

    // Drive from knob events
    void navigate(int direction);   // +1 = next, -1 = prev (or adjust value in edit mode)
    void select();                  // confirm / toggle / enter edit mode
    void back();                    // pop menu level (or exit edit mode)

    // Render the menu overlay using Dear ImGui windows
    void draw(int screen_w, int screen_h);

    bool is_open()    const { return open_; }
    void open()             { open_ = true; push_level(root_items_); }
    void close() {
        open_            = false;
        in_edit_mode_    = false;
        in_channel_edit_ = false;
        stack_.clear();
    }

    int  current_index() const { return cursor_; }
    int  menu_depth()    const { return static_cast<int>(stack_.size()); }
    const std::string& current_label() const;

private:
    struct Level { std::vector<MenuItem> items; };

    void push_level(const std::vector<MenuItem>& items);
    void pop_level();
    void emit_detents();
    void emit_detents_override(int count);

    // ── edit-mode state ───────────────────────────────────────────────────────
    bool  in_edit_mode_    = false;
    float edit_float_      = 0.f;     // working copy for SLIDER
    int   edit_channel_    = 0;       // 0=R 1=G 2=B for COLOR_PICKER
    bool  in_channel_edit_ = false;   // true when knob adjusts channel value
    float edit_r_ = 0.f, edit_g_ = 0.f, edit_b_ = 0.f;

    std::vector<MenuItem>  root_items_;
    std::vector<Level>     stack_;
    int                    cursor_ = 0;
    bool                   open_   = false;
    DetentCallback         detent_cb_;

    // Runtime style
    ImU32 accent_color_ = IM_COL32(255, 160,  32, 255);
    bool  bg_enabled_   = true;
    ImU32 bg_color_     = IM_COL32( 10,  15,  20, 225);
};

#pragma once
// ── item_factories.h ──────────────────────────────────────────────────────────
// Small pure MenuItem factories shared by every menu-tab builder. These were
// local lambdas inside build_menu() before the menu tree moved out of main.cpp;
// they are stateless, so they live here as inline free functions with identical
// semantics.

#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "menu/menu_system.h"

inline MenuItem leaf(std::string lbl, std::function<void()> fn) {
    MenuItem m;
    m.label  = std::move(lbl);
    m.type   = MenuItemType::LEAF;
    m.action = std::move(fn);
    return m;
}

// leaf with a radio-indicator getter — shows filled dot when get_state() is true
inline MenuItem leaf_sel(std::string lbl, std::function<void()> fn,
                         std::function<bool()> state_fn) {
    MenuItem m;
    m.label     = std::move(lbl);
    m.type      = MenuItemType::LEAF;
    m.action    = std::move(fn);
    m.get_state = std::move(state_fn);
    return m;
}

inline MenuItem submenu(std::string lbl, std::vector<MenuItem> ch) {
    MenuItem m;
    m.label    = std::move(lbl);
    m.type     = MenuItemType::SUBMENU;
    m.children = std::move(ch);
    return m;
}

// Attach a context panel to a SUBMENU item.  Returns the same item so
// calls can be chained at the call site:
//   with_panel(submenu("Position", ...), "Eye Position Preview",
//              [&state]( ImDrawList* dl, ImVec2 o, ImVec2 s){ ... })
inline MenuItem with_panel(MenuItem m, std::string title,
                           MenuContextPanelDraw draw) {
    m.context_panel_title = std::move(title);
    m.context_panel_draw  = std::move(draw);
    return m;
}

// Attach a right-pane context description to any item (shown in the deep menu).
//   with_desc(slider(...), "What this changes and why.")
inline MenuItem with_desc(MenuItem m, std::string desc) {
    m.description = std::move(desc);
    return m;
}

// Make an option leaf apply its effect as soon as it's highlighted (live
// preview), so tabbing through zoom/crop/position options updates the preview
// without a select. Reuses the item's own action.
inline MenuItem live(MenuItem m) {
    m.on_highlight = m.action;
    return m;
}

inline MenuItem toggle(std::string lbl,
                       std::function<bool()>     get_fn,
                       std::function<void(bool)> set_fn) {
    MenuItem m;
    m.label      = std::move(lbl);
    m.type       = MenuItemType::TOGGLE;
    m.get_toggle = std::move(get_fn);
    m.set_toggle = std::move(set_fn);
    return m;
}

inline MenuItem slider(std::string lbl,
                       float mn, float mx, float step, std::string unit,
                       std::function<float()>     get_fn,
                       std::function<void(float)> set_fn) {
    MenuItem m;
    m.label            = std::move(lbl);
    m.type             = MenuItemType::SLIDER;
    m.slider.min       = mn;
    m.slider.max       = mx;
    m.slider.step      = step;
    m.slider.unit      = std::move(unit);
    m.slider.get_value = std::move(get_fn);
    m.slider.set_value = std::move(set_fn);
    return m;
}

inline MenuItem color_picker(std::string lbl,
                             std::function<void(uint8_t,uint8_t,uint8_t)> set_fn,
                             std::function<std::tuple<uint8_t,uint8_t,uint8_t>()> get_fn
                                 = nullptr) {
    MenuItem m;
    m.label           = std::move(lbl);
    m.type            = MenuItemType::COLOR_PICKER;
    m.color.set_color = std::move(set_fn);
    m.color.get_color = std::move(get_fn);
    return m;
}

inline MenuItem face_picker(std::string lbl, int face_count,
                            std::function<int()>     get_fn,
                            std::function<void(int)> set_fn) {
    MenuItem m;
    m.label                   = std::move(lbl);
    m.type                    = MenuItemType::FACE_PICKER;
    m.face_picker.face_count  = face_count;
    m.face_picker.get_face    = std::move(get_fn);
    m.face_picker.set_face    = std::move(set_fn);
    return m;
}

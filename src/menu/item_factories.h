#pragma once
// ── item_factories.h ──────────────────────────────────────────────────────────
// Small pure MenuItem factories shared by every menu-tab builder. These were
// local lambdas inside build_menu() before the menu tree moved out of main.cpp;
// they are stateless, so they live here as inline free functions with identical
// semantics.

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "app_state.h"
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

// ── Dynamic placeholder lists ────────────────────────────────────────────────
// MenuSystem levels hold POINTERS into the menu tree, so a list whose contents
// change at runtime must NOT add/remove MenuItems while the menu is open.
// Instead, pre-allocate max_rows placeholder rows up front and let each row
// materialize through its hooks: visible_fn hides rows at/past the live count,
// and the row_builder-supplied label_fn / action read the live data by index.
// Refreshing the backing data happens elsewhere — typically the enclosing
// SUBMENU's on-enter action. max_rows is the hard cap on visible entries;
// bump it at the call site if a list outgrows it.
//
// row_builder(i) returns the row for index i; any visible_fn it already set
// is kept and ANDed with the count gate.
inline std::vector<MenuItem> make_dynamic_rows(
        int max_rows,
        std::function<int()> live_count,
        const std::function<MenuItem(int)>& row_builder) {
    std::vector<MenuItem> rows;
    rows.reserve(max_rows);
    for (int i = 0; i < max_rows; ++i) {
        MenuItem m = row_builder(i);
        std::function<bool()> extra = std::move(m.visible_fn);
        m.visible_fn = [live_count, extra = std::move(extra), i]{
            return i < live_count() && (!extra || extra());
        };
        rows.push_back(std::move(m));
    }
    return rows;
}

// ── Asset-slot management row ────────────────────────────────────────────────
// One parameterized builder for the slot rows under Files > GIFs and
// Face Display > Faces / Mouth Shapes / Boop Reactions. Each row is a SUBMENU
// whose children gate on whether the slot is bound on disk:
//   bound   → Play, Edit…, Versions, Replace…, Copy from…, Clear
//   unbound → Import…
// Per-asset behaviour goes through the descriptor: callbacks left null (and
// optionals left empty) drop that child entirely, so e.g. GIF slots get no
// Edit/Versions/Copy and mouth shapes get no Play. Replace… and Import… share
// import_action — they have always been the same operation on every slot type.
struct AssetSlotRowDesc {
    std::string                  label;         // static fallback / id
    std::function<std::string()> label_fn;      // dynamic label (manifest/disk state)
    std::function<bool()>        exists;        // slot bound on disk? (gates children)
    std::function<void()>        on_highlight;  // preview hook (slot thumbnail panes)
    std::function<void()>        play;          // null → no Play row
    std::function<void()>        edit;          // null → no Edit… row
    std::function<bool()>        edit_visible;  // capability gate for Edit… (e.g.
                                                // only backends with LED regions)
    std::optional<MenuItem>      versions;      // pre-built Versions submenu
    std::optional<MenuItem>      copy_from;     // pre-built Copy from… submenu
    std::function<void()>        import_action; // shared by Replace… and Import…
    std::function<void()>        clear;
};

inline MenuItem make_asset_slot_row(AssetSlotRowDesc d) {
    MenuItem m;
    m.type         = MenuItemType::SUBMENU;
    m.label        = std::move(d.label);
    m.label_fn     = std::move(d.label_fn);
    m.on_highlight = std::move(d.on_highlight);

    const std::function<bool()> exists = std::move(d.exists);

    if (d.play) {
        MenuItem play = leaf("Play", std::move(d.play));
        play.visible_fn = exists;
        m.children.push_back(std::move(play));
    }
    if (d.edit) {
        MenuItem edit_it = leaf("Edit...", std::move(d.edit));
        edit_it.visible_fn = std::move(d.edit_visible);
        m.children.push_back(std::move(edit_it));
    }
    if (d.versions) {
        MenuItem versions = std::move(*d.versions);
        versions.visible_fn = exists;
        m.children.push_back(std::move(versions));
    }
    {
        MenuItem replace = leaf("Replace...", d.import_action);
        replace.visible_fn = exists;
        m.children.push_back(std::move(replace));
    }
    if (d.copy_from) m.children.push_back(std::move(*d.copy_from));
    {
        MenuItem clear = leaf("Clear", std::move(d.clear));
        clear.visible_fn = exists;
        m.children.push_back(std::move(clear));
    }
    {
        MenuItem imp = leaf("Import...", std::move(d.import_action));
        imp.visible_fn = [exists]{ return !exists(); };
        m.children.push_back(std::move(imp));
    }
    return m;
}

// ── Overlay placement factories ──────────────────────────────────────────────
// Shared by the Vision tab (camera PiPs, Android mirror) and the Face Display
// tab (Protoface panel preview placement). Were local lambdas in build_menu().

// Snap position presets — sets anchor_x/y and resets any pan offset.
inline std::vector<MenuItem> make_position_items(OverlayConfig* cfg) {
    auto snap = [cfg](float ax, float ay){
        cfg->anchor_x = ax; cfg->anchor_y = ay;
        cfg->pan_x = 0.f;   cfg->pan_y = 0.f;
    };
    auto at   = [cfg](float ax, float ay){
        return std::abs(cfg->anchor_x - ax) < 0.01f &&
               std::abs(cfg->anchor_y - ay) < 0.01f;
    };
    std::vector<MenuItem> nudge = {
        leaf("Left  -10px", [cfg]{ cfg->pan_x -= 10.f; }),
        leaf("Right +10px", [cfg]{ cfg->pan_x += 10.f; }),
        leaf("Up    -10px", [cfg]{ cfg->pan_y -= 10.f; }),
        leaf("Down  +10px", [cfg]{ cfg->pan_y += 10.f; }),
        leaf("Left  -50px", [cfg]{ cfg->pan_x -= 50.f; }),
        leaf("Right +50px", [cfg]{ cfg->pan_x += 50.f; }),
        leaf("Up    -50px", [cfg]{ cfg->pan_y -= 50.f; }),
        leaf("Down  +50px", [cfg]{ cfg->pan_y += 50.f; }),
        leaf("Reset Nudge", [cfg]{ cfg->pan_x = 0.f; cfg->pan_y = 0.f; }),
    };
    return std::vector<MenuItem>{
        leaf_sel("Top Left",      [snap]{ snap(0.0f, 0.0f); }, [cfg, at]{ return at(0.0f, 0.0f); }),
        leaf_sel("Top Center",    [snap]{ snap(0.5f, 0.0f); }, [cfg, at]{ return at(0.5f, 0.0f); }),
        leaf_sel("Top Right",     [snap]{ snap(1.0f, 0.0f); }, [cfg, at]{ return at(1.0f, 0.0f); }),
        leaf_sel("Center Left",   [snap]{ snap(0.0f, 0.5f); }, [cfg, at]{ return at(0.0f, 0.5f); }),
        leaf_sel("Center",        [snap]{ snap(0.5f, 0.5f); }, [cfg, at]{ return at(0.5f, 0.5f); }),
        leaf_sel("Center Right",  [snap]{ snap(1.0f, 0.5f); }, [cfg, at]{ return at(1.0f, 0.5f); }),
        leaf_sel("Bottom Left",   [snap]{ snap(0.0f, 1.0f); }, [cfg, at]{ return at(0.0f, 1.0f); }),
        leaf_sel("Bottom Center", [snap]{ snap(0.5f, 1.0f); }, [cfg, at]{ return at(0.5f, 1.0f); }),
        leaf_sel("Bottom Right",  [snap]{ snap(1.0f, 1.0f); }, [cfg, at]{ return at(1.0f, 1.0f); }),
        submenu("Nudge", std::move(nudge)),
    };
}

inline MenuItem make_size_slider(std::string lbl, OverlayConfig* cfg) {
    return slider(std::move(lbl), 15.f, 60.f, 5.f, " %",
        [cfg]{ return cfg->size * 100.f; },
        [cfg](float v){ cfg->size = v / 100.f; });
}

inline std::vector<MenuItem> make_rotation_items(OverlayConfig* cfg) {
    using R = OverlayConfig::Rotation;
    return std::vector<MenuItem>{
        leaf_sel("Landscape",         [cfg]{ cfg->rotation = R::Landscape;        }, [cfg]{ return cfg->rotation == R::Landscape;        }),
        leaf_sel("Portrait",          [cfg]{ cfg->rotation = R::Portrait;          }, [cfg]{ return cfg->rotation == R::Portrait;          }),
        leaf_sel("Landscape Flipped", [cfg]{ cfg->rotation = R::LandscapeFlipped; }, [cfg]{ return cfg->rotation == R::LandscapeFlipped; }),
        leaf_sel("Portrait Flipped",  [cfg]{ cfg->rotation = R::PortraitFlipped;  }, [cfg]{ return cfg->rotation == R::PortraitFlipped;  }),
    };
}

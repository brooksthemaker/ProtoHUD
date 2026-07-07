// ── shared_items.cpp ──────────────────────────────────────────────────────────
// Single source of truth for menu items that appear in both the deep tabs and
// the quick (corner/radial) menu. See shared_items.h for the per-item notes.

#include "menu/shared_items.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <utility>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#include "camera/camera_manager.h"
#include "integrations/kdeconnect_bridge.h"
#include "menu/item_factories.h"

namespace menu_shared {

MenuItem night_vision_toggle(AppState& state, std::string label) {
    return toggle(std::move(label),
        [&state]{ return state.night_vision.nv_enabled; },
        [&state](bool v){
            state.night_vision.nv_enabled  = v;
            state.night_vision.exposure_ev = v ? 3.0f : 0.0f;
            state.night_vision.shutter_us  = v ? 40000 : 16667;
        });
}

void set_both_eye_zoom(AppState& state, float zoom) {
    state.zoom_left.zoom  = zoom;
    state.zoom_right.zoom = zoom;
}

MenuItem record_toggle(AppState& state) {
    return toggle("Record",
        [&state]{ return state.video_recording; },
        [&state](bool v){
            std::lock_guard<std::mutex> lk(state.mtx);
            state.video_request = v ? VideoRequest::Start : VideoRequest::Stop;
        });
}

MenuItem theater_toggle(AppState& state, std::string label) {
    return toggle(std::move(label),
        [&state]{ return state.theater_mode; },
        [&state](bool v){ state.theater_mode = v; });
}

MenuItem swap_cameras_toggle(AppState& state) {
    return toggle("Swap Cameras",
        [&state]{ return state.cameras_swapped; },
        [&state](bool v){ state.cameras_swapped = v; });
}

MenuItem edge_highlight_toggle(AppState& state) {
    return toggle("Edge Highlight",
        [&state]{ return state.pp_cfg.edge_enabled; },
        [&state](bool v){ state.pp_cfg.edge_enabled = v; });
}

MenuItem motion_highlight_toggle(AppState& state) {
    return toggle("Motion Highlight",
        [&state]{ return state.pp_cfg.motion_enabled; },
        [&state](bool v){ state.pp_cfg.motion_enabled = v; });
}

MenuItem desaturate_toggle(AppState& state, std::string label) {
    return toggle(std::move(label),
        [&state]{ return state.pp_cfg.desat_enabled; },
        [&state](bool v){ state.pp_cfg.desat_enabled = v; });
}

MenuItem fps_overlay_toggle(bool* fps_overlay_active) {
    return toggle("FPS Overlay",
        [fps_overlay_active]{ return fps_overlay_active && *fps_overlay_active; },
        [fps_overlay_active](bool v){ if (fps_overlay_active) *fps_overlay_active = v; });
}

MenuItem system_panel_toggle(bool* sys_panel_active) {
    return toggle("System Panel",
        [sys_panel_active]{ return sys_panel_active && *sys_panel_active; },
        [sys_panel_active](bool v){ if (sys_panel_active) *sys_panel_active = v; });
}

std::function<void()> capture_action(AppState* state_ptr,
                                     CaptureRequest which, int burst_extra) {
    return [state_ptr, which, burst_extra]{
        if (!state_ptr) return;
        std::lock_guard<std::mutex> lk(state_ptr->mtx);
        state_ptr->capture_request = which;
        if (burst_extra >= 0) state_ptr->capture_burst = burst_extra;
    };
}

MenuItem expand_map_leaf(AppState* state_ptr, std::string label,
                         MenuSystem** close_menu) {
    return leaf(std::move(label), [state_ptr, close_menu]{
        if (state_ptr) {
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->map_overlay.expanded   = true;
            state_ptr->map_overlay.view_zoom  = 1.f;
            state_ptr->map_overlay.view_pan_x = 0.f;
            state_ptr->map_overlay.view_pan_y = 0.f;
        }
        if (close_menu && *close_menu) (*close_menu)->close();
    });
}

std::vector<MenuItem> timer_preset_items(AppState& state,
                                         bool show_selected_state,
                                         std::string cancel_label) {
    auto set_timer = [&state](int secs){
        state.timer_alarm.timer_active = true;
        state.timer_alarm.timer_end    = time(nullptr) + secs;
    };
    // Selected (deep tab) when the running countdown's remaining time falls
    // in (lo, hi]; hi < 0 = unbounded so the 60-min row also claims longer
    // custom countdowns, matching the original deep behaviour.
    struct Preset { const char* label; int secs; long lo; long hi; };
    static constexpr Preset kPresets[] = {
        { "5 min",   300,    0,  300 },
        { "10 min",  600,  300,  600 },
        { "30 min", 1800,  600, 1800 },
        { "60 min", 3600, 1800,   -1 },
    };
    std::vector<MenuItem> items;
    for (const auto& p : kPresets) {
        auto fire = [set_timer, secs = p.secs]{ set_timer(secs); };
        if (show_selected_state) {
            items.push_back(leaf_sel(p.label, std::move(fire),
                [&state, lo = p.lo, hi = p.hi]{
                    if (!state.timer_alarm.timer_active) return false;
                    const long rem = state.timer_alarm.timer_end - time(nullptr);
                    return rem > lo && (hi < 0 || rem <= hi);
                }));
        } else {
            items.push_back(leaf(p.label, std::move(fire)));
        }
    }
    items.push_back(leaf(std::move(cancel_label),
        [&state]{ state.timer_alarm.timer_active = false; }));
    return items;
}

MenuItem reinit_csi_leaf(CameraManager* cameras, AppState& state,
                         std::string label, bool live_status_label) {
    MenuItem m;
    m.type  = MenuItemType::LEAF;
    m.label = std::move(label);
    if (live_status_label) {
        m.label_fn = [cameras]{
            std::string s = "Reinitialize CSI  [L:";
            s += (cameras && cameras->owl_left_ok())  ? "ok" : "\xE2\x80\x94";
            s += " R:";
            s += (cameras && cameras->owl_right_ok()) ? "ok" : "\xE2\x80\x94";
            return s + "]";
        };
    }
    m.action = [cameras, &state]{
        if (!cameras) return;
        const bool ok  = cameras->reinit_owls();
        const bool lok = cameras->owl_left_ok(), rok = cameras->owl_right_ok();
        std::lock_guard<std::mutex> lk(state.mtx);
        Notification n;
        n.type  = NotifType::App;
        n.title = ok ? "CSI cameras reinitialized" : "CSI reinit: no camera found";
        char b[64];
        snprintf(b, sizeof(b), "Left %s  \xC2\xB7  Right %s",
                 lok ? "OK" : "\xE2\x80\x94", rok ? "OK" : "\xE2\x80\x94");
        n.body = b;
        n.auto_dismiss_s = 5.f;
        state.notifs.push(std::move(n));
    };
    return m;
}

MenuItem restart_face_renderer_leaf(std::function<void()> restart,
                                    AppState* state_ptr,
                                    const std::string* backend_p) {
    MenuItem rr = leaf("Restart Face Renderer",
        [restart = std::move(restart), state_ptr]{
            restart();
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            Notification n;
            n.type  = NotifType::App;
            n.title = "Face renderer restarted";
            n.body  = "Relaunched the HUB75 panel driver";
            n.auto_dismiss_s = 4.f;
            state_ptr->notifs.push(std::move(n));
        });
    rr.visible_fn = [backend_p]{ return !backend_p || *backend_p == "hub75"; };
    return rr;
}

std::function<void()> ring_phone_action(integrations::KdeConnectBridge* kdc,
                                        AppState& state) {
    return [kdc, &state]{
        const bool ok = kdc && kdc->ring_phone();
        std::lock_guard<std::mutex> lk(state.mtx);
        Notification n;
        n.type = NotifType::App; n.icon = "message";
        n.title = ok ? "Ringing phone\xE2\x80\xA6" : "Phone not connected";
        n.body  = ok ? "KDE Connect \xC2\xB7 findmyphone"
                     : "Pair a device in the KDE Connect app first";
        n.auto_dismiss_s = 4.f;
        state.notifs.push(std::move(n));
    };
}

bool i2c_probe_addr(int fd, int addr) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) return false;
    union i2c_smbus_data data{};
    i2c_smbus_ioctl_data args{};
    if ((addr >= 0x30 && addr <= 0x37) || (addr >= 0x50 && addr <= 0x5F)) {
        args.read_write = I2C_SMBUS_READ;    // read-byte: safe for EEPROMs/RTCs
        args.size       = I2C_SMBUS_BYTE;
        args.data       = &data;
    } else {
        args.read_write = I2C_SMBUS_WRITE;   // quick-write: a bare addr+W ACK
        args.size       = I2C_SMBUS_QUICK;
        args.data       = nullptr;
    }
    args.command = 0;
    return ioctl(fd, I2C_SMBUS, &args) >= 0;
}

} // namespace menu_shared

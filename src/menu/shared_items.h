#pragma once
// ── shared_items.h ────────────────────────────────────────────────────────────
// Item definitions used by BOTH a deep tab and the quick (corner/radial) menu.
// The quick tree used to re-declare these inline and the two copies drifted;
// now each shared behaviour is defined exactly once here and both surfaces
// call the factory. Labels / descriptions may legitimately differ per surface,
// so they're parameters where they do. Where the two surfaces intentionally
// behave differently, the difference is an explicit parameter (see the
// comments at each call site).

#include <functional>
#include <string>
#include <vector>

#include "app_state.h"
#include "capture.h"
#include "menu/menu_system.h"

class CameraManager;
class MenuSystem;
namespace integrations { class KdeConnectBridge; }

namespace menu_shared {

// Low-light boost: flips nv_enabled and seeds exposure_ev / shutter_us to the
// boosted (or daylight) defaults. Deep: Vision > Low-Light Mode > "Enable
// Low-Light"; quick: "Night Vision".
MenuItem night_vision_toggle(AppState& state, std::string label);

// Digital zoom applies to both eyes together. The deep tab exposes preset
// leaves, the quick menu a continuous slider — both go through this setter so
// the "both eyes move together" semantics can't drift.
void set_both_eye_zoom(AppState& state, float zoom);

// Start/stop video recording via state.video_request.
MenuItem record_toggle(AppState& state);

// Theater mode (raw camera passthrough). Deep: Raw View > "Enable";
// quick favorites: "Theater Mode".
MenuItem theater_toggle(AppState& state, std::string label);

MenuItem swap_cameras_toggle(AppState& state);

// Vision-assist post-process toggles (also pinned via quick favorites).
MenuItem edge_highlight_toggle(AppState& state);
MenuItem motion_highlight_toggle(AppState& state);
// Quick says "Desaturate BG", deep says "Bg Desaturate" — label is a param.
MenuItem desaturate_toggle(AppState& state, std::string label);

MenuItem fps_overlay_toggle(bool* fps_overlay_active);
MenuItem system_panel_toggle(bool* sys_panel_active);

// Photo capture request. burst_extra >= 0 also (re)sets capture_burst (the
// quick-photo burst modes); burst_extra < 0 leaves any pending burst counter
// untouched — the deep Capture Photo leaves never wrote it.
std::function<void()> capture_action(AppState* state_ptr,
                                     CaptureRequest which, int burst_extra);

// Open the Helldivers-style pan/zoom map view (resets view zoom/pan).
// close_menu non-null → also closes the menu on select (the quick wheel
// closes itself so the expanded map is immediately interactive; the deep
// menu passes nullptr and stays open).
MenuItem expand_map_leaf(AppState* state_ptr, std::string label,
                         MenuSystem** close_menu);

// 5 / 10 / 30 / 60-minute countdown presets plus a cancel leaf.
// show_selected_state adds the deep tab's "which preset is running"
// radio indicators; the quick wheel passes false (plain leaves).
std::vector<MenuItem> timer_preset_items(AppState& state,
                                         bool show_selected_state,
                                         std::string cancel_label);

// Tear down + re-enumerate + restart both CSI cameras, with a result toast.
// live_status_label adds the deep tab's dynamic "[L:ok R:—]" label suffix.
MenuItem reinit_csi_leaf(CameraManager* cameras, AppState& state,
                         std::string label, bool live_status_label);

// Kill + relaunch the HUB75 panel pusher, with a toast. Hidden unless the
// active Protoface backend is hub75. Call sites attach their own description.
MenuItem restart_face_renderer_leaf(std::function<void()> restart,
                                    AppState* state_ptr,
                                    const std::string* backend_p);

// Ring the paired phone via KDE Connect and toast the outcome.
std::function<void()> ring_phone_action(integrations::KdeConnectBridge* kdc,
                                        AppState& state);

// i2cdetect-style presence probe of one address on an already-open bus fd.
// A plain read() misreports on the Pi: a NACK comes back as EREMOTEIO, which
// the old "any errno but ENODEV/ENXIO means present" test counted as found —
// every address probed as occupied. This issues the same SMBus transactions
// i2cdetect does (quick-write for most addresses; read-byte in the
// EEPROM/RTC ranges where a stray quick-write can corrupt state) and only
// trusts an ACKed transfer. Used by the IMU scan and the Diagnostics
// full-bus scanner.
bool i2c_probe_addr(int fd, int addr);

} // namespace menu_shared

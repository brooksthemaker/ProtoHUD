// ── build_communications.cpp ──────────────────────────────────────────────────
// The Communications tab, moved verbatim out of build_menu(): LoRa team mesh,
// the Phone (KDE Connect) item (handed to the System builder through
// ctx.phone_item — it is shown under System > Connectivity), the notification
// log, and the scanned QR codes browser.

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <random>
#include <cctype>
#include <ctime>
#include <csignal>
#include <cstdlib>

#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>
#include <linux/videodev2.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>

#include "app_state.h"
#include "android/android_mirror.h"
#include "camera/camera_manager.h"
#include "camera/viture_camera.h"
#include "input/gpio_buttons.h"
#include "input/gpio_inputs.h"
#include "input/coproc_inputs.h"
#include "input/cmd_fifo.h"
#include "input/gpio_function.h"
#include "input/gamepad_input.h"
#include "input/wireless_controller.h"
#include "serial/face_controller.h"
#include "serial/protoface_controller.h"
#include "serial/teensy_controller.h"
#include "serial/lora_radio.h"
#include "serial/smartknob.h"
#include "protocols.h"
#include "hud/hud_renderer.h"
#include "menu/menu_system.h"
#include "vitrue/xr_display.h"
#include "vitrue/timewarp.h"
#include "audio/audio_engine.h"
#include "post_process.h"
#include "sensor/mpu9250.h"
#include "sensor/bno055.h"
#include "sensor/light_sensor.h"
#include "sensor/mpr121_boop_sensor.h"
#include "accessory/accessory_leds.h"
#include "sys/fan_controller.h"
#include "sys/system_monitor.h"
#include "sys/scheduler_monitor.h"
#include "sys/gpio_pinmap.h"
#include "sys/gpio_input_reader.h"
#include "net/weather_monitor.h"
#include "net/wifi_monitor.h"
#include "net/ping_monitor.h"
#include "net/bt_monitor.h"
#include "crash_reporter.h"
#include "capture.h"
#include "gl_async_read.h"
#include "video_recorder.h"
#include "qr_scanner.h"
#include "splash.h"
#include "integrations/kdeconnect_bridge.h"   // header is dbus-free; .cpp guarded by HAVE_DBUS in CMake
#include "integrations/phone_inbox.h"
#include "hud/background_library.h"
#include "profile_manager.h"
#include "face/face_config.h"
#include "face/face_image.h"
#include "face/eye_animations.h"
#include "face/gif_player.h"
#include "face/native_face_controller.h"
#include "face/panel_output.h"
#include "face/shm_pusher_output.h"
#include "face/max7219_panel_output.h"
#include "face/neopixel_matrix_output.h"

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/file.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

using json = nlohmann::json;

#include "menu/build_menu.h"
#include "menu/item_factories.h"

std::vector<MenuItem> build_communications_menu(MenuBuildContext& ctx)
{
    // ── Parameter aliases ─────────────────────────────────────────────────────
    // Re-create build_menu's original parameter names as locals so the moved
    // body below stays byte-for-byte identical to its main.cpp version.
    // By-value parameters become local copies with the same lifetime they
    // always had (the duration of the build).
    IFaceController* teensy  = ctx.teensy;
    XRDisplay*       xr      = ctx.xr;
    CameraManager*   cameras = ctx.cameras;
    LoRaRadio*       lora    = ctx.lora;
    SmartKnob*       knob    = ctx.knob;
    AudioEngine*     audio   = ctx.audio;
    AppState&        state   = *ctx.state;
    AndroidMirror*   android_mirror  = ctx.android_mirror;
    bool*            android_overlay = ctx.android_overlay;
    OverlayConfig*   pip_cfg1 = ctx.pip_cfg1;
    OverlayConfig*   pip_cfg2 = ctx.pip_cfg2;
    OverlayConfig*   pip_cfg3 = ctx.pip_cfg3;
    bool*            pip_cam1_overlay = ctx.pip_cam1_overlay;
    bool*            pip_cam2_overlay = ctx.pip_cam2_overlay;
    bool*            pip_cam3_overlay = ctx.pip_cam3_overlay;
    OverlayConfig*   android_cfg = ctx.android_cfg;
    HudColors*       hud_col = ctx.hud_col;
    HudConfig*       hud_cfg = ctx.hud_cfg;
    MenuSystem**     menu_sys_pp = ctx.menu_sys_pp;
    Mpu9250*         mpu9250 = ctx.mpu9250;
    Bno055*          bno055  = ctx.bno055;
    const std::vector<std::string>& gif_names = *ctx.gif_names;
    BtMonitor*       bt_mon = ctx.bt_mon;
    bool*            sys_panel_active   = ctx.sys_panel_active;
    bool*            fps_overlay_active = ctx.fps_overlay_active;
    AppState*        state_ptr = ctx.state_ptr;
    IFaceController** active_face_pp = ctx.active_face_pp;
    IFaceController*  teensy_option  = ctx.teensy_option;
    IFaceController*  fp_option      = ctx.fp_option;
    bool*             panel_preview_pp = ctx.panel_preview_pp;
    OverlayConfig*    protoface_preview_cfg = ctx.protoface_preview_cfg;
    int*              protoface_preview_view_pp = ctx.protoface_preview_view_pp;
    std::string       map_dir = ctx.map_dir;
    EyeSource* left_eye_src  = ctx.left_eye_src;
    EyeSource* right_eye_src = ctx.right_eye_src;
    bool*      multicam_layout = ctx.multicam_layout;
    EyeSource* multicam_usb_a  = ctx.multicam_usb_a;
    EyeSource* multicam_usb_b  = ctx.multicam_usb_b;
    EyeSource* multicam_top_a  = ctx.multicam_top_a;
    EyeSource* multicam_top_b  = ctx.multicam_top_b;
    ProfileManager* profiles    = ctx.profiles;
    ProfileManager* hud_presets = ctx.hud_presets;
    std::vector<MenuItem>* quick_out = ctx.quick_out;
    std::string gifs_dir = ctx.gifs_dir;
    BackgroundLibrary** bg_lib_pp = ctx.bg_lib_pp;
    std::string bg_user_dir = ctx.bg_user_dir;
    sensor::BoopSensor** boop_sensor_pp = ctx.boop_sensor_pp;
    audio::VoiceAnalyzer* voice_analyzer = ctx.voice_analyzer;
    accessory::AccessoryLeds* leds = ctx.leds;
    sys::FanController* fans = ctx.fans;
    std::function<void(const std::string&)> swap_backend = ctx.swap_backend;
    const std::string* pf_backend_p = ctx.pf_backend_p;
    std::function<void(const std::string&)> edit_face = ctx.edit_face;
    std::string* pf_eye_layout_p   = ctx.pf_eye_layout_p;
    std::string* pf_mouth_layout_p = ctx.pf_mouth_layout_p;
    std::string* pf_nose_layout_p  = ctx.pf_nose_layout_p;
    PfHub75Layout* pf_hub75_p = ctx.pf_hub75_p;
    std::map<std::string, PfHub75Layout>* pf_hub75_layouts_p = ctx.pf_hub75_layouts_p;
    std::string* pf_hub75_active_p = ctx.pf_hub75_active_p;
    std::function<void()> pf_layout_changed = ctx.pf_layout_changed;
    bool*   pf_blink_enabled_p = ctx.pf_blink_enabled_p;
    double* pf_blink_min_p     = ctx.pf_blink_min_p;
    double* pf_blink_max_p     = ctx.pf_blink_max_p;
    double* pf_blink_dur_p     = ctx.pf_blink_dur_p;
    double* pf_expr_fade_p     = ctx.pf_expr_fade_p;
    double* pf_preview_duration_p = ctx.pf_preview_duration_p;
    std::function<void()> pf_anim_push = ctx.pf_anim_push;
    std::function<void(const nlohmann::json&)> pf_set_effect_json = ctx.pf_set_effect_json;
    std::function<void(bool)> pf_set_expr_effects = ctx.pf_set_expr_effects;
    bool* pf_expr_effects_p = ctx.pf_expr_effects_p;
    std::shared_ptr<std::function<void()>> pf_live_tick = ctx.pf_live_tick;
    nlohmann::json* cfg_root = ctx.cfg_root;
    GLuint* tex_usb1 = ctx.tex_usb1;
    GLuint* tex_usb2 = ctx.tex_usb2;
    GLuint* tex_usb3 = ctx.tex_usb3;
    int*    usb_preview_req = ctx.usb_preview_req;
    PfGradient* pf_gradient_p = ctx.pf_gradient_p;
    std::function<void(const std::string&)> pf_set_material = ctx.pf_set_material;
    std::function<void()> pf_restart_renderer = ctx.pf_restart_renderer;
    input::GpioPinCfg* gpio_pins_p = ctx.gpio_pins_p;
    int   gpio_slot_count = ctx.gpio_slot_count;
    bool* gpio_inputs_enabled_p = ctx.gpio_inputs_enabled_p;
    std::shared_ptr<std::function<void()>> gpio_reload = ctx.gpio_reload;
    integrations::KdeConnectBridge* kdc_p = ctx.kdc_p;
    std::vector<std::string>* kdc_ignore_p = ctx.kdc_ignore_p;
    std::vector<std::string>* kdc_msg_p = ctx.kdc_msg_p;
    bool* coproc_enabled_p = ctx.coproc_enabled_p;
    std::shared_ptr<std::function<void()>> coproc_reload = ctx.coproc_reload;
    std::shared_ptr<std::function<std::string()>> coproc_status = ctx.coproc_status;
    face::GlitchConfig* pf_glitch_p = ctx.pf_glitch_p;

    // Shared notification helper from the build context, re-wrapped locally so
    // call sites keep the optional trailing actions argument.
    auto push_notif = [pn = ctx.push_notif](NotifType type, std::string title,
                                            std::string body, float auto_dismiss_s,
                                            std::vector<NotifAction> actions = {}) {
        pn(type, std::move(title), std::move(body), auto_dismiss_s, std::move(actions));
    };

    // ── LoRa menu ─────────────────────────────────────────────────────────────


    // Node lookup: one leaf per possible node slot (1–8); hidden when no data.
    // Selecting pushes an App toast with live telemetry at that moment.
    std::vector<MenuItem> lora_nodes_menu;
    for (int n = 1; n <= 8; ++n) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "Node %d", n);
        lora_nodes_menu.push_back({
            .label      = lbl,
            .type       = MenuItemType::LEAF,
            .action     = [n, state_ptr, push_notif]{
                if (!state_ptr) return;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                for (const auto& nd : state_ptr->lora_nodes) {
                    if (nd.local_id != static_cast<uint8_t>(n)) continue;
                    char body[128];
                    char since[32] = "never";
                    if (nd.last_seen) {
                        int secs = static_cast<int>(time(nullptr) - nd.last_seen);
                        if (secs < 60)       snprintf(since, sizeof(since), "%ds ago", secs);
                        else if (secs < 3600) snprintf(since, sizeof(since), "%dm ago", secs/60);
                        else                  snprintf(since, sizeof(since), "%dh ago", secs/3600);
                    }
                    snprintf(body, sizeof(body),
                             "%.0fm  %.0f\xC2\xB0  RSSI %d dBm  SNR %d  %s",
                             static_cast<double>(nd.distance_m),
                             static_cast<double>(nd.heading_deg),
                             static_cast<int>(nd.rssi),
                             static_cast<int>(nd.snr),
                             since);
                    std::string title = nd.name.empty()
                                        ? std::string("Node ") + std::to_string(n)
                                        : nd.name;
                    push_notif(NotifType::LoRa, std::move(title), body, 6.f);
                    return;
                }
                // Node slot known to menu but not yet seen
                push_notif(NotifType::App, std::string("Node ") + std::to_string(n),
                           "No data received yet", 4.f);
            },
            .visible_fn = [n, state_ptr]{
                if (!state_ptr) return false;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                for (const auto& nd : state_ptr->lora_nodes)
                    if (nd.local_id == static_cast<uint8_t>(n)) return true;
                return false;
            },
        });
    }

    std::vector<MenuItem> lora_menu = {
        leaf("Clear Messages",      [state_ptr]{
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->lora_messages.clear();
        }),
        leaf("Clear Notifications", [state_ptr]{
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->notifs.dismiss_all();
        }),
        submenu("Lookup Node",      std::move(lora_nodes_menu)),
    };

    // ── Communications: LoRa + Phone (KDE Connect) + Notification Log ─────────
    MenuItem phone_item = [&]() -> MenuItem {
            // ── Phone (KDE Connect) ───────────────────────────────────────────
            // Ring the phone + edit the ignore list (mute servers/chats) and the
            // message-apps list (which apps get the big chat toast), from the HUD.
            auto ring_toast = [kdc_p, &state]{
                const bool ok = kdc_p && kdc_p->ring_phone();
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n;
                n.type = NotifType::App; n.icon = "message";
                n.title = ok ? "Ringing phone\xE2\x80\xA6" : "Phone not connected";
                n.body  = ok ? "KDE Connect \xC2\xB7 findmyphone"
                             : "Pair a device in the KDE Connect app first";
                n.auto_dismiss_s = 4.f;
                state.notifs.push(std::move(n));
            };
            // Generic CSV-list editor: an "Add… (keyboard)" row plus up to 16
            // removable entry rows. apply() pushes the joined CSV to the bridge.
            // menu_sys_pp is captured by value (it points at main's menu_ptr).
            auto list_menu = [menu_sys_pp](std::vector<std::string>* vec,
                                           std::function<void()> apply,
                                           std::string add_label, std::string osk_title) {
                std::vector<MenuItem> items;
                MenuItem add; add.type = MenuItemType::LEAF; add.label = std::move(add_label);
                add.action = [menu_sys_pp, vec, apply, osk_title]{
                    if (!menu_sys_pp || !*menu_sys_pp || !vec) return;
                    (*menu_sys_pp)->open_keyboard(osk_title, "",
                        [vec, apply](const std::string& s){
                            const size_t b = s.find_first_not_of(" \t");
                            const size_t e = s.find_last_not_of(" \t");
                            if (b == std::string::npos) return;
                            vec->push_back(s.substr(b, e - b + 1));
                            apply();
                        });
                };
                items.push_back(std::move(add));
                for (int i = 0; i < 16; ++i) {
                    MenuItem m; m.type = MenuItemType::LEAF; m.label = "entry";
                    m.label_fn   = [vec, i]{
                        return (vec && i < static_cast<int>(vec->size()))
                                   ? ("\xE2\x9C\x95  " + (*vec)[i]) : std::string(); };
                    m.visible_fn = [vec, i]{ return vec && i < static_cast<int>(vec->size()); };
                    m.action     = [vec, i, apply]{
                        if (vec && i < static_cast<int>(vec->size())) {
                            vec->erase(vec->begin() + i); apply(); } };
                    items.push_back(std::move(m));
                }
                return items;
            };
            auto join = [](std::vector<std::string>* v){
                std::string csv;
                if (v) for (size_t i = 0; i < v->size(); ++i) { if (i) csv += ','; csv += (*v)[i]; }
                return csv;
            };
            auto apply_ignore = [kdc_p, kdc_ignore_p, join]{
                if (kdc_p) kdc_p->set_ignore_list(join(kdc_ignore_p)); };
            auto apply_msg = [kdc_p, kdc_msg_p, join]{
                if (kdc_p) kdc_p->set_message_apps(join(kdc_msg_p)); };

            // Small toast helper for action feedback.
            auto toast = [&state](std::string t, std::string b){
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App; n.icon = "message";
                n.title = std::move(t); n.body = std::move(b); n.auto_dismiss_s = 4.f;
                state.notifs.push(std::move(n));
            };

            // ── Notifications (reply / dismiss) ────────────────────────────────
            // Each row mirrors a recent phone notification; Reply (messaging apps)
            // opens the OSK, Dismiss clears it on the phone.
            std::vector<MenuItem> notif_menu;
            notif_menu.push_back(with_desc(leaf("Dismiss All", [kdc_p, toast]{
                if (kdc_p && kdc_p->dismiss_notification("")) toast("Dismissed", "Cleared phone notifications");
            }), "Dismiss every active notification on the phone."));
            for (int i = 0; i < 12; ++i) {
                MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "notif";
                row.label_fn = [kdc_p, i]{
                    if (!kdc_p) return std::string();
                    auto v = kdc_p->phone_notifications();
                    if (i >= (int)v.size()) return std::string();
                    const auto& n = v[i];
                    std::string who = !n.title.empty() ? n.title
                                    : (!n.app.empty() ? n.app : std::string("Phone"));
                    return who + (n.repliable ? "  \xE2\x9C\x89" : "");
                };
                row.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->phone_notifications().size(); };
                MenuItem reply = leaf("Reply\xE2\x80\xA6", [kdc_p, i, menu_sys_pp, toast]{
                    if (!kdc_p || !menu_sys_pp || !*menu_sys_pp) return;
                    auto v = kdc_p->phone_notifications();
                    if (i >= (int)v.size()) return;
                    const std::string id = v[i].id, who = v[i].title;
                    (*menu_sys_pp)->open_keyboard("Reply to " + who, "",
                        [kdc_p, id, toast](const std::string& s){
                            if (s.empty()) return;
                            if (kdc_p->reply_notification(id, s)) toast("Reply sent", s);
                        });
                });
                reply.visible_fn = [kdc_p, i]{
                    auto v = kdc_p ? kdc_p->phone_notifications() : std::vector<integrations::KdeConnectBridge::PhoneNotif>{};
                    return i < (int)v.size() && v[i].repliable; };
                MenuItem dismiss = leaf("Dismiss", [kdc_p, i, toast]{
                    if (!kdc_p) return;
                    auto v = kdc_p->phone_notifications();
                    if (i < (int)v.size() && kdc_p->dismiss_notification(v[i].id))
                        toast("Dismissed", v[i].title);
                });
                // Context panel: show the full body text of the focused notification.
                row.children = { std::move(reply), std::move(dismiss) };
                notif_menu.push_back(std::move(row));
            }

            // ── Media control (mprisremote) ────────────────────────────────────
            std::vector<MenuItem> media_menu;
            {
                MenuItem now; now.type = MenuItemType::LEAF; now.label = "now playing";
                now.label_fn = [kdc_p]{
                    if (!kdc_p) return std::string("No media");
                    auto m = kdc_p->media_status();
                    if (!m.has_player) return std::string("No media");
                    std::string s = m.playing ? "\xE2\x96\xB6 " : "\xE2\x9D\x9A ";
                    s += !m.title.empty() ? m.title : std::string("(unknown)");
                    if (!m.artist.empty()) s += " \xE2\x80\x94 " + m.artist;
                    return s;
                };
                media_menu.push_back(std::move(now));
                auto act = [kdc_p](const char* a){ if (kdc_p) kdc_p->media_action(a); };
                media_menu.push_back(leaf("Play / Pause", [act]{ act("PlayPause"); }));
                media_menu.push_back(leaf("Next",         [act]{ act("Next"); }));
                media_menu.push_back(leaf("Previous",     [act]{ act("Previous"); }));
                media_menu.push_back(leaf("Stop",         [act]{ act("Stop"); }));
                media_menu.push_back(slider("Volume", 0.f, 100.f, 5.f, "%",
                    [kdc_p]{ auto m = kdc_p ? kdc_p->media_status() : integrations::KdeConnectBridge::MediaStatus{};
                             return (float)(m.volume < 0 ? 0 : m.volume); },
                    [kdc_p](float v){ if (kdc_p) kdc_p->media_set_volume((int)v); }));
                // Player picker (up to 6).
                std::vector<MenuItem> players;
                for (int i = 0; i < 6; ++i) {
                    MenuItem p; p.type = MenuItemType::LEAF; p.label = "player";
                    p.label_fn = [kdc_p, i]{ auto m = kdc_p ? kdc_p->media_status()
                            : integrations::KdeConnectBridge::MediaStatus{};
                        if (i >= (int)m.players.size()) return std::string();
                        return m.players[i] + (m.players[i] == m.player ? "  \xE2\x97\x8F" : ""); };
                    p.visible_fn = [kdc_p, i]{ auto m = kdc_p ? kdc_p->media_status()
                            : integrations::KdeConnectBridge::MediaStatus{};
                        return i < (int)m.players.size(); };
                    p.action = [kdc_p, i]{ if (!kdc_p) return; auto m = kdc_p->media_status();
                        if (i < (int)m.players.size()) kdc_p->media_set_player(m.players[i]); };
                    players.push_back(std::move(p));
                }
                media_menu.push_back(submenu("Player", std::move(players)));
            }

            // ── Run commands (remotecommands) ──────────────────────────────────
            std::vector<MenuItem> cmd_menu;
            for (int i = 0; i < 16; ++i) {
                MenuItem c; c.type = MenuItemType::LEAF; c.label = "command";
                c.label_fn = [kdc_p, i]{ auto v = kdc_p ? kdc_p->run_commands()
                        : std::vector<integrations::KdeConnectBridge::RunCommand>{};
                    return i < (int)v.size() ? v[i].name : std::string(); };
                c.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->run_commands().size(); };
                c.action = [kdc_p, i, toast]{ if (!kdc_p) return; auto v = kdc_p->run_commands();
                    if (i < (int)v.size() && kdc_p->run_command(v[i].key)) toast("Sent", v[i].name); };
                cmd_menu.push_back(std::move(c));
            }

            // ── Grouped ignore picker (app → senders; red = muted) ─────────────
            auto roster = [kdc_p]{ return kdc_p ? kdc_p->notif_roster()
                : std::vector<std::pair<std::string, std::vector<std::string>>>{}; };
            auto ig_has = [kdc_ignore_p](const std::string& s){
                return kdc_ignore_p && std::find(kdc_ignore_p->begin(), kdc_ignore_p->end(), s)
                                       != kdc_ignore_p->end(); };
            auto ig_toggle = [kdc_ignore_p, apply_ignore](const std::string& s){
                if (!kdc_ignore_p || s.empty()) return;
                auto it = std::find(kdc_ignore_p->begin(), kdc_ignore_p->end(), s);
                if (it != kdc_ignore_p->end()) kdc_ignore_p->erase(it);
                else                           kdc_ignore_p->push_back(s);
                apply_ignore();
            };
            auto build_app_senders = [roster, ig_has, ig_toggle](int ai){
                std::vector<MenuItem> kids;
                MenuItem all; all.type = MenuItemType::TOGGLE; all.label = "Mute whole app";
                all.get_toggle = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                all.set_toggle = [roster, ig_toggle, ai](bool){ auto r = roster();
                    if (ai < (int)r.size()) ig_toggle(r[ai].first); };
                all.warn_fn = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                kids.push_back(std::move(all));
                for (int si = 0; si < 24; ++si) {
                    MenuItem m; m.type = MenuItemType::TOGGLE; m.label = "sender";
                    m.label_fn   = [roster, ai, si]{ auto r = roster();
                        return (ai < (int)r.size() && si < (int)r[ai].second.size())
                                   ? r[ai].second[si] : std::string(); };
                    m.visible_fn = [roster, ai, si]{ auto r = roster();
                        return ai < (int)r.size() && si < (int)r[ai].second.size(); };
                    m.get_toggle = [roster, ig_has, ai, si]{ auto r = roster();
                        if (ai >= (int)r.size() || si >= (int)r[ai].second.size()) return false;
                        return ig_has(r[ai].second[si]); };
                    m.set_toggle = [roster, ig_toggle, ai, si](bool){ auto r = roster();
                        if (ai < (int)r.size() && si < (int)r[ai].second.size())
                            ig_toggle(r[ai].second[si]); };
                    m.warn_fn    = [roster, ig_has, ai, si]{ auto r = roster();
                        if (ai >= (int)r.size() || si >= (int)r[ai].second.size()) return false;
                        return ig_has(r[ai].second[si]); };
                    kids.push_back(std::move(m));
                }
                return kids;
            };
            std::vector<MenuItem> ignore_menu;
            ignore_menu.push_back(with_desc(leaf("Add Word\xE2\x80\xA6 (keyboard)",
                [menu_sys_pp, kdc_ignore_p, apply_ignore]{
                    if (!menu_sys_pp || !*menu_sys_pp || !kdc_ignore_p) return;
                    (*menu_sys_pp)->open_keyboard("Ignore (matches title/text)", "",
                        [kdc_ignore_p, apply_ignore](const std::string& s){
                            const size_t b = s.find_first_not_of(" \t");
                            const size_t e = s.find_last_not_of(" \t");
                            if (b == std::string::npos) return;
                            kdc_ignore_p->push_back(s.substr(b, e - b + 1)); apply_ignore();
                        });
                }), "Add a free-text mute (matches a notification's title/text)."));
            ignore_menu.push_back(with_desc(leaf("Unmute All",
                [kdc_ignore_p, apply_ignore]{ if (kdc_ignore_p) { kdc_ignore_p->clear(); apply_ignore(); } }),
                "Clear every mute."));
            for (int ai = 0; ai < 24; ++ai) {
                MenuItem appsub = submenu("app", build_app_senders(ai));
                appsub.label_fn   = [roster, ai]{ auto r = roster();
                    return ai < (int)r.size() ? r[ai].first : std::string(); };
                appsub.visible_fn = [roster, ai]{ return ai < (int)roster().size(); };
                appsub.warn_fn    = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                ignore_menu.push_back(std::move(appsub));
            }

            // ── SMS (send) ─────────────────────────────────────────────────────
            auto send_sms_flow = [menu_sys_pp, kdc_p, toast]{
                if (!menu_sys_pp || !*menu_sys_pp || !kdc_p) return;
                (*menu_sys_pp)->open_keyboard("SMS to (number)", "",
                    [menu_sys_pp, kdc_p, toast](const std::string& num){
                        if (num.empty() || !menu_sys_pp || !*menu_sys_pp) return;
                        (*menu_sys_pp)->open_keyboard("Message", "",
                            [kdc_p, num, toast](const std::string& msg){
                                if (msg.empty()) return;
                                if (kdc_p->send_sms(num, msg)) toast("SMS sent", num + ": " + msg);
                            });
                    });
            };

            // ── Connectivity readout ───────────────────────────────────────────
            MenuItem conn_item; conn_item.type = MenuItemType::LEAF; conn_item.label = "Signal";
            conn_item.label_fn = [kdc_p]{
                if (!kdc_p) return std::string("Signal: n/a");
                auto c = kdc_p->connectivity();
                if (!c.ok) return std::string("Signal: n/a");
                std::string s = "Signal: " + (c.network_type.empty() ? std::string("?") : c.network_type);
                if (c.strength >= 0) { s += "  "; for (int i = 0; i < 5; ++i) s += (i < c.strength) ? "\xE2\x96\xB0" : "\xE2\x96\xB1"; }
                return s;
            };

            // ── Pair Device ────────────────────────────────────────────────────
            // Lists every reachable KDE Connect device (paired or not). Selecting
            // an unpaired one requests pairing (accept the prompt on the phone);
            // selecting a paired one unpairs it.
            std::vector<MenuItem> pair_menu;
            for (int i = 0; i < 8; ++i) {
                MenuItem d; d.type = MenuItemType::LEAF; d.label = "device";
                d.label_fn = [kdc_p, i]{
                    if (!kdc_p) return std::string();
                    auto v = kdc_p->devices();
                    if (i >= (int)v.size()) return std::string();
                    const auto& dev = v[i];
                    std::string suffix = dev.paired ? "  \xE2\x9C\x93 paired"
                                       : dev.reachable ? "  \xE2\x80\x94 tap to pair"
                                                       : "  (offline)";
                    return dev.name + suffix;
                };
                d.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->devices().size(); };
                d.action = [kdc_p, i, toast]{
                    if (!kdc_p) return;
                    auto v = kdc_p->devices();
                    if (i >= (int)v.size()) return;
                    const auto& dev = v[i];
                    if (dev.paired) {
                        if (kdc_p->unpair(dev.id)) toast("Unpaired", dev.name);
                    } else if (kdc_p->request_pairing(dev.id)) {
                        toast("Pairing requested", "Accept the prompt on " + dev.name);
                    }
                };
                pair_menu.push_back(std::move(d));
            }

            // ── Export to Phone ─────────────────────────────────────────────────
            // Bundle a whole category (faces / gifs / qr codes) into one .tar.gz
            // and share it to the phone via KDE Connect. Source dirs are derived
            // from the active face path (faces + its sibling gifs) and state.qr_dir,
            // so no extra params have to be threaded through build_menu.
            auto export_to_phone = [kdc_p, teensy, &state, toast](const char* kind) {
                namespace fsx = std::filesystem;
                std::string src;
                if (std::string(kind) == "qr") {
                    std::lock_guard<std::mutex> lk(state.mtx); src = state.qr_dir;
                } else {
                    const std::string fp = teensy ? teensy->face_image_path("neutral")
                                                  : std::string();
                    if (!fp.empty()) {
                        fsx::path faces = fsx::path(fp).parent_path().parent_path();  // <assets>/faces
                        src = (std::string(kind) == "faces")
                                ? faces.string()
                                : (faces.parent_path() / "gifs").string();           // <assets>/gifs
                    }
                }
                const std::string k = kind;
                // Archive + share off-thread so a big folder doesn't stall the menu.
                std::thread([kdc_p, toast, src, k]{
                    namespace fsx = std::filesystem;
                    if (src.empty() || !fsx::exists(src)) {
                        toast("Export failed", "No " + k + " folder found"); return;
                    }
                    const std::string out    = "/tmp/protohud_" + k + ".tar.gz";
                    const std::string parent = fsx::path(src).parent_path().string();
                    const std::string base   = fsx::path(src).filename().string();
                    const std::string cmd = "tar -czf '" + out + "' -C '" + parent +
                                            "' '" + base + "' 2>/dev/null";
                    if (std::system(cmd.c_str()) != 0 || !fsx::exists(out)) {
                        toast("Export failed", "Could not archive " + k); return;
                    }
                    if (kdc_p && kdc_p->share_file(out)) toast("Sent to phone", k + ".tar.gz");
                    else                                 toast("Export ready", "Saved " + out);
                }).detach();
            };
            std::vector<MenuItem> export_menu = {
                with_desc(leaf("Export Faces",    [export_to_phone]{ export_to_phone("faces"); }),
                    "Bundle the faces folder into one .tar.gz and send it to the phone."),
                with_desc(leaf("Export GIFs",     [export_to_phone]{ export_to_phone("gifs"); }),
                    "Bundle the gifs folder into one .tar.gz and send it to the phone."),
                with_desc(leaf("Export QR Codes", [export_to_phone]{ export_to_phone("qr"); }),
                    "Bundle the captured QR-code folder into one .tar.gz and send it."),
                with_desc(leaf("Export All", [export_to_phone]{
                    export_to_phone("faces"); export_to_phone("gifs"); export_to_phone("qr"); }),
                    "Send all three bundles to the phone, one file each."),
                // Send the most recent photo/recording (newest file in the photo
                // dir, derived as the parent of map_dir).
                with_desc(leaf("Send Last Capture", [kdc_p, map_dir, toast]{
                    namespace fsx = std::filesystem;
                    const std::string dir = fsx::path(map_dir).parent_path().string();
                    std::error_code ec; fsx::path newest; fsx::file_time_type best{};
                    fsx::directory_iterator it(dir, ec), end;
                    for (; !ec && it != end; it.increment(ec)) {
                        if (!it->is_regular_file(ec)) continue;
                        const std::string ext = it->path().extension().string();
                        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".mp4")
                            continue;
                        const auto t = it->last_write_time(ec); if (ec) continue;
                        if (newest.empty() || t > best) { best = t; newest = it->path(); }
                    }
                    if (newest.empty()) { toast("No captures", "Nothing in the photo folder yet"); return; }
                    if (kdc_p && kdc_p->share_file(newest.string()))
                        toast("Sent to phone", newest.filename().string());
                    else toast("Export ready", newest.filename().string() + " (no phone)");
                }), "Share the newest photo/recording with the phone over KDE Connect."),
            };

            std::vector<MenuItem> phone_menu;
            phone_menu.push_back(with_desc(submenu("Pair Device", std::move(pair_menu)),
                "Pair or unpair a KDE Connect device. Reachable devices are listed; "
                "select an unpaired one and accept the prompt on the phone."));
            phone_menu.push_back(with_desc(submenu("Export to Phone", std::move(export_menu)),
                "Send your faces, GIFs or captured QR codes to the phone \xE2\x80\x94 each "
                "category bundled into a single .tar.gz over KDE Connect."));
            phone_menu.push_back(with_desc(leaf("Ring My Phone", ring_toast),
                "Ring the paired phone (KDE Connect findmyphone) so it plays its "
                "ringtone \xE2\x80\x94 handy for locating it."));
            phone_menu.push_back(with_desc(submenu("Notifications", std::move(notif_menu)),
                "Recent phone notifications. Reply to messaging apps or dismiss them "
                "(clears on the phone too)."));
            phone_menu.push_back(with_desc(submenu("Media", std::move(media_menu)),
                "Control the phone's media playback (play/pause, skip, volume, player)."));
            phone_menu.push_back(with_desc(submenu("Run Command", std::move(cmd_menu)),
                "Trigger one of the phone's saved KDE Connect commands."));
            phone_menu.push_back(with_desc(leaf("Send SMS\xE2\x80\xA6", send_sms_flow),
                "Send a text message through the phone (enter number, then message)."));
            phone_menu.push_back(with_desc(leaf("Mute Ringer", [kdc_p, toast]{
                if (kdc_p && kdc_p->mute_ringer()) toast("Ringer muted", "Silenced incoming call"); }),
                "Silence an incoming call's ringer (best-effort, depends on the phone)."));
            phone_menu.push_back(with_desc(conn_item,
                "Cellular network type + signal strength reported by the phone."));
            phone_menu.push_back(with_desc(submenu("Ignore List", std::move(ignore_menu)),
                "Mute notifications by app or sender. Apps/senders the phone has sent "
                "are grouped here \xE2\x80\x94 select one to mute it (muted entries turn red). "
                "\"Add Word\" still takes free text."));
            phone_menu.push_back(with_desc(submenu("Message Apps",
                list_menu(kdc_msg_p, apply_msg, "Add App\xE2\x80\xA6", "App name (e.g. Discord)")),
                "Apps whose notifications get the larger chat toast (sender + wrapped "
                "message, held longer). Select an entry to remove it."));
            return with_desc(submenu("Phone (KDE Connect)", std::move(phone_menu)),
                "Ring/mute, reply to & dismiss notifications, control media, run "
                "commands, send SMS, and mute apps/senders.");
    }();
    MenuItem notiflog_item = [&]() -> MenuItem {
        // ── Notification Log: grouped by sender, filterable, full text in panel ──
        // Type filter (radio).
        auto type_opt = [&state](const char* lbl, int t){
            return leaf_sel(lbl, [&state, t]{ state.notif_type_filter = t; },
                                 [&state, t]{ return state.notif_type_filter == t; });
        };
        std::vector<MenuItem> typ_menu = {
            type_opt("All Types",    -1),
            type_opt("Alarms",       static_cast<int>(NotifType::Alarm)),
            type_opt("Timers",       static_cast<int>(NotifType::Timer)),
            type_opt("LoRa",         static_cast<int>(NotifType::LoRa)),
            type_opt("Phone / Apps", static_cast<int>(NotifType::App)),
        };
        MenuItem typ_item = submenu("Filter: Type", std::move(typ_menu));
        typ_item.label_fn = [&state]{
            const char* t = state.notif_type_filter < 0 ? "All"
                : state.notif_type_filter == (int)NotifType::Alarm ? "Alarms"
                : state.notif_type_filter == (int)NotifType::Timer ? "Timers"
                : state.notif_type_filter == (int)NotifType::LoRa  ? "LoRa" : "Phone/Apps";
            return std::string("Filter: Type  [") + t + "]";
        };

        // Distinct sender names currently in the log (sorted).
        auto distinct_senders = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            std::set<std::string> s;
            for (const auto& n : state.notifs.items) if (!n.title.empty()) s.insert(n.title);
            return std::vector<std::string>(s.begin(), s.end());
        };
        // Sender checklist (multi-select).
        std::vector<MenuItem> snd_menu;
        snd_menu.push_back(with_desc(leaf("Show All Senders", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx); state.notif_sender_sel.clear();
        }), "Clear the selection so every sender shows."));
        for (int i = 0; i < 24; ++i) {
            MenuItem m; m.type = MenuItemType::TOGGLE; m.label = "sender";
            m.label_fn   = [distinct_senders, i]{
                auto d = distinct_senders(); return i < (int)d.size() ? d[i] : std::string(); };
            m.visible_fn = [distinct_senders, i]{ return i < (int)distinct_senders().size(); };
            m.get_toggle = [distinct_senders, i, &state]{
                auto d = distinct_senders(); if (i >= (int)d.size()) return false;
                std::lock_guard<std::mutex> lk(state.mtx); return state.notif_sender_sel.count(d[i]) > 0; };
            m.set_toggle = [distinct_senders, i, &state](bool v){
                auto d = distinct_senders(); if (i >= (int)d.size()) return;
                std::lock_guard<std::mutex> lk(state.mtx);
                if (v) state.notif_sender_sel.insert(d[i]); else state.notif_sender_sel.erase(d[i]); };
            snd_menu.push_back(std::move(m));
        }
        MenuItem snd_item = submenu("Filter: Senders", std::move(snd_menu));
        snd_item.label_fn = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            return state.notif_sender_sel.empty()
                ? std::string("Filter: Senders  [all]")
                : std::string("Filter: Senders  [") + std::to_string(state.notif_sender_sel.size()) + "]";
        };

        // Display order: queue indices passing the filters, grouped by sender
        // (sender A-Z, newest first within each). Rebuilt only when something changes.
        auto order     = std::make_shared<std::vector<int>>();
        auto order_key = std::make_shared<size_t>(SIZE_MAX);
        auto ensure_order = [&state, order, order_key]{
            std::lock_guard<std::mutex> lk(state.mtx);
            size_t key = static_cast<size_t>(state.notifs.next_id) * 1315423911u
                       + static_cast<size_t>(state.notif_type_filter + 2) * 2654435761u
                       + state.notifs.items.size();
            for (const auto& s : state.notif_sender_sel)
                key ^= std::hash<std::string>{}(s) + 0x9e3779b9u + (key << 6) + (key >> 2);
            if (key == *order_key) return;
            *order_key = key;
            order->clear();
            for (int i = 0; i < (int)state.notifs.items.size(); ++i) {
                const auto& n = state.notifs.items[i];
                if (state.notif_type_filter >= 0 &&
                    static_cast<int>(n.type) != state.notif_type_filter) continue;
                if (!state.notif_sender_sel.empty() && !state.notif_sender_sel.count(n.title)) continue;
                order->push_back(i);
            }
            std::stable_sort(order->begin(), order->end(), [&state](int a, int b){
                const auto& na = state.notifs.items[a]; const auto& nb = state.notifs.items[b];
                if (na.title != nb.title) return na.title < nb.title;
                return a < b;   // newest-first within a sender (queue is newest-first)
            });
        };

        std::vector<MenuItem> nlog_menu;
        // Fixed items (must stay 4 — the context panel maps cursor → row via -4).
        nlog_menu.push_back(with_desc(toggle("Persist Log",
            [&state]{ return state.notif_persist; },
            [&state](bool v){ state.notif_persist = v; }),
            "Save the log to disk so a sudden reboot doesn't lose it."));
        // Bulk clear — a small confirmation layer with the safe option first.
        // "Clear Unsaved" keeps anything the user pinned; "Clear All" wipes
        // everything including saved messages (red).
        std::vector<MenuItem> clr_menu;
        clr_menu.push_back(with_desc(leaf("Cancel", [menu_sys_pp]{
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); }),
            "Back out without clearing anything."));
        clr_menu.push_back(with_desc(leaf("Clear Unsaved", [&state, menu_sys_pp]{
            { std::lock_guard<std::mutex> lk(state.mtx);
              state.notifs.clear_if([](const Notification&){ return true; }, /*include_saved=*/false); }
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
        }), "Remove everything except messages you've saved."));
        {
            MenuItem all = leaf("Clear All", [&state, menu_sys_pp]{
                { std::lock_guard<std::mutex> lk(state.mtx); state.notifs.items.clear(); }
                if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
            });
            all.warn_fn = []{ return true; };   // red — also drops saved messages
            clr_menu.push_back(with_desc(std::move(all),
                "Remove every notification, including saved ones."));
        }
        MenuItem clr_item = submenu("Clear Log\xE2\x80\xA6", std::move(clr_menu));
        clr_item.context_panel_title = "Clear";
        clr_item.context_panel_draw  = [&state](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            (void)sz; ImFont* f = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            int total, saved; { std::lock_guard<std::mutex> lk(state.mtx);
                total = (int)state.notifs.items.size(); saved = 0;
                for (const auto& n : state.notifs.items) saved += n.saved ? 1 : 0; }
            char ln[64]; float y = o.y;
            snprintf(ln, sizeof(ln), "%d total", total);
            dl->AddText(f, fs, {o.x,y}, IM_COL32(235,240,245,255), ln); y += fs*1.4f;
            snprintf(ln, sizeof(ln), "%d saved \xC2\xB7 %d unsaved", saved, total - saved);
            dl->AddText(f, fs*0.85f, {o.x,y}, IM_COL32(190,196,204,230), ln);
        };
        nlog_menu.push_back(with_desc(std::move(clr_item),
            "Bulk-clear the log: keep saved messages or wipe everything."));
        nlog_menu.push_back(with_desc(std::move(typ_item), "Show only the chosen type."));
        nlog_menu.push_back(with_desc(std::move(snd_item),
            "Tick which senders to show. None ticked = all senders."));
        // Message rows (grouped by sender, scrollable). Full text shows in the panel.
        // Draw a single notification (full text) into a panel — used by both the
        // log preview and the per-row delete confirmation.
        auto draw_one = [&state, order, ensure_order](ImDrawList* dl, ImVec2 o, ImVec2 sz,
                                                      int slot, bool confirm){
            ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (slot < 0 || slot >= (int)order->size() ||
                (*order)[slot] >= (int)state.notifs.items.size()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a message.");
                return;
            }
            const auto& n = state.notifs.items[(*order)[slot]];
            float y = o.y;
            if (confirm) {
                dl->AddText(font, fs * 0.95f, {o.x, y}, IM_COL32(255,110,100,255),
                            "Delete this message?");
                y += fs * 1.4f;
            }
            dl->AddText(font, fs * 1.05f, {o.x, y}, IM_COL32(235,240,245,255), n.title.c_str());
            y += fs * 1.3f;
            if (n.timestamp > 0) {
                char ts[24]; time_t t = (time_t)n.timestamp;
                strftime(ts, sizeof(ts), "%a %H:%M", localtime(&t));
                dl->AddText(font, fs * 0.7f, {o.x, y}, IM_COL32(150,158,166,210), ts);
                y += fs * 1.2f;
            }
            if (!n.body.empty())
                dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(210,214,220,235),
                            n.body.c_str(), nullptr, sz.x - 6.f);
        };
        // Selecting a row opens a per-message popup with the full text in the
        // panel and three actions: Save (pin), Confirm Delete (red), Cancel.
        // Helper: queue index of the message currently at display slot `i`, or -1.
        auto qindex = [&state, order, ensure_order](int i) -> int {
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i >= (int)order->size()) return -1;
            const int qi = (*order)[i];
            return qi < (int)state.notifs.items.size() ? qi : -1;
        };
        for (int i = 0; i < NotificationQueue::kMax; ++i) {
            MenuItem m; m.type = MenuItemType::SUBMENU; m.label = "msg";
            m.label_fn = [&state, qindex, i]{
                const int qi = qindex(i);
                std::lock_guard<std::mutex> lk(state.mtx);
                if (qi < 0) return std::string();
                const auto& n = state.notifs.items[qi];
                std::string s = n.saved ? std::string("\xE2\x98\x85 ") : std::string();  // ★ if saved
                s += n.title;
                if (!n.body.empty()) s += "  \xC2\xB7  " + n.body;
                if (s.size() > 60) s = s.substr(0, 59) + "\xE2\x80\xA6";
                return s;
            };
            m.visible_fn = [order, ensure_order, i]{ ensure_order(); return i < (int)order->size(); };
            // Save / pin toggle. qindex() locks internally, so resolve it first,
            // then re-validate the index after re-acquiring the lock.
            MenuItem save; save.type = MenuItemType::TOGGLE; save.label = "Save Message";
            save.get_toggle = [&state, qindex, i]{
                const int qi = qindex(i); std::lock_guard<std::mutex> lk(state.mtx);
                return qi >= 0 && qi < (int)state.notifs.items.size() && state.notifs.items[qi].saved; };
            save.set_toggle = [&state, qindex, i](bool v){
                const int qi = qindex(i);
                { std::lock_guard<std::mutex> lk(state.mtx);
                  if (qi >= 0 && qi < (int)state.notifs.items.size()) state.notifs.items[qi].saved = v; }
                state.notif_dirty.store(true);   // metadata-only edit → force a flush
            };
            MenuItem confirm = leaf("Confirm Delete",
                [&state, qindex, i, menu_sys_pp]{
                    const int qi = qindex(i);
                    { std::lock_guard<std::mutex> lk(state.mtx);
                      if (qi >= 0 && qi < (int)state.notifs.items.size())
                          state.notifs.items.erase(state.notifs.items.begin() + qi); }
                    if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();   // back to the log
                });
            confirm.warn_fn = []{ return true; };   // red — destructive
            MenuItem cancel = leaf("Cancel",
                [menu_sys_pp]{ if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); });
            m.children = { with_desc(std::move(save), "Pin this message so Clear and the rolling buffer keep it."),
                           std::move(confirm), std::move(cancel) };
            m.context_panel_title = "Message";
            m.context_panel_draw  = [draw_one, i](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                draw_one(dl, o, sz, i, false); };
            nlog_menu.push_back(std::move(m));
        }
        // Context panel: full text of the focused row.
        auto panel = [&state, order, ensure_order, menu_sys_pp]
                     (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const int idx  = (menu_sys_pp && *menu_sys_pp) ? (*menu_sys_pp)->current_index() : -1;
            const int slot = idx - 4;   // 4 fixed items precede the rows
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (slot < 0 || slot >= (int)order->size() ||
                (*order)[slot] >= (int)state.notifs.items.size()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a message to read it here.");
                return;
            }
            const auto& n = state.notifs.items[(*order)[slot]];
            float y = o.y;
            dl->AddText(font, fs * 1.05f, {o.x, y}, IM_COL32(235,240,245,255), n.title.c_str());
            y += fs * 1.3f;
            if (n.timestamp > 0) {
                char ts[24]; time_t t = (time_t)n.timestamp;
                strftime(ts, sizeof(ts), "%a %H:%M", localtime(&t));
                dl->AddText(font, fs * 0.7f, {o.x, y}, IM_COL32(150,158,166,210), ts);
                y += fs * 1.2f;
            }
            if (!n.body.empty())
                dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(210,214,220,235),
                            n.body.c_str(), nullptr, sz.x - 6.f);   // wrapped
        };
        return with_panel(with_desc(submenu("Notification Log", std::move(nlog_menu)),
            "Browse the log grouped by sender; scroll to a message to read its full "
            "text on the right. Filter by type/sender, save messages to pin them, or "
            "bulk-clear the rest."),
            "Message", panel);
    }();
    std::vector<MenuItem> communications_menu;
    communications_menu.push_back(with_desc(submenu("LoRa", std::move(lora_menu)),
        "Long-range radio: team nodes, messages and status."));
    // The Phone (KDE Connect) item lives under System > Connectivity; hand it
    // to the System builder through the context.
    ctx.phone_item = std::move(phone_item);
    communications_menu.push_back(std::move(notiflog_item));

    // ── Scanned QR Codes ──────────────────────────────────────────────────
    // Each captured code lives in its own folder (link + raw image). This
    // submenu browses the de-duplicated running list; the panel shows the
    // captured image + link, and each row can Open (URLs) or Delete.
    MenuItem qr_codes_item = [&]() -> MenuItem {
        struct QrThumb { GLuint tex = 0; std::string loaded; cv::Mat img; };
        auto thumb = std::make_shared<QrThumb>();
        auto focus = std::make_shared<int>(-1);

        auto cap_at = [&state](int i) -> QrCapture {
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i < 0 || i >= static_cast<int>(state.qr_captures.items.size())) return {};
            return state.qr_captures.items[i];
        };
        auto is_url = [](const std::string& t){
            return t.size() > 7 && (t.compare(0, 7, "http://") == 0 ||
                                    t.compare(0, 8, "https://") == 0);
        };
        // Shared panel painter: thumbnail (top) + link (wrapped) + type/time.
        auto draw_cap = [thumb](ImDrawList* dl, ImVec2 o, ImVec2 sz, const QrCapture& c){
            ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            if (c.text.empty()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a code.");
                return;
            }
            float y = o.y;
            // Prefer the colour camera frame; fall back to the grayscale decode.
            std::string ipath;
            const std::string& fn = !c.image.empty() ? c.image : c.decode;
            if (!c.folder.empty() && !fn.empty())
                ipath = (std::filesystem::path(c.folder) / fn).string();
            if (!ipath.empty()) {
                if (ipath != thumb->loaded) {
                    thumb->img = face::load_png_rgba(ipath, 240, 180);
                    thumb->loaded = ipath;
                }
                const float pw = std::min(sz.x * 0.85f, 200.f), ph = pw * 0.75f;
                const float px = o.x, py = y;
                dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10,16,22,190));
                if (!thumb->img.empty() && thumb->img.isContinuous()) {
                    if (!thumb->tex) {
                        glGenTextures(1, &thumb->tex);
                        glBindTexture(GL_TEXTURE_2D, thumb->tex);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    }
                    glBindTexture(GL_TEXTURE_2D, thumb->tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumb->img.cols, thumb->img.rows,
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, thumb->img.data);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    dl->AddImage(reinterpret_cast<ImTextureID>(
                        static_cast<uintptr_t>(thumb->tex)), {px, py}, {px + pw, py + ph});
                }
                y = py + ph + 8.f;
            }
            dl->AddText(font, fs * 0.9f, {o.x, y}, IM_COL32(235,240,245,255),
                        c.text.c_str(), nullptr, sz.x - 6.f);
            ImVec2 tsz = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, sz.x - 6.f, c.text.c_str());
            y += tsz.y + 6.f;
            char meta[80]; std::string when;
            if (c.timestamp > 0) {
                char ts[24]; time_t t = static_cast<time_t>(c.timestamp);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&t));
                when = ts;
            }
            std::snprintf(meta, sizeof(meta), "%s%s%s", c.type.c_str(),
                          when.empty() ? "" : "  \xC2\xB7  ", when.c_str());
            dl->AddText(font, fs * 0.72f, {o.x, y}, IM_COL32(150,158,166,210), meta);
        };

        // List removable mounts (USB sticks etc.) under the auto-mount roots as
        // {path, display}. Cached ~1 s so the menu's per-slot label/visible fns
        // don't readdir every frame. Cheap listing only (no write probe).
        auto usb_cache   = std::make_shared<std::vector<std::pair<std::string,std::string>>>();
        auto usb_cache_t = std::make_shared<double>(-100.0);
        auto list_usb_mounts =
            [usb_cache, usb_cache_t]() -> const std::vector<std::pair<std::string,std::string>>& {
            const double now = ImGui::GetTime();
            if (now - *usb_cache_t > 1.0) {
                *usb_cache_t = now;
                usb_cache->clear();
                namespace fs = std::filesystem;
                std::vector<std::string> roots;
                std::error_code ec;
                if (const char* u = std::getenv("USER")) {
                    const std::string mu = std::string("/media/") + u;
                    if (fs::is_directory(mu, ec)) roots.push_back(mu);
                }
                if (roots.empty()) roots.push_back("/media");
                roots.push_back("/mnt");
                for (const auto& root : roots) {
                    if (!fs::is_directory(root, ec)) continue;
                    for (const auto& e : fs::directory_iterator(root, ec))
                        if (e.is_directory(ec))
                            usb_cache->emplace_back(e.path().string(),
                                                    e.path().filename().string());
                }
            }
            return *usb_cache;
        };
        // Derive a filename-safe, human-ish name from a code's text so the image
        // matches its link in the manifest. Host/path for URLs, else first chars,
        // plus a short hash of the full text to keep it unique.
        auto qr_parsed_name = [](const std::string& text) -> std::string {
            std::string base = text;
            const auto scheme = base.find("://");
            if (scheme != std::string::npos) base = base.substr(scheme + 3);
            std::string out;
            for (char ch : base) {
                if (std::isalnum(static_cast<unsigned char>(ch))) out += ch;
                else if (ch == '.' || ch == '-' || ch == '_') out += ch;
                else out += '_';
                if (out.size() >= 40) break;
            }
            if (out.empty()) out = "qr";
            char hx[8];
            std::snprintf(hx, sizeof(hx), "%04x",
                          static_cast<unsigned>(std::hash<std::string>{}(text) & 0xffff));
            return out + "_" + hx;
        };
        // Stage a temp bundle: each code's image copied to <parsed_name>.png plus
        // a qr_links.txt manifest mapping names → links. Returns the file list.
        auto build_qr_bundle = [qr_parsed_name](const std::vector<QrCapture>& caps)
            -> std::vector<std::string> {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path(ec) / "protohud_qr_share";
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            std::vector<std::string> files;
            std::ofstream man((dir / "qr_links.txt").string());
            if (man) man << "ProtoHUD scanned codes\n======================\n\n";
            for (const auto& c : caps) {
                const std::string nm = qr_parsed_name(c.text);
                std::string src;
                if (!c.folder.empty()) {
                    for (const char* pref : {"camera.png", "decode.png"}) {
                        const fs::path p = fs::path(c.folder) / pref;
                        if (fs::exists(p, ec)) { src = p.string(); break; }
                    }
                    if (src.empty())
                        for (const auto& e : fs::directory_iterator(c.folder, ec))
                            if (e.is_regular_file() && e.path().extension() == ".png") {
                                src = e.path().string(); break; }
                }
                if (!src.empty()) {
                    const fs::path dst = dir / (nm + ".png");
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                    if (!ec) files.push_back(dst.string());
                }
                if (man) {
                    char when[24] = "";
                    if (c.timestamp > 0) { time_t t = static_cast<time_t>(c.timestamp);
                        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t)); }
                    man << nm << ".png\n    " << c.text << "\n    [" << c.type
                        << (when[0] ? std::string("  ") + when : std::string()) << "]\n\n";
                }
            }
            if (man) { man.close(); files.push_back((dir / "qr_links.txt").string()); }
            return files;
        };
        // Send a set of captures (images + manifest) to the paired phone.
        auto send_to_phone = [kdc_p, build_qr_bundle, &state](const std::vector<QrCapture>& caps) {
            const bool dev = kdc_p && kdc_p->device_ready();
            int n = 0;
            if (dev) for (const auto& f : build_qr_bundle(caps))
                if (kdc_p->share_file(f)) ++n;
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification nt; nt.type = NotifType::App;
            nt.title = dev ? "Sent to phone" : "No phone connected";
            nt.body  = dev ? (std::to_string(n) + " file(s) shared (images + qr_links.txt)")
                           : "Pair + connect a device in KDE Connect first.";
            nt.auto_dismiss_s = 5.f; state.notifs.push(std::move(nt));
        };
        // Copy the bundle (renamed images + manifest) to a chosen mount.
        auto export_to_usb = [build_qr_bundle, &state](const std::vector<QrCapture>& caps,
                                                       const std::string& dest) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path target = fs::path(dest) / "protohud_qr";
            fs::create_directories(target, ec);
            int n = 0;
            for (const auto& f : build_qr_bundle(caps)) {
                fs::copy_file(f, target / fs::path(f).filename(),
                              fs::copy_options::overwrite_existing, ec);
                if (!ec) ++n;
            }
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification nt; nt.type = NotifType::App;
            nt.title = n > 0 ? "Exported to USB" : "Export failed";
            nt.body  = n > 0 ? ("Copied " + std::to_string(n) + " file(s) to " + target.string())
                             : ("Could not write to " + dest);
            nt.auto_dismiss_s = 6.f; state.notifs.push(std::move(nt));
        };
        // Build the "Export / Send" target list (phone + each connected drive).
        // get_caps() returns the captures to export (one row, or all).
        auto build_export_targets =
            [kdc_p, list_usb_mounts, send_to_phone, export_to_usb](
                std::function<std::vector<QrCapture>()> get_caps) {
            std::vector<MenuItem> t;
            MenuItem p; p.type = MenuItemType::LEAF; p.label = "phone";
            p.label_fn   = [kdc_p]{ return std::string("Phone: ") +
                                    (kdc_p ? kdc_p->active_device_name() : std::string()); };
            p.visible_fn = [kdc_p]{ return kdc_p && kdc_p->device_ready(); };
            p.action     = [send_to_phone, get_caps]{ send_to_phone(get_caps()); };
            t.push_back(std::move(p));
            for (int j = 0; j < 6; ++j) {
                MenuItem m; m.type = MenuItemType::LEAF; m.label = "drive";
                m.label_fn   = [list_usb_mounts, j]{
                    const auto& v = list_usb_mounts();
                    return j < (int)v.size() ? ("USB: " + v[j].second) : std::string(); };
                m.visible_fn = [list_usb_mounts, j]{ return j < (int)list_usb_mounts().size(); };
                m.action     = [export_to_usb, get_caps, list_usb_mounts, j]{
                    const auto& v = list_usb_mounts();
                    if (j < (int)v.size()) export_to_usb(get_caps(), v[j].first); };
                t.push_back(std::move(m));
            }
            MenuItem none; none.type = MenuItemType::LEAF; none.label = "(no devices connected)";
            none.visible_fn = [kdc_p, list_usb_mounts]{
                return !(kdc_p && kdc_p->device_ready()) && list_usb_mounts().empty(); };
            t.push_back(std::move(none));
            return t;
        };

        std::vector<MenuItem> qmenu;
        // Clear All (red) — drops every entry + its folder.
        {
            MenuItem clr = leaf("Clear All", [&state]{
                std::vector<std::string> folders;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    for (auto& c : state.qr_captures.items)
                        if (!c.folder.empty()) folders.push_back(c.folder);
                    state.qr_captures.items.clear();
                    state.qr_captures.seen.clear();
                    qr_write_index(state.qr_dir, state.qr_captures);
                }
                std::error_code ec;
                for (auto& f : folders) std::filesystem::remove_all(f, ec);
            });
            clr.warn_fn    = []{ return true; };
            clr.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                       return !state.qr_captures.items.empty(); };
            qmenu.push_back(with_desc(std::move(clr),
                "Delete every saved QR code and its folder."));
        }
        // Export / Send All — to a chosen connected device (phone or a drive).
        // One bundle with every code's image + a combined qr_links.txt manifest.
        {
            MenuItem all = submenu("Export / Send All", build_export_targets(
                [&state]() -> std::vector<QrCapture> {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return std::vector<QrCapture>(state.qr_captures.items.begin(),
                                                  state.qr_captures.items.end());
                }));
            all.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                       return !state.qr_captures.items.empty(); };
            qmenu.push_back(with_desc(std::move(all),
                "Send all saved codes to a connected device as named images plus a "
                "single qr_links.txt listing every link."));
        }
        // Capture rows (newest first).
        for (int i = 0; i < 40; ++i) {
            MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "qr";
            row.label_fn = [&state, i]{
                std::lock_guard<std::mutex> lk(state.mtx);
                if (i >= static_cast<int>(state.qr_captures.items.size())) return std::string();
                std::string s = state.qr_captures.items[i].text;
                if (s.size() > 48) s = s.substr(0, 47) + "\xE2\x80\xA6";
                return s;
            };
            row.visible_fn   = [&state, i]{ std::lock_guard<std::mutex> lk(state.mtx);
                                 return i < static_cast<int>(state.qr_captures.items.size()); };
            row.on_highlight = [focus, i]{ *focus = i; };
            MenuItem open = leaf("Open Link", [cap_at, is_url, i]{
                QrCapture c = cap_at(i);
                if (!is_url(c.text)) return;
                std::string safe = c.text; for (auto& ch : safe) if (ch == '\'') ch = ' ';
                std::string cmd = "xdg-open '" + safe + "' >/dev/null 2>&1 &";
                system(cmd.c_str());
            });
            open.visible_fn = [cap_at, is_url, i]{ return is_url(cap_at(i).text); };
            MenuItem del = leaf("Delete", [&state, i, menu_sys_pp]{
                std::string folder;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (i < static_cast<int>(state.qr_captures.items.size())) {
                        folder = state.qr_captures.items[i].folder;
                        state.qr_captures.seen.erase(state.qr_captures.items[i].text);
                        state.qr_captures.items.erase(state.qr_captures.items.begin() + i);
                        qr_write_index(state.qr_dir, state.qr_captures);
                    }
                }
                if (!folder.empty()) { std::error_code ec;
                    std::filesystem::remove_all(folder, ec); }
                if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
            });
            del.warn_fn = []{ return true; };
            MenuItem exp = submenu("Export / Send", build_export_targets(
                [cap_at, i]() -> std::vector<QrCapture> { return { cap_at(i) }; }));
            exp.visible_fn = [cap_at, i]{ return !cap_at(i).folder.empty(); };
            exp.description = "Send this code (image + a qr_links.txt) to a connected "
                              "device — the paired phone (KDE Connect) or a USB drive.";
            row.children = { std::move(open), std::move(exp), std::move(del) };
            row.context_panel_title = "QR Code";
            row.context_panel_draw  = [draw_cap, cap_at, i](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                draw_cap(dl, o, sz, cap_at(i)); };
            qmenu.push_back(std::move(row));
        }
        MenuItem sub = submenu("QR Codes", std::move(qmenu));
        sub.label_fn = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            const size_t n = state.qr_captures.items.size();
            return n ? ("QR Codes  [" + std::to_string(n) + "]") : std::string("QR Codes");
        };
        sub.context_panel_title = "QR Code";
        sub.context_panel_draw  = [draw_cap, focus, &state](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            QrCapture c;
            { std::lock_guard<std::mutex> lk(state.mtx);
              if (*focus >= 0 && *focus < static_cast<int>(state.qr_captures.items.size()))
                  c = state.qr_captures.items[*focus]; }
            draw_cap(dl, o, sz, c);
        };
        return with_desc(std::move(sub),
            "Scanned QR/barcodes — each saved in its own folder with the link "
            "and the captured image. Already-seen codes aren't captured twice.");
    }();
    communications_menu.push_back(std::move(qr_codes_item));

    return communications_menu;
}

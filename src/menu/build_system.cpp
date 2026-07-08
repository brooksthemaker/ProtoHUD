// ── build_system.cpp ──────────────────────────────────────────────────────────
// The System tab, moved verbatim out of build_menu(): display & HUD/menu
// presets, audio, connectivity (incl. the Phone item built by the
// Communications builder), Pi settings (GPIO visualizer/buttons, fans),
// XR headset, timers & alarm, diagnostics, power, and Software (updates,
// demo mode, issue reports, profiles & backup).

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
#include "sys/pico_pinmap.h"
#include "input/coproc_inputs.h"
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
#include "menu/shared_items.h"

// ── I2C bus scanner ───────────────────────────────────────────────────────────
// Runs in a background thread. Opens the bus, probes addresses 0x08–0x77, stores
// found addresses in state.i2c_scan_results, then clears i2c_scan_busy.
static void run_i2c_scan(AppState* sp) {
    std::string bus;
    { std::lock_guard<std::mutex> lk(sp->mtx); bus = sp->i2c_scan_bus; }

    std::vector<uint8_t> found;
    int fd = open(bus.c_str(), O_RDWR);
    if (fd >= 0) {
        // i2cdetect-style ACK probe. The old read()-based test counted any
        // errno but ENODEV/ENXIO as present, but a NACK on the Pi surfaces
        // as EREMOTEIO — every address scanned as occupied.
        for (int addr = 0x08; addr <= 0x77; ++addr)
            if (menu_shared::i2c_probe_addr(fd, addr))
                found.push_back(static_cast<uint8_t>(addr));
        close(fd);
    }

    std::lock_guard<std::mutex> lk(sp->mtx);
    sp->i2c_scan_results = std::move(found);
    sp->i2c_scan_busy    = false;
}

// ── RP2350 GPIO Expander menu (button coprocessor) ────────────────────────────
// Everything under GPIO > RP2350 GPIO Expander: link enable + status, the I2C
// bus test, the Board picker, and the Pins visualizer/editor. Kept as one
// self-contained builder so it can be changed without touching the on-board
// GPIO code — build_system_menu only routes the result (ctx.gpio_expander_out).
//
// Live-variant design: which GPs exist / are ADC / are board-reserved is read
// from the config's `variant` AT DISPLAY TIME (label_fn / visible_fn / the
// context-panel draw), so switching Board re-shapes the picker and the diagram
// immediately — no menu rebuild, no restart. Edits mutate the live CoprocConfig
// in place; apply_coproc persists them to cfg["inputs"]["coprocessor"] and
// re-pushes the PINCFG map to the firmware (Apply & Reload / Board / Remove).
static std::vector<MenuItem> build_coproc_expander_menu(MenuBuildContext& ctx)
{
    // Matches the firmware's kMaxButtons (PINCFG table size).
    constexpr int kMaxCoprocButtons = 16;

    std::vector<MenuItem> out;
    bool* enabled_p = ctx.coproc_enabled_p;
    auto  reload    = ctx.coproc_reload;   // shared_ptr<function<void()>>
    auto  status    = ctx.coproc_status;   // shared_ptr<function<string()>>

    out.push_back(with_desc(toggle("Enabled",
        [enabled_p]{ return enabled_p && *enabled_p; },
        [enabled_p, reload](bool v){
            if (enabled_p) *enabled_p = v;
            if (reload && *reload) (*reload)();
        }),
        "Use an external button coprocessor (USB/I\xC2\xB2""C). Applies "
        "immediately. On-board GPIO stays active too unless "
        "replace_local_gpio is set in config.json."));

    if (status) {
        MenuItem st = leaf("Status", []{});
        st.label_fn = [status]{
            return std::string("Status: ") +
                   (*status ? (*status)() : std::string("n/a"));
        };
        out.push_back(with_desc(std::move(st),
            "Connection state reported by the coprocessor link "
            "(connected / offline)."));
    }

    // I²C bus test — scan the coprocessor's I²C lines for devices.
    if (ctx.coproc_i2c_scan) {
        std::vector<MenuItem> i2c_menu;
        i2c_menu.push_back(with_desc(leaf("Scan Now",
            [scan = ctx.coproc_i2c_scan]{ if (scan) scan(); }),
            "Ask the coprocessor to probe its I2C lines (default GP20/21 = "
            "I2C0, the voice DAC bus) and report which 7-bit addresses ACK."));
        MenuItem res = leaf("Result", []{});
        res.label_fn = [get = ctx.coproc_i2c_result]{
            return std::string("Found: ") + (get ? get() : std::string("n/a"));
        };
        i2c_menu.push_back(with_desc(std::move(res),
            "Hex addresses that ACKed on the last scan (18 = the TLV320 DAC), "
            "or 'none' / 'err bad-pins'."));
        out.push_back(with_desc(submenu("I2C Bus Test", std::move(i2c_menu)),
            "Test I2C connectivity on the coprocessor \xe2\x80\x94 scan for "
            "devices (e.g. the TLV320 DAC at 0x18) and see which hex "
            "addresses respond. Scan Now triggers it; Result updates when "
            "the coprocessor replies."));
    }

    // Peripheral Test — exercise the pre-assigned test pins (servos GP6-9,
    // WS2812 zone GP22, ADC GP26-28, TTP223 touch GP0/1/12/16/17/18; see
    // firmware/button_coproc/pico/include/config.h).
    if (ctx.coproc_servo) {
        std::vector<MenuItem> pt;
        for (int ch = 0; ch < 4; ++ch) {
            auto pos = std::make_shared<float>(90.f);
            char nm[16]; snprintf(nm, sizeof(nm), "Servo %d", ch + 1);
            pt.push_back(with_desc(slider(nm, 0.f, 180.f, 5.f, "\xc2\xb0",
                [pos]{ return *pos; },
                [pos, ch, sv = ctx.coproc_servo](float v){
                    *pos = v; if (sv) sv(ch, (int)v);
                }),
                "Drive the test servo live (GP6-9 = channels 1-4). The GPIO "
                "doubles as a button slot: it becomes a servo on the first "
                "command and stays one until the coprocessor reboots. Servo "
                "V+ must come from an external 5-6 V supply, grounds common."));
        }
        pt.push_back(with_desc(leaf("All Servos Off",
            [sv = ctx.coproc_servo]{ if (sv) for (int c = 0; c < 4; ++c) sv(c, -1); }),
            "Detach all four test channels (stops holding torque)."));
        if (ctx.coproc_led_zone) {
            struct Sw { const char* name; int r, g, b; };
            static constexpr Sw kSw[] = {
                { "LED Zone: Off",   0,   0,   0 }, { "LED Zone: White", 255, 255, 255 },
                { "LED Zone: Red", 255,   0,   0 }, { "LED Zone: Green",   0, 255,   0 },
                { "LED Zone: Blue",  0,   0, 255 },
            };
            for (const auto& sw : kSw)
                pt.push_back(with_desc(leaf(sw.name,
                    [lz = ctx.coproc_led_zone, sw]{ if (lz) lz(sw.r, sw.g, sw.b, -1); }),
                    "Fill the WS2812 test strip on GP22 (level-shift for long "
                    "strips; strip length set in firmware config.h)."));
        }
        if (ctx.coproc_adc_read) {
            pt.push_back(with_desc(leaf("Read ADC Now",
                [rd = ctx.coproc_adc_read]{ if (rd) rd(); }),
                "One-shot reading of the three test ADC inputs "
                "(GP26/27/28 = ch0-2, 0-3300 mV)."));
            MenuItem ar = leaf("ADC", []{});
            ar.label_fn = [get = ctx.coproc_adc_result]{
                return std::string("ADC: ") + (get ? get() : std::string("n/a"));
            };
            pt.push_back(with_desc(std::move(ar),
                "Millivolts per channel from the last Read ADC Now."));
        }
        out.push_back(with_desc(submenu("Peripheral Test", std::move(pt)),
            "Exercise the pre-assigned test pins for the planned peripherals: "
            "4 servo channels (GP6-9), a WS2812 LED zone (GP22) and 3 ADC "
            "inputs (GP26-28). TTP223 touch pads (GP0/1/12/16/17/18) test "
            "themselves \xe2\x80\x94 touching one fires its boop zone / "
            "mapped function. See docs/wiring-guide.md."));
    }

    if (!ctx.coproc_cfg_p) return out;    // the Pins editor needs the live config

    input::CoprocConfig* C    = ctx.coproc_cfg_p;
    PfMax7219Layout*     MX   = ctx.pf_max7219_p;   // MAX bridge pin roles
    json*                cfgr = ctx.cfg_root;

    // Board variant, read live everywhere below so the Board picker takes
    // effect immediately.
    auto variant = [C]{ return sys::pico_variant_from_id(C->variant); };

    // "GP26  ADC  I2C1 SDA  (LED)" — everything a user needs to judge a pin,
    // in one label. The I2C role comes from the RP2350's fixed mux (GP % 4).
    auto gp_label = [variant](int gp) -> std::string {
        std::string s = "GP" + std::to_string(gp);
        if (sys::pico_gp_is_adc(variant(), gp)) s += "  ADC";
        s += std::string("  ") + sys::pico_gp_i2c(gp);
        if (const char* r = sys::pico_gp_reserved(variant(), gp)) {
            s += "  ("; s += r; s += ")";
        }
        return s;
    };

    // Role + colour of a GP for the diagram: button / LED / MAX bridge /
    // board-reserved / free. (The firmware-fixed voice pins aren't visible to
    // the host, so they're flagged in the submenu description instead.)
    auto role_of = [C, MX, variant](int gp) -> std::pair<std::string, ImU32> {
        for (size_t i = 0; i < C->pins.size(); ++i) {
            if (C->pins[i].gp == gp) {
                auto it = C->short_map.find(static_cast<int>(i));
                const std::string fn =
                    (it != C->short_map.end() && it->second != input::GpioFunc::None)
                    ? input::gpio_func_name(it->second) : "unset";
                return { "Btn " + std::to_string(i) + "  " + fn,
                         IM_COL32(45, 140, 225, 255) };
            }
            if (C->pins[i].led_gp == gp)
                return { "LED " + std::to_string(i), IM_COL32(240, 200, 70, 255) };
        }
        if (MX && MX->enabled && (MX->mode == "section" || MX->mode == "main")) {
            if (gp == 10) return { "MAX CLK", IM_COL32(230, 100, 180, 255) };
            if (gp == 11) return { "MAX DIN", IM_COL32(230, 100, 180, 255) };
            if (gp == 13) return { "MAX CS",  IM_COL32(230, 100, 180, 255) };
        }
        if (const char* r = sys::pico_gp_reserved(variant(), gp))
            return { std::string(r), IM_COL32(150, 120, 100, 255) };
        if (sys::pico_gp_is_adc(variant(), gp))
            return { "free (ADC)", IM_COL32(90, 190, 170, 255) };
        return { "free", IM_COL32(60, 180, 75, 255) };
    };

    auto used_elsewhere = [C](int gp, int except_i) {
        for (size_t j = 0; j < C->pins.size(); ++j) {
            if (static_cast<int>(j) == except_i) continue;
            if (C->pins[j].gp == gp || C->pins[j].led_gp == gp) return true;
        }
        return false;
    };

    // Persist + re-push. Compacts out pinless buttons and re-keys the function
    // maps so the firmware button ids stay contiguous.
    auto apply_coproc = [C, cfgr, reload]{
        std::vector<input::CoprocPin> np;
        std::map<int, input::GpioFunc> ns, nl;
        int nid = 0;
        for (size_t i = 0; i < C->pins.size(); ++i) {
            if (C->pins[i].gp < 0) continue;
            np.push_back(C->pins[i]);
            auto s = C->short_map.find(static_cast<int>(i));
            auto l = C->long_map.find(static_cast<int>(i));
            if (s != C->short_map.end()) ns[nid] = s->second;
            if (l != C->long_map.end())  nl[nid] = l->second;
            ++nid;
        }
        C->pins.swap(np); C->short_map.swap(ns); C->long_map.swap(nl);
        if (cfgr) {
            json& jc = (*cfgr)["inputs"]["coprocessor"];
            jc["variant"] = C->variant;
            json jpins = json::array(), jbtns = json::array();
            for (size_t i = 0; i < C->pins.size(); ++i) {
                const auto& p = C->pins[i];
                jpins.push_back(json{{"gp", p.gp}, {"pull", p.pull},
                    {"active_low", p.active_low}, {"led_gp", p.led_gp}});
                auto s = C->short_map.find(static_cast<int>(i));
                auto l = C->long_map.find(static_cast<int>(i));
                jbtns.push_back(json{{"id", static_cast<int>(i)},
                    {"short", s != C->short_map.end() ? input::gpio_func_id(s->second) : "none"},
                    {"long",  l != C->long_map.end()  ? input::gpio_func_id(l->second)  : "none"}});
            }
            jc["pins"] = std::move(jpins);
            jc["buttons"] = std::move(jbtns);
        }
        if (reload && *reload) (*reload)();
    };

    // One GP picker page, shared by a slot's Pin and LED Pin submenus (`field`
    // selects which CoprocPin member it writes). All 48 rows are built once;
    // visible_fn hides the GPs the current board doesn't break out and label_fn
    // keeps the ADC / I2C / reserved tags current, so a Board change re-shapes
    // the picker live.
    auto gp_picker = [C, variant, gp_label, used_elsewhere](
            int i, int input::CoprocPin::* field, const char* none_label) {
        auto in_range = [C, i]{ return i < static_cast<int>(C->pins.size()); };
        std::vector<MenuItem> items;
        items.push_back(leaf_sel(none_label,
            [C, i, field, in_range]{ if (in_range()) C->pins[i].*field = -1; },
            [C, i, field, in_range]{ return in_range() && C->pins[i].*field < 0; }));
        for (int gp = 0; gp <= 47; ++gp) {
            MenuItem pm = leaf_sel(gp_label(gp),
                [C, i, gp, field, in_range]{ if (in_range()) C->pins[i].*field = gp; },
                [C, i, gp, field, in_range]{ return in_range() && C->pins[i].*field == gp; });
            pm.label_fn   = [gp_label, gp]{ return gp_label(gp); };
            pm.visible_fn = [variant, gp]{ return sys::pico_variant_gp_exists(variant(), gp); };
            // Flag pins already used by another slot, or reserved by the board.
            pm.warn_fn = [used_elsewhere, variant, gp, i]{
                return used_elsewhere(gp, i) ||
                       sys::pico_gp_reserved(variant(), gp) != nullptr;
            };
            items.push_back(std::move(pm));
        }
        return items;
    };

    // Per-button function picker — keyed by slot index (stable across the
    // compaction apply_coproc does, because rows re-read C->pins[i] live).
    auto fn_picker = [C](int i, bool is_long) {
        std::vector<MenuItem> items;
        for (int k = 0; k < input::gpio_func_count(); ++k) {
            const auto f = static_cast<input::GpioFunc>(k);
            items.push_back(leaf_sel(input::gpio_func_name(f),
                [C, i, is_long, f]{ (is_long ? C->long_map : C->short_map)[i] = f; },
                [C, i, is_long, f]{ auto& m = is_long ? C->long_map : C->short_map;
                    auto it = m.find(i); return it != m.end() && it->second == f; }));
        }
        return items;
    };

    // Button slots — one row per configured button, hidden past the live count.
    auto btn_count = [C]{ return static_cast<int>(C->pins.size()); };
    std::vector<MenuItem> slot_rows = make_dynamic_rows(kMaxCoprocButtons, btn_count,
        [&](int i) -> MenuItem {
            auto in_range = [C, i]{ return i < static_cast<int>(C->pins.size()); };
            std::vector<MenuItem> pulls = {
                leaf_sel("Pull Up",
                    [C, i, in_range]{ if (in_range()) C->pins[i].pull = "up"; },
                    [C, i, in_range]{ return in_range() && C->pins[i].pull == "up"; }),
                leaf_sel("Pull Down",
                    [C, i, in_range]{ if (in_range()) C->pins[i].pull = "down"; },
                    [C, i, in_range]{ return in_range() && C->pins[i].pull == "down"; }),
                leaf_sel("None",
                    [C, i, in_range]{ if (in_range()) C->pins[i].pull = "none"; },
                    [C, i, in_range]{ return in_range() && C->pins[i].pull == "none"; }),
            };
            std::vector<MenuItem> slot;
            slot.push_back(submenu("Pin",         gp_picker(i, &input::CoprocPin::gp, "(unused)")));
            slot.push_back(submenu("Short Press", fn_picker(i, false)));
            slot.push_back(submenu("Long Press",  fn_picker(i, true)));
            slot.push_back(submenu("Pull",        std::move(pulls)));
            slot.push_back(toggle("Active Low",
                [C, i, in_range]{ return in_range() && C->pins[i].active_low; },
                [C, i, in_range](bool v){ if (in_range()) C->pins[i].active_low = v; }));
            slot.push_back(submenu("LED Pin",     gp_picker(i, &input::CoprocPin::led_gp, "(none)")));
            slot.push_back(leaf("Remove This Button",
                [C, i, apply_coproc]{
                    if (i < static_cast<int>(C->pins.size())) C->pins[i].gp = -1;
                    apply_coproc();   // compacts it out + re-pushes
                }));
            MenuItem m = submenu("Button", std::move(slot));
            m.label_fn = [C, i]() -> std::string {
                if (i >= static_cast<int>(C->pins.size())) return "";
                const int gp = C->pins[i].gp;
                auto it = C->short_map.find(i);
                const std::string fn =
                    (it != C->short_map.end() && it->second != input::GpioFunc::None)
                    ? input::gpio_func_name(it->second) : "unset";
                char b[96];
                if (gp < 0) std::snprintf(b, sizeof b, "Btn %d  (no pin)  %s", i, fn.c_str());
                else        std::snprintf(b, sizeof b, "Btn %d  GP%d  %s", i, gp, fn.c_str());
                return std::string(b);
            };
            return m;
        });

    // Board selector — which RP2350 the coprocessor runs on.
    auto board_pick = [C, apply_coproc](sys::PicoVariant v) {
        return leaf_sel(sys::pico_variant_name(v),
            [C, v, apply_coproc]{ C->variant = sys::pico_variant_id(v); apply_coproc(); },
            [C, v]{ return sys::pico_variant_from_id(C->variant) == v; });
    };
    std::vector<MenuItem> board_items = {
        board_pick(sys::PicoVariant::Rp2350a),
        board_pick(sys::PicoVariant::PicoPlus2),
        board_pick(sys::PicoVariant::PicoLipo2XlW),
        board_pick(sys::PicoVariant::Raw),
    };
    MenuItem board_item = with_desc(submenu("Board", std::move(board_items)),
        "Which RP2350 board the coprocessor runs on. RP2350 (Pico 2) breaks out "
        "GP0-22 + GP26-28 (ADC GP26-28); the RP2350B boards expose GP0-47 (ADC "
        "GP40-47) with their onboard-reserved pins flagged. Raw is a "
        "board-agnostic GP0-47 view. Takes effect immediately \xe2\x80\x94 the "
        "Pins picker and diagram follow it.");
    board_item.label_fn = [C]{ return std::string("Board:  ") +
        sys::pico_variant_name(sys::pico_variant_from_id(C->variant)); };
    out.push_back(std::move(board_item));

    std::vector<MenuItem> pins_menu;
    pins_menu.push_back(with_desc(leaf("Add Button",
        [C]{ if (static_cast<int>(C->pins.size()) < kMaxCoprocButtons)
                 C->pins.push_back(input::CoprocPin{}); }),
        "Append a button slot; open it to pick a free pin + function."));
    for (auto& r : slot_rows) pins_menu.push_back(std::move(r));
    pins_menu.push_back(with_desc(leaf("Apply & Reload",
        [apply_coproc]{ apply_coproc(); }),
        "Save the pin map + button functions to config and re-push them "
        "to the coprocessor (reconnects the link). Applies live."));

    // Context-panel visualizer: the Pico 2 physical header, or a logical GP
    // grid for the RP2350B boards / raw. Reads the variant live, so it follows
    // the Board picker. Colours: free green, button blue, LED amber, MAX pink,
    // board-reserved brown, power/ground per the shared header palette.
    MenuItem pins_item = submenu("Pins", std::move(pins_menu));
    pins_item.context_panel_title = "Coprocessor Pins";
    pins_item.context_panel_draw = [role_of, variant](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
        const sys::PicoVariant pv = variant();
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        dl->AddText(font, fs * 1.05f, {o.x, o.y},
                    IM_COL32(230, 235, 240, 255), sys::pico_variant_name(pv));
        const float top = o.y + fs * 1.7f;
        if (pv == sys::PicoVariant::Rp2350a) {
            // Physical Pico 2 header: 2 columns × 20 pins (1-20 down the left,
            // 40-21 down the right, matching the board held USB-up).
            const int rows = 20;
            const float rh = std::max(9.f, (o.y + sz.y - top - 4.f) / rows);
            const float colw = (sz.x - 8.f) * 0.5f;
            auto cell = [&](const sys::PicoPin& p, float x, float y) {
                ImU32 col; std::string txt;
                if (p.gp < 0) { col = sys::pin_kind_color(p.kind); txt = p.label; }
                else { auto r = role_of(p.gp);
                       col = r.second; txt = std::string(p.label) + "  " + r.first; }
                dl->AddRectFilled({x, y + 1.f}, {x + rh - 3.f, y + rh - 2.f}, col, 2.f);
                dl->AddText(font, fs * 0.68f, {x + rh + 2.f, y + (rh - fs * 0.68f) * 0.5f},
                            IM_COL32(215, 220, 226, 255), txt.c_str());
            };
            for (int r = 0; r < rows; ++r) {
                cell(sys::kPico2Pins[r],      o.x + 4.f,        top + r * rh);
                cell(sys::kPico2Pins[39 - r], o.x + 4.f + colw, top + r * rh);
            }
        } else {
            // Logical GP grid (physical layouts differ per RP2350B board and
            // aren't needed for logical pin assignment).
            const int max_gp  = sys::pico_variant_max_gp(pv);
            const int per_col = 16;
            const int ncols   = (max_gp + per_col) / per_col;
            const float rh   = std::max(8.f, (o.y + sz.y - top - 4.f) / per_col);
            const float colw = (sz.x - 8.f) / std::max(1, ncols);
            for (int gp = 0; gp <= max_gp; ++gp) {
                const float x = o.x + 4.f + (gp / per_col) * colw;
                const float y = top + (gp % per_col) * rh;
                auto r = role_of(gp);
                char lbl[64]; std::snprintf(lbl, sizeof lbl, "GP%d %s", gp, r.first.c_str());
                dl->AddRectFilled({x, y + 1.f}, {x + rh - 3.f, y + rh - 2.f}, r.second, 2.f);
                dl->AddText(font, fs * 0.6f, {x + rh, y + (rh - fs * 0.6f) * 0.5f},
                            IM_COL32(215, 220, 226, 255), lbl);
            }
        }
    };
    out.push_back(with_desc(std::move(pins_item),
        "Visualise + edit the coprocessor's pins: each button's GPIO, "
        "function, pull, polarity and optional LED. Free pins green, "
        "buttons blue, LEDs amber, MAX7219 pink, board-reserved brown. "
        "Firmware-fixed pins aren't auto-detected \xe2\x80\x94 avoid them per "
        "your build: voice I2S GP16-18, I2C GP20/21, DAC reset GP22, mic on "
        "the board's first ADC pin; peripheral hub 1-Wire GP19, fans GP14/15. "
        "Apply & Reload pushes the map live."));
    return out;
}

std::vector<MenuItem> build_system_menu(MenuBuildContext& ctx)
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

    // Shared helpers from the build context. push_notif gets a thin local
    // wrapper so call sites keep the optional trailing actions argument the
    // original lambda had.
    auto push_notif = [pn = ctx.push_notif](NotifType type, std::string title,
                                            std::string body, float auto_dismiss_s,
                                            std::vector<NotifAction> actions = {}) {
        pn(type, std::move(title), std::move(body), auto_dismiss_s, std::move(actions));
    };
    auto make_builtin_theme_leaves = ctx.make_builtin_theme_leaves;

    // ── Headset controls ──────────────────────────────────────────────────────
    auto xr_gaze = std::make_shared<bool>(false);   // local mirror of the mode state
    auto xr_3d   = std::make_shared<bool>(false);
    std::vector<MenuItem> headset_menu = {
        with_desc(slider("Electrochromic Transparency", 0.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_dimming); },
            [xr, &state](float v){
                state.xr_dimming = static_cast<int>(v);
                if (xr) xr->set_dimming(static_cast<int>(v));
            }),
            "Darken the glasses' electrochromic film (0 = clear see-through, 9 = "
            "fully dimmed) so the display stands out against bright surroundings."),
        with_desc(slider("HUD Brightness", 1.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_hud_brightness); },
            [xr, &state](float v){
                state.xr_hud_brightness = static_cast<int>(v);
                if (xr) xr->set_hud_brightness(static_cast<int>(v));
            }),
            "Brightness of the glasses' on-board HUD layer."),
        with_desc(slider("Backlight Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
            }),
            "Display panel / backlight brightness."),
        with_desc(leaf("Recenter Display", [xr]{ if (xr) xr->recenter_tracking(); }),
            "Re-center the head-tracked view so straight-ahead is forward right now."),
        with_desc(toggle("Gaze Lock (0-DoF Mode)",
            [xr_gaze]{ return *xr_gaze; },
            [xr, xr_gaze](bool v){ *xr_gaze = v; if (xr) xr->toggle_gaze_lock(); }),
            "Lock the image to the glasses (0-DoF) instead of head-tracking it \xe2\x80\x94 "
            "the HUD stays put as you look around."),
        with_desc(toggle("3D Side-by-Side",
            [xr_3d]{ return *xr_3d; },
            [xr, xr_3d](bool v){ *xr_3d = v; if (xr) xr->set_3d_mode(v); }),
            "Switch the glasses panel to 3D side-by-side stereo mode."),
    };

    // ── Audio controls ────────────────────────────────────────────────────────
    // Beamforming / noise suppression handled by RP2350; CM5 controls output + volume.
    std::vector<MenuItem> output_menu = {
        leaf_sel("VITURE",     [audio]{ if (audio) audio->set_output(AudioOutput::VITURE);     }, [&state]{ return state.audio.output == 0; }),
        leaf_sel("Headphones", [audio]{ if (audio) audio->set_output(AudioOutput::HEADPHONES); }, [&state]{ return state.audio.output == 1; }),
        leaf_sel("HDMI",       [audio]{ if (audio) audio->set_output(AudioOutput::HDMI);       }, [&state]{ return state.audio.output == 2; }),
    };

    std::vector<MenuItem> audio_menu = {
        toggle("Audio",
            [audio]{ return audio ? audio->is_enabled() : false; },
            [audio](bool v){ if (audio) audio->set_enabled(v); }),
        slider("Volume", 25.f, 150.f, 5.f, " %",
            [audio]{ return audio ? audio->get_master_gain() * 100.f : 100.f; },
            [audio](float v){ if (audio) audio->set_master_gain(v / 100.f); }),
        submenu("Output", std::move(output_menu)),
    };

    // ── Clock & Timers ────────────────────────────────────────────────────────
    std::vector<MenuItem> custom_timer_menu = {
        slider("Minutes", 0.f, 99.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.custom_timer_min); },
            [&state](float v){ state.timer_alarm.custom_timer_min = static_cast<int>(v); }),
        slider("Seconds", 0.f, 59.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.custom_timer_sec); },
            [&state](float v){ state.timer_alarm.custom_timer_sec = static_cast<int>(v); }),
        leaf("Start", [&state]{
            int secs = state.timer_alarm.custom_timer_min * 60
                     + state.timer_alarm.custom_timer_sec;
            if (secs > 0) {
                state.timer_alarm.timer_active = true;
                state.timer_alarm.timer_end    = time(nullptr) + secs;
            }
        }),
    };

    // Shared 5/10/30/60-min presets (menu_shared); the deep tab adds the
    // "which preset is running" radio indicators — the quick wheel's copy
    // passes false and shows plain leaves.
    std::vector<MenuItem> timer_presets_menu = {
        submenu("Custom", std::move(custom_timer_menu)),
    };
    for (auto& t : menu_shared::timer_preset_items(
             state, /*show_selected_state=*/true, "Cancel Timer"))
        timer_presets_menu.push_back(std::move(t));

    std::vector<MenuItem> alarm_picker_menu = {
        slider("Hour",   0.f, 23.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.alarm_hour); },
            [&state](float v){ state.timer_alarm.alarm_hour = static_cast<int>(v); }),
        slider("Minute", 0.f, 59.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.alarm_minute); },
            [&state](float v){ state.timer_alarm.alarm_minute = static_cast<int>(v); }),
        leaf("Confirm", [&state]{
            time_t now = time(nullptr);
            struct tm t = *localtime(&now);
            t.tm_hour = state.timer_alarm.alarm_hour;
            t.tm_min  = state.timer_alarm.alarm_minute;
            t.tm_sec  = 0;
            time_t fire = mktime(&t);
            if (fire <= now) fire += 86400;
            state.timer_alarm.alarm_active  = true;
            state.timer_alarm.alarm_fire_at = fire;
        }),
        leaf("Delete Alarm", [&state]{
            state.timer_alarm.alarm_active    = false;
            state.timer_alarm.alarm_triggered = false;
        }),
    };

    std::vector<MenuItem> timers_alarm_menu = {
        submenu("Set Timer",   std::move(timer_presets_menu)),
        submenu("Set Alarm",   std::move(alarm_picker_menu)),
        leaf("Dismiss Alarm", [&state]{ state.timer_alarm.alarm_triggered = false; }),
    };

    // ── System / dev menu ─────────────────────────────────────────────────────

    std::vector<MenuItem> demo_menu = {
        leaf("Trigger Alarm", [state_ptr, push_notif]{
            if (!state_ptr) return;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx);
              state_ptr->timer_alarm.alarm_triggered = true; }
            push_notif(NotifType::Alarm, "Test Alarm", "Demo alarm fired", 0.f,
                {{"DISMISS", [](AppState& s){ s.timer_alarm.alarm_triggered = false; }}});
        }),
        leaf("Trigger Timer Done", [state_ptr, push_notif]{
            if (!state_ptr) return;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx);
              state_ptr->timer_alarm.timer_triggered = true; }
            push_notif(NotifType::Timer, "Timer Done", "Demo 5:00 timer expired", 0.f,
                {{"DISMISS", [](AppState& s){ s.timer_alarm.timer_triggered = false; }},
                 {"+2 MIN",  [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+120; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }},
                 {"+5 MIN",  [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+300; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }}});
        }),
        leaf("LoRa Message (Node-2)", [state_ptr, push_notif]{
            if (!state_ptr) return;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx);
              state_ptr->push_lora_message({2, time(nullptr), "Demo: en route to objective"}); }
            push_notif(NotifType::LoRa, "Node-2", "Demo: en route to objective", 8.f);
        }),
        leaf("App Toast", [push_notif]{
            push_notif(NotifType::App, "ProtoHUD", "Demo app notification", 4.f);
        }),
        leaf("Toast Stack (x4)", [push_notif]{
            push_notif(NotifType::Alarm, "Alarm",  "Demo alarm",  0.f);
            push_notif(NotifType::Timer, "Timer",  "Demo timer",  0.f);
            push_notif(NotifType::LoRa,  "Node-3", "Demo LoRa",  8.f);
            push_notif(NotifType::App,   "System", "Demo app",   4.f);
        }),
        leaf("Clear All", [state_ptr]{
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->notifs.dismiss_all();
            state_ptr->timer_alarm.alarm_triggered = false;
            state_ptr->timer_alarm.timer_triggered = false;
        }),
    };

    // Software gets its contents (Updates, Demo Mode, Profiles & Backup) appended
    // below once those menus are built; the old "Check for Updates" / "Pull &&
    // Rebuild" shell shortcuts were removed in favour of the in-HUD Updater.
    std::vector<MenuItem> software_menu;

    std::vector<MenuItem> fps_interval_menu = {
        leaf_sel("1 second",  [state_ptr]{ state_ptr->fps_avg_interval_s = 1;  }, [state_ptr]{ return state_ptr->fps_avg_interval_s == 1;  }),
        leaf_sel("5 seconds", [state_ptr]{ state_ptr->fps_avg_interval_s = 5;  }, [state_ptr]{ return state_ptr->fps_avg_interval_s == 5;  }),
        leaf_sel("10 seconds",[state_ptr]{ state_ptr->fps_avg_interval_s = 10; }, [state_ptr]{ return state_ptr->fps_avg_interval_s == 10; }),
    };

    std::vector<MenuItem> i2c_bus_menu = {
        leaf_sel("/dev/i2c-0", [state_ptr]{ std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_bus = "/dev/i2c-0"; }, [state_ptr]{ return state_ptr->i2c_scan_bus == "/dev/i2c-0"; }),
        leaf_sel("/dev/i2c-1", [state_ptr]{ std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_bus = "/dev/i2c-1"; }, [state_ptr]{ return state_ptr->i2c_scan_bus == "/dev/i2c-1"; }),
        leaf_sel("/dev/i2c-2", [state_ptr]{ std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_bus = "/dev/i2c-2"; }, [state_ptr]{ return state_ptr->i2c_scan_bus == "/dev/i2c-2"; }),
    };

    std::vector<MenuItem> diagnostics_menu = {
        leaf("Scan I2C Bus", [state_ptr]{
            bool already;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx); already = state_ptr->i2c_scan_busy; }
            if (!already) {
                { std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_busy = true; state_ptr->i2c_scan_results.clear(); }
                std::thread(run_i2c_scan, state_ptr).detach();
            }
        }),
        submenu("I2C Bus",    std::move(i2c_bus_menu)),
    };

    // ── HUD / Menu Presets ───────────────────────────────────────────────────────
    // Visual-only presets (HUD colors + menu accent/border/selection/scale). Built-in
    // coordinated themes apply instantly; custom ones are saved/loaded live (no
    // restart, unlike full Profiles). Load/delete lists are dynamic
    // (make_dynamic_rows — label_fn reads the manager live). kProfileSlots = max
    // dynamic rows shown for save/load lists.
    constexpr int kProfileSlots = 16;
    auto hud_presets_count = [hud_presets]{ return hud_presets ? hud_presets->count() : 0; };
    std::vector<MenuItem> hud_preset_delete_menu = make_dynamic_rows(
        kProfileSlots, hud_presets_count, [&](int i) -> MenuItem {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "preset";
        m.label_fn    = [hud_presets, i]{ return hud_presets ? hud_presets->name(i) : std::string(); };
        m.description = "Delete this HUD/menu preset permanently.";
        m.action = [state_ptr, hud_presets, i]{
            if (!state_ptr || !hud_presets) return;
            std::string nm = hud_presets->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->hud_preset_delete_name = nm;
        };
        return m;
    });

    std::vector<MenuItem> hud_presets_menu;
    // Built-in coordinated themes (instant apply).
    for (auto& t : make_builtin_theme_leaves())
        hud_presets_menu.push_back(with_desc(std::move(t),
            "Built-in theme: applies a coordinated HUD color + menu style instantly."));
    // Save current look as a named preset (on-screen keyboard).
    hud_presets_menu.push_back(with_desc(
        leaf("Save Current As...", [menu_sys_pp, state_ptr]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Preset Name", std::string(),
                [state_ptr](const std::string& name){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->hud_preset_save_name = name;
                });
        }),
        "Save the current HUD colors + menu style (accent, border, selection, UI "
        "scale) as a named preset. Applies live when loaded — no restart."));
    // Dynamic load slots.
    for (auto& m : make_dynamic_rows(kProfileSlots, hud_presets_count,
                                     [&](int i) -> MenuItem {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "preset";
        m.label_fn    = [hud_presets, i]{ return hud_presets ? hud_presets->name(i) : std::string(); };
        m.description = "Apply this HUD/menu preset (live, no restart).";
        m.action = [state_ptr, hud_presets, i]{
            if (!state_ptr || !hud_presets) return;
            std::string nm = hud_presets->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->hud_preset_load_name = nm;
        };
        return m;
    }))
        hud_presets_menu.push_back(std::move(m));
    hud_presets_menu.push_back(with_desc(submenu("Delete Preset", std::move(hud_preset_delete_menu)),
        "Remove a saved HUD/menu preset permanently."));

    // ── GPIO Visualizer ──────────────────────────────────────────────────────
    // Renders the Pi 5 / CM5 40-pin header in the context pane as a vertical
    // 2x20 grid, colour-coded per Pi Foundation pinout. Each pin has a list
    // of claimants (the things in cfg that use it); pins with one claimant
    // get a green outline, pins with two or more get a red "conflict"
    // outline, idle pins stay grey. Hover any pin to see its claimants,
    // bus speed (SPI), I²C address list, and any user note assigned to it.
    struct GpioVizState {
        bool show_pin_num    = true;
        bool show_primary    = true;
        bool show_secondary  = true;
        bool show_tertiary   = false;
        bool show_inactive   = true;
        bool show_user_notes = true;   // 4th column when cfg["gpio_user_notes"] is set
        bool show_live_state = false;  // small H/L badge from /dev/gpiochip0 readings
        int  filter_kind     = -1;     // -1 = no filter, else PinKind enum value
    };
    static GpioVizState gpio_viz;

    // (BCM) → list of human-readable claimants (e.g. "MAX7219 left.spi",
    // "BNO055 IMU", "HUB75 R1"). Pins with >1 claimant are conflicts.
    struct PinClaims {
        std::map<int, std::vector<std::string>> claimants;
        std::map<int, int>                      spi_speed_hz;   // BCM → bus speed (for MOSI lines)
    };
    auto gpio_pin_claims_uncached = [cfg_root, gpio_pins_p, gpio_slot_count]() -> PinClaims {
        PinClaims pc;
        if (!cfg_root) return pc;
        const json& cfg = *cfg_root;
        auto add = [&pc](int bcm, std::string who) {
            if (bcm >= 0) pc.claimants[bcm].push_back(std::move(who));
        };
        // Configurable GPIO switch map — claim each assigned pin, labelled with
        // its (short-press, else long-press) function. Reads the live slots so
        // unsaved menu edits show up immediately.
        if (gpio_pins_p) {
            for (int i = 0; i < gpio_slot_count; ++i) {
                const auto& s = gpio_pins_p[i];
                const bool used = s.short_fn != input::GpioFunc::None ||
                                  s.long_fn  != input::GpioFunc::None;
                if (s.gpio >= 0 && used) {
                    const auto fn = (s.short_fn != input::GpioFunc::None) ? s.short_fn : s.long_fn;
                    add(s.gpio, std::string("GPIO: ") + input::gpio_func_name(fn));
                }
            }
        }

        // I²C peripherals — each enabled sensor on /dev/i2c-1 claims SDA1/SCL1.
        auto add_i2c = [&](const char* key, const char* label) {
            if (!cfg.contains(key) || !cfg[key].is_object()) return;
            const auto& jk = cfg[key];
            if (!jk.value("enabled", false)) return;
            if (jk.value("i2c_bus", std::string("/dev/i2c-1")) != "/dev/i2c-1") return;
            add(2, label);
            add(3, label);
        };
        add_i2c("bno055",       "BNO055 IMU");
        add_i2c("mpu9250",      "MPU9250 IMU");
        add_i2c("boop",         "MPR121 boop");
        add_i2c("light_sensor", "BH1750 light");

        // SPI chains — every chain on /dev/spidev0.x claims SPI0 pins,
        // /dev/spidev1.x claims SPI1 pins. Record per-bus speed for the
        // hover tooltip and pick up GPIO-transport CS pins.
        auto walk_chains = [&](const json& jchains, const char* tag) {
            if (!jchains.is_array()) return;
            for (const auto& jc : jchains) {
                if (!jc.is_object()) continue;
                const std::string dev = jc.value("spi_device", std::string());
                const std::string name = jc.value("name", std::string("chain"));
                const std::string who  = std::string(tag) + " " + name;
                const int speed = jc.value("speed_hz", 1'000'000);
                if (dev.find("/dev/spidev0") == 0) {
                    add(10, who + " (MOSI)"); add(9,  who + " (MISO)");
                    add(11, who + " (SCLK)"); add(8,  who + " (CE0)");
                    add(7,  who + " (CE1)");
                    pc.spi_speed_hz[10] = std::max(pc.spi_speed_hz[10], speed);
                }
                if (dev.find("/dev/spidev1") == 0) {
                    add(20, who + " (MOSI)"); add(19, who + " (MISO)");
                    add(21, who + " (SCLK)"); add(16, who + " (CE2)");
                    add(17, who + " (CE1)");
                    pc.spi_speed_hz[20] = std::max(pc.spi_speed_hz[20], speed);
                }
                add(jc.value("gpio_cs_pin", -1), who + " (GPIO CS)");
            }
        };
        if (cfg.contains("protoface") && cfg["protoface"].is_object()) {
            const auto& jpf = cfg["protoface"];
            if (jpf.contains("max7219") && jpf["max7219"].is_object()) {
                const auto& jm = jpf["max7219"];
                walk_chains(jm.contains("chains") ? jm["chains"] : json::array(),
                            "MAX7219");
                add(jm.value("gpio_din_pin", -1), "MAX7219 GPIO bus DIN");
                add(jm.value("gpio_clk_pin", -1), "MAX7219 GPIO bus CLK");
            }
            if (jpf.contains("rgb_matrix") && jpf["rgb_matrix"].is_object()
                && jpf["rgb_matrix"].contains("chains"))
                walk_chains(jpf["rgb_matrix"]["chains"], "RGB matrix");

            // HUB75 + piomatter — uses the Adafruit RGB HAT pinout by default
            // (the most common shipping config). We tag these so a HUB75 build
            // shows the full pin claim, conflicts included.
            const std::string be = jpf.value("backend", std::string("hub75"));
            if (be == "hub75") {
                struct HubPin { int bcm; const char* name; };
                static constexpr HubPin kHub75[] = {
                    { 5,"R1"},{13,"G1"},{ 6,"B1"},{12,"R2"},{16,"G2"},{23,"B2"},
                    {22,"A" },{26,"B" },{27,"C" },{20,"D" },{24,"E" },
                    { 4,"OE"},{17,"CLK"},{21,"STB"},
                };
                for (const auto& h : kHub75)
                    add(h.bcm, std::string("HUB75 ") + h.name);
            }
        }
        // Accessory LEDs (WS2812) — single MOSI line on their chosen spidev.
        if (cfg.contains("accessory_leds") && cfg["accessory_leds"].is_object()) {
            const auto& jal = cfg["accessory_leds"];
            const std::string dev = jal.value("spi_device", std::string());
            const int speed = jal.value("speed_hz", 2'400'000);
            if (dev.find("/dev/spidev0") == 0) { add(10, "Accessory LEDs (MOSI)"); pc.spi_speed_hz[10] = std::max(pc.spi_speed_hz[10], speed); }
            if (dev.find("/dev/spidev1") == 0) { add(20, "Accessory LEDs (MOSI)"); pc.spi_speed_hz[20] = std::max(pc.spi_speed_hz[20], speed); }
        }
        return pc;
    };
    // Per-frame memo: the full-config walk above is invoked from label_fn /
    // warn_fn / panel draws — up to 40× per frame on the pin-picker page —
    // and its result only changes when the config changes. Cache it for the
    // duration of one ImGui frame (config edits land next frame, invisible).
    struct PinClaimsCache { int frame = -1; PinClaims pc; };
    auto pin_claims_cache = std::make_shared<PinClaimsCache>();
    auto gpio_pin_claims = [gpio_pin_claims_uncached, pin_claims_cache]() -> const PinClaims& {
        const int f = ImGui::GetFrameCount();
        if (pin_claims_cache->frame != f) {
            pin_claims_cache->pc    = gpio_pin_claims_uncached();
            pin_claims_cache->frame = f;
        }
        return pin_claims_cache->pc;
    };

    // I²C peripheral list for the SDA/SCL hover tooltip. (addr, label).
    auto i2c_peripherals = [cfg_root]() -> std::vector<std::pair<int, std::string>> {
        std::vector<std::pair<int, std::string>> out;
        if (!cfg_root) return out;
        const json& cfg = *cfg_root;
        auto add_if = [&](const char* key, int default_addr, const char* label) {
            if (!cfg.contains(key) || !cfg[key].is_object()) return;
            const auto& jk = cfg[key];
            if (!jk.value("enabled", false)) return;
            if (jk.value("i2c_bus", std::string("/dev/i2c-1")) != "/dev/i2c-1") return;
            const int addr = jk.value("i2c_addr", default_addr);
            out.push_back({addr, label});
        };
        add_if("bno055",       0x28, "BNO055 IMU");
        add_if("mpu9250",      0x68, "MPU9250 IMU");
        add_if("boop",         0x5A, "MPR121 boop");
        add_if("light_sensor", 0x23, "BH1750 light");
        return out;
    };

    // User notes — free-form labels per BCM line via cfg["gpio_user_notes"]
    // ("17": "My LED strip"). Returned as a copy so the draw lambda doesn't
    // hold a json& back into main()'s cfg longer than necessary.
    auto user_notes = [cfg_root]() -> std::map<int, std::string> {
        std::map<int, std::string> out;
        if (!cfg_root) return out;
        const json& cfg = *cfg_root;
        if (!cfg.contains("gpio_user_notes") || !cfg["gpio_user_notes"].is_object())
            return out;
        for (const auto& [k, v] : cfg["gpio_user_notes"].items()) {
            try {
                const int bcm = std::stoi(k);
                if (v.is_string()) out[bcm] = v.get<std::string>();
            } catch (...) { /* skip non-int keys */ }
        }
        return out;
    };

    // Per-rail current estimate (mA) from known device draws. Anything not
    // in the table doesn't contribute — better silently zero than wildly
    // off-by-orders.
    auto rail_currents_mA = [cfg_root]() -> std::pair<int, int> {   // {3v3, 5v}
        int rail3 = 0, rail5 = 0;
        if (!cfg_root) return {rail3, rail5};
        const json& cfg = *cfg_root;
        auto enabled = [&cfg](const char* key) {
            return cfg.contains(key) && cfg[key].is_object()
                && cfg[key].value("enabled", false);
        };
        if (enabled("bno055"))        rail3 +=  12;
        if (enabled("mpu9250"))       rail3 +=   4;
        if (enabled("boop"))          rail3 +=  30;
        if (enabled("light_sensor"))  rail3 +=   1;
        // Accessory LEDs (WS2812) — assume ~20mA/LED at moderate brightness.
        if (cfg.contains("accessory_leds") && cfg["accessory_leds"].is_object()) {
            const auto& jal = cfg["accessory_leds"];
            int total = 0;
            if (jal.contains("zones") && jal["zones"].is_array())
                for (const auto& z : jal["zones"]) total += z.value("count", 0);
            rail5 += total * 20;
        }
        // MAX7219 chains — ~80 mA per module at full brightness.
        if (cfg.contains("protoface") && cfg["protoface"].is_object()) {
            const auto& jpf = cfg["protoface"];
            const std::string be = jpf.value("backend", std::string("hub75"));
            int max7219_mods = 0;
            if (be == "max7219" && jpf.contains("max7219") && jpf["max7219"].is_object()
                && jpf["max7219"].contains("chains") && jpf["max7219"]["chains"].is_array()) {
                for (const auto& jc : jpf["max7219"]["chains"]) {
                    if (!jc.is_object()) continue;
                    if (jc.contains("module_positions") && jc["module_positions"].is_array())
                        max7219_mods += static_cast<int>(jc["module_positions"].size());
                    else
                        max7219_mods += jc.value("cols_chips", 1) * jc.value("rows_chips", 1);
                }
            }
            rail5 += max7219_mods * 80;
        }
        return {rail3, rail5};
    };

    // The MenuContextPanelDraw lambda holds all of build_menu's GPIO helpers by
    // value (each captures cfg by reference, owned by main() which outlives
    // the menu system). gpio_viz has static storage so we capture its address.
    GpioVizState* gpvz_draw = &gpio_viz;
    auto draw_gpio_visualizer =
        [gpio_pin_claims, i2c_peripherals, user_notes, rail_currents_mA,
         gpvz_draw]() -> MenuContextPanelDraw {
        return [gpio_pin_claims, i2c_peripherals, user_notes, rail_currents_mA,
                gpvz_draw]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            const PinClaims& claims = gpio_pin_claims();
            const auto i2c         = i2c_peripherals();
            const auto notes       = user_notes();
            const auto [ma3, ma5]  = rail_currents_mA();
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            // Live-state reader — lazily opens /dev/gpiochip0 the first time
            // the toggle is enabled. Lines already claimed by another consumer
            // (SPI/I²C driver, piomatter, MAX7219 chain) silently fail to
            // claim, and read() returns -1 for those. Static so the fds
            // persist across draws.
            static sys::GpioInputReader reader;
            static bool reader_attempted = false;
            if (gpvz_draw->show_live_state && !reader_attempted) {
                // Only request lines NOT already claimed by another subsystem.
                // Requesting a pin piomatter is driving (HUB75 panels) steals it
                // and blanks the face — so skip anything in the claims map.
                std::vector<int> wanted;
                for (int i = 0; i < 28; ++i)
                    if (claims.claimants.find(i) == claims.claimants.end())
                        wanted.push_back(i);  // free BCM line
                reader.open(wanted);
                reader_attempted = true;
            }

            // Light rounded card + white border behind the visualizer, then
            // inset the working area so content never touches the edges.
            {
                const ImU32 card_bg     = IM_COL32(214, 221, 228, 252);
                const ImU32 card_border = IM_COL32(255, 255, 255, 255);
                dl->AddRectFilled(o, {o.x + sz.x, o.y + sz.y}, card_bg, 10.f);
                dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, card_border, 10.f, 0, 2.f);
            }
            o.x += 8.f; o.y += 6.f; sz.x -= 16.f; sz.y -= 12.f;
            const ImU32 text_dark = IM_COL32(22, 26, 32, 255);   // body text on the light card
            const ImU32 text_dim  = IM_COL32(78, 86, 96, 255);   // secondary text

            // Layout: a dedicated text column on the left (title, rail
            // estimate, hovered-pin details — whatever used to live in the
            // floating tooltip) and the 2x20 grid pushed to the right so
            // the two never overlap. Text column width is a fraction of the
            // pane that scales sensibly between docked + expanded views.
            const float label_fs   = std::max(9.f, fs * 0.72f);
            const float info_w_min = 180.f;
            const float info_w     = std::clamp(sz.x * 0.34f, info_w_min,
                                                 sz.x * 0.55f);
            const float info_x     = o.x;
            const float grid_pane_x = o.x + info_w + 8.f;
            const float grid_pane_w = std::max(120.f, sz.x - info_w - 8.f);

            // Left text column: title + rail-current estimate (top), then a
            // "Pin info" block that follows the hovered pin.
            dl->AddText(font, fs * 1.05f, {info_x, o.y},
                        text_dark, "GPIO Header");
            dl->AddText(font, fs * 0.7f, {info_x, o.y + fs * 1.05f},
                        text_dim, "Pi 5 / CM5 — 40-pin");
            char rail_buf[96];
            std::snprintf(rail_buf, sizeof(rail_buf),
                          "3.3V ~%d mA   5V ~%d mA", ma3, ma5);
            const ImU32 rail_col = (ma5 > 2500 || ma3 > 500)
                ? IM_COL32(200, 110,  20, 255) : text_dim;
            dl->AddText(font, fs * 0.75f, {info_x, o.y + fs * 1.85f},
                        text_dim, "Est. rail draw:");
            dl->AddText(font, fs * 0.85f, {info_x, o.y + fs * 2.65f},
                        rail_col, rail_buf);

            const float info_block_top = o.y + fs * 4.0f;

            // Vertically centre the 20-row grid with equal top/bottom margin,
            // zooming the rows up to a shared max so both panels match.
            const float vmargin = 12.f;
            const float usable  = std::max(80.f, sz.y - 2.f * vmargin);
            const float row_h   = std::max(14.f, std::min(34.f, usable / 20.f));
            const float grid_h  = row_h * 20.f;
            const float top     = o.y + vmargin + std::max(0.f, (usable - grid_h) * 0.5f);
            const float pin_sz  = row_h * 0.80f;

            const float center_x    = grid_pane_x + grid_pane_w * 0.5f;
            const float pin_left_x  = center_x - pin_sz - 3.f;
            const float pin_right_x = center_x + 3.f;

            const ImU32 outline_active   = IM_COL32( 40, 170,  60, 255);
            const ImU32 outline_conflict = IM_COL32(220,  50,  35, 255);
            const ImU32 outline_inactive = IM_COL32( 95, 105, 115, 220);
            const ImU32 live_high_col    = IM_COL32( 40, 170,  60, 255);
            const ImU32 live_low_col     = IM_COL32(110, 120, 130, 230);

            // Hover state for the tooltip.
            const ImVec2 mp = ImGui::GetMousePos();
            int           hovered_bcm = -1;
            sys::PinKind  hovered_kind = sys::PinKind::Gpio;
            std::string   hovered_primary;
            ImVec2        hover_pos{};

            // Three columns per pin, ordered outward from the indicator box:
            //   [GPIO #]  [primary function]  [user / predefined function]
            // Each is its own grey box; per-column widths are uniform across pins.
            auto note_for = [&notes](int bcm) -> const char* {
                if (bcm < 0) return nullptr;
                auto it = notes.find(bcm);
                return (it == notes.end()) ? nullptr : it->second.c_str();
            };
            std::string c1[40], c2[40], c3[40]; ImU32 c3col[40] = {};
            for (int idx = 0; idx < 40; ++idx) {
                const sys::GpioPin& p = sys::kPi40Pins[idx];
                c1[idx] = (p.bcm >= 0) ? ("GPIO" + std::to_string(p.bcm))
                                       : std::string(p.primary ? p.primary : "");
                std::string fn = (p.secondary && p.secondary[0]) ? p.secondary : "";
                if (p.tertiary && p.tertiary[0]) { if (!fn.empty()) fn += " / "; fn += p.tertiary; }
                c2[idx] = fn;
                std::string cl;
                if (p.bcm >= 0) {
                    auto c = claims.claimants.find(p.bcm);
                    if (c != claims.claimants.end() && !c->second.empty()) cl = c->second.front();
                }
                const char* note = gpvz_draw->show_user_notes ? note_for(p.bcm) : nullptr;
                if (!cl.empty()) {
                    c3[idx]    = cl;
                    c3col[idx] = (cl.rfind("GPIO: ", 0) == 0) ? IM_COL32(120, 170, 235, 255)  // GPIO map
                                                              : IM_COL32(235, 180,  90, 255); // hardware
                } else if (note) {
                    c3[idx]    = note;
                    c3col[idx] = IM_COL32(150, 195, 235, 255);                                 // user note
                }
            }
            auto colw = [&](std::string* arr) {
                float w = 0.f;
                for (int i = 0; i < 40; ++i)
                    if (!arr[i].empty())
                        w = std::max(w, font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, arr[i].c_str()).x);
                return w;
            };
            const float box_pad = 5.f, colgap = 3.f;
            const float side_w_l = (pin_left_x - 4.f) - grid_pane_x;
            const float side_w_r = (grid_pane_x + grid_pane_w) - (pin_right_x + pin_sz + 4.f);
            const float r1 = colw(c1), r2 = colw(c2), r3 = colw(c3);
            float w1 = r1 > 0.f ? r1 + box_pad * 2.f : 0.f;
            float w2 = r2 > 0.f ? r2 + box_pad * 2.f : 0.f;
            float w3 = r3 > 0.f ? r3 + box_pad * 2.f : 0.f;
            {
                const float avail = std::max(40.f, std::min(side_w_l, side_w_r));
                const float total = w1 + (w2 > 0.f ? w2 + colgap : 0.f) + (w3 > 0.f ? w3 + colgap : 0.f);
                if (total > avail && total > 0.f) { const float k = avail / total; w1 *= k; w2 *= k; w3 *= k; }
            }
            const ImU32 desc_bg = IM_COL32(56, 62, 70, 240);
            const ImU32 c1col   = IM_COL32(230, 234, 238, 255);   // GPIO #
            const ImU32 c2col   = IM_COL32(180, 188, 196, 255);   // primary function
            auto draw_cell = [&](float bx0, float w, float cy, const std::string& t, ImU32 col, bool dim) {
                if (t.empty() || w < 6.f) return;
                ImU32 bg = desc_bg;
                if (dim) { bg = (bg & 0x00FFFFFFu) | (110u << 24); col = (col & 0x00FFFFFFu) | (110u << 24); }
                dl->AddRectFilled({bx0, cy}, {bx0 + w, cy + pin_sz}, bg, 3.f);
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, t.c_str());
                dl->PushClipRect({bx0, cy}, {bx0 + w, cy + pin_sz}, true);
                dl->AddText(font, label_fs, {bx0 + box_pad, cy + (pin_sz - ts.y) * 0.5f}, col, t.c_str());
                dl->PopClipRect();
            };
            auto draw_cols = [&](int idx, float cy, bool left, bool dim) {
                if (left) {
                    float x = pin_left_x - 4.f;
                    draw_cell(x - w1, w1, cy, c1[idx], c1col, dim);                 x -= w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x - w2, w2, cy, c2[idx], c2col, dim); x -= w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x - w3, w3, cy, c3[idx], c3col[idx], dim);
                } else {
                    float x = pin_right_x + pin_sz + 4.f;
                    draw_cell(x, w1, cy, c1[idx], c1col, dim);                       x += w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x, w2, cy, c2[idx], c2col, dim);       x += w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x, w3, cy, c3[idx], c3col[idx], dim);
                }
            };

            for (int row = 0; row < 20; ++row) {
                const sys::GpioPin& pl = sys::kPi40Pins[row * 2];     // odd phys (left col)
                const sys::GpioPin& pr = sys::kPi40Pins[row * 2 + 1]; // even phys (right col)
                const float y = top + row * row_h;
                const float cy = y + (row_h - pin_sz) * 0.5f;

                auto draw_pin = [&](float x, const sys::GpioPin& p, bool right_side) {
                    const bool dim_by_filter = gpvz_draw->filter_kind >= 0 &&
                        static_cast<int>(p.kind) != gpvz_draw->filter_kind;
                    ImU32 fill = sys::pin_kind_color(p.kind);
                    if (dim_by_filter)
                        fill = (fill & 0x00FFFFFFu) | (90u << 24);

                    auto it = (p.bcm >= 0) ? claims.claimants.find(p.bcm)
                                           : claims.claimants.end();
                    const bool is_active   = it != claims.claimants.end();
                    const bool is_conflict = is_active && it->second.size() > 1;
                    const ImU32 outline = is_conflict ? outline_conflict
                                       : is_active   ? outline_active
                                       : (gpvz_draw->show_inactive
                                          ? outline_inactive : 0);
                    dl->AddRectFilled({x, cy}, {x + pin_sz, cy + pin_sz}, fill, 3.f);
                    if (outline)
                        dl->AddRect({x, cy}, {x + pin_sz, cy + pin_sz},
                                    outline, 3.f, 0,
                                    is_conflict ? 3.f : (is_active ? 2.5f : 1.5f));
                    if (gpvz_draw->show_pin_num) {
                        char buf[6];
                        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.physical));
                        const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, buf);
                        const float tx = x + (pin_sz - ts.x) * 0.5f;
                        const float ty = cy + (pin_sz - ts.y) * 0.5f;
                        // Bold white with an 8-direction black outline.
                        const ImU32 blk = IM_COL32(0, 0, 0, 235);
                        for (int dx = -1; dx <= 1; ++dx)
                            for (int dy = -1; dy <= 1; ++dy)
                                if (dx || dy) dl->AddText(font, label_fs, {tx + dx, ty + dy}, blk, buf);
                        dl->AddText(font, label_fs, {tx, ty}, IM_COL32(255, 255, 255, 255), buf);
                    }
                    // Live state badge — small H/L dot inside the square's
                    // corner. Skipped for non-GPIO pins (power / GND / ID).
                    if (gpvz_draw->show_live_state && p.bcm >= 0) {
                        const int v = reader.read(p.bcm);
                        if (v >= 0) {
                            const ImU32 col = v ? live_high_col : live_low_col;
                            dl->AddCircleFilled({x + pin_sz - 4.f, cy + 4.f}, 2.5f, col);
                        }
                    }
                    // Hover detection (over square OR its description columns).
                    const float side_w = w1 + (w2 > 0.f ? w2 + colgap : 0.f)
                                            + (w3 > 0.f ? w3 + colgap : 0.f) + 6.f;
                    const float hit_l = right_side ? x : (x - side_w);
                    const float hit_r = right_side ? (x + pin_sz + side_w)
                                                   : (x + pin_sz);
                    if (mp.x >= hit_l && mp.x < hit_r &&
                        mp.y >= cy && mp.y < cy + pin_sz) {
                        hovered_bcm     = p.bcm;
                        hovered_kind    = p.kind;
                        hovered_primary = p.primary;
                        hover_pos       = {x + pin_sz * 0.5f, cy + pin_sz};
                    }
                };
                draw_pin(pin_left_x,  pl, false);
                draw_pin(pin_right_x, pr, true);

                const bool dim_l = gpvz_draw->filter_kind >= 0 &&
                    static_cast<int>(pl.kind) != gpvz_draw->filter_kind;
                const bool dim_r = gpvz_draw->filter_kind >= 0 &&
                    static_cast<int>(pr.kind) != gpvz_draw->filter_kind;

                // Three description columns on each side.
                draw_cols(row * 2,     cy, true,  dim_l);
                draw_cols(row * 2 + 1, cy, false, dim_r);
            }

            // Pin info — used to be a floating tooltip near the cursor;
            // now lives in the left text column so it never overlaps the
            // grid and shows even without active hover. Default copy
            // points the user at the grid.
            const bool   have_hover = hovered_bcm >= 0 || !hovered_primary.empty();
            const ImU32  hint_col   = text_dim;
            const ImU32  body_col   = text_dark;
            const float  info_lh    = label_fs + 2.f;
            float        info_y     = info_block_top;
            // Section header — coloured by conflict state when applicable.
            auto cit = claims.claimants.find(hovered_bcm);
            const bool conflict = (cit != claims.claimants.end()
                                   && cit->second.size() > 1);
            const ImU32 head_col = conflict ? outline_conflict
                                            : IM_COL32(40, 95, 165, 255);
            dl->AddText(font, fs * 0.8f, {info_x, info_y},
                        head_col, have_hover ? "Pin Info" : "Hover");
            info_y += fs * 1.0f;
            if (!have_hover) {
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "Hover a pin to see its");
                info_y += info_lh;
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "claimants, bus speed,");
                info_y += info_lh;
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "I\xc2\xb2""C peripherals, etc.");
            } else {
                std::vector<std::string> lines;
                lines.push_back(hovered_primary.empty() ? std::string("(unknown)")
                                                       : hovered_primary);
                if (cit != claims.claimants.end()) {
                    lines.push_back(conflict ? "CONFLICT — claimants:"
                                             : "Used by:");
                    for (const auto& w : cit->second) lines.push_back("  " + w);
                } else if (hovered_bcm >= 0) {
                    lines.push_back("(idle)");
                }
                auto sit = claims.spi_speed_hz.find(hovered_bcm);
                if (sit != claims.spi_speed_hz.end()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Bus speed: %.2f MHz",
                                  sit->second / 1e6);
                    lines.push_back(buf);
                }
                if (hovered_kind == sys::PinKind::I2c) {
                    lines.push_back("");
                    lines.push_back("I\xc2\xb2""C /dev/i2c-1");
                    if (i2c.empty()) lines.push_back("  (none enabled)");
                    else for (const auto& [addr, name] : i2c) {
                        char buf[80];
                        std::snprintf(buf, sizeof(buf), "  0x%02X  %s",
                                      addr, name.c_str());
                        lines.push_back(buf);
                    }
                }
                auto nit = notes.find(hovered_bcm);
                if (nit != notes.end()) {
                    lines.push_back("");
                    lines.push_back("Note: " + nit->second);
                }
                // Render top-down, stop if we'd overrun the pane.
                const float info_max_y = o.y + sz.y - info_lh;
                for (const auto& l : lines) {
                    if (info_y > info_max_y) break;
                    dl->AddText(font, label_fs, {info_x, info_y},
                                body_col, l.c_str());
                    info_y += info_lh;
                }
            }
            // Suppress the now-unused float-tooltip anchor (kept hover_pos
            // around in case we ever bring back an alt tooltip).
            (void)hover_pos;
        };
    };

    // Pinout export — dumps the current header view to a text file the user
    // can paste into a build journal. Path is /tmp/protohud_pinout.txt for
    // simplicity (no menu picker for the location).
    auto export_pinout = [gpio_pin_claims, i2c_peripherals,
                          user_notes, rail_currents_mA]() {
        const PinClaims& pc      = gpio_pin_claims();
        const auto      i2c      = i2c_peripherals();
        const auto      notes    = user_notes();
        const auto [ma3, ma5]    = rail_currents_mA();
        const std::string path   = "/tmp/protohud_pinout.txt";
        std::ofstream f(path);
        if (!f) {
            std::fprintf(stderr, "[gpio] could not open %s for write\n", path.c_str());
            return;
        }
        f << "ProtoHUD GPIO header snapshot\n";
        f << "Estimated rail draw: 3.3V ~" << ma3 << " mA   5V ~" << ma5 << " mA\n\n";
        f << "Phys  BCM  Name      Alt1        Alt2       Used by / notes\n";
        f << "----  ---  --------  ----------  ---------  ----------------------------\n";
        for (const auto& p : sys::kPi40Pins) {
            char line[256];
            std::snprintf(line, sizeof(line), "%4d  %3d  %-8s  %-10s  %-9s  ",
                          static_cast<int>(p.physical),
                          static_cast<int>(p.bcm),
                          p.primary, p.secondary, p.tertiary);
            f << line;
            auto it = (p.bcm >= 0) ? pc.claimants.find(p.bcm) : pc.claimants.end();
            if (it != pc.claimants.end()) {
                if (it->second.size() > 1) f << "[CONFLICT] ";
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (i) f << " | ";
                    f << it->second[i];
                }
            }
            auto nit = notes.find(p.bcm);
            if (nit != notes.end()) {
                if (it != pc.claimants.end()) f << "  ";
                f << "note=\"" << nit->second << "\"";
            }
            f << "\n";
        }
        if (!i2c.empty()) {
            f << "\nI2C bus /dev/i2c-1:\n";
            for (const auto& [addr, name] : i2c) {
                char ab[80];
                std::snprintf(ab, sizeof(ab), "  0x%02X  %s\n", addr, name.c_str());
                f << ab;
            }
        }
        f.close();
        std::cerr << "[gpio] pinout exported to " << path << "\n";
    };

    GpioVizState* gpvz = &gpio_viz;   // static storage — capture by pointer is safe

    // Filter chip — single-select radio. -1 = no filter (everything in colour).
    auto filter_radio = [gpvz](const char* lbl, int v) -> MenuItem {
        return leaf_sel(lbl,
            [gpvz, v]{ gpvz->filter_kind = v; },
            [gpvz, v]{ return gpvz->filter_kind == v; });
    };
    std::vector<MenuItem> filter_menu = {
        filter_radio("None",   -1),
        filter_radio("GPIO",    static_cast<int>(sys::PinKind::Gpio)),
        filter_radio("I\xc2\xb2""C",  static_cast<int>(sys::PinKind::I2c)),
        filter_radio("SPI",     static_cast<int>(sys::PinKind::Spi)),
        filter_radio("UART",    static_cast<int>(sys::PinKind::Uart)),
        filter_radio("PWM",     static_cast<int>(sys::PinKind::Pwm)),
        filter_radio("Clock",   static_cast<int>(sys::PinKind::Gpclk)),
        filter_radio("3.3V",    static_cast<int>(sys::PinKind::Power3V3)),
        filter_radio("5V",      static_cast<int>(sys::PinKind::Power5V)),
        filter_radio("Ground",  static_cast<int>(sys::PinKind::Ground)),
    };

    std::vector<MenuItem> gpio_viz_menu = {
        toggle("Show Pin Numbers",
            [gpvz]{ return gpvz->show_pin_num; },
            [gpvz](bool v){ gpvz->show_pin_num = v; }),
        toggle("Show Primary Function",
            [gpvz]{ return gpvz->show_primary; },
            [gpvz](bool v){ gpvz->show_primary = v; }),
        toggle("Show Secondary Function",
            [gpvz]{ return gpvz->show_secondary; },
            [gpvz](bool v){ gpvz->show_secondary = v; }),
        toggle("Show Tertiary Function",
            [gpvz]{ return gpvz->show_tertiary; },
            [gpvz](bool v){ gpvz->show_tertiary = v; }),
        toggle("Show User Notes",
            [gpvz]{ return gpvz->show_user_notes; },
            [gpvz](bool v){ gpvz->show_user_notes = v; }),
        toggle("Outline Inactive Pins",
            [gpvz]{ return gpvz->show_inactive; },
            [gpvz](bool v){ gpvz->show_inactive = v; }),
        toggle("Show Live State (H/L)",
            [gpvz]{ return gpvz->show_live_state; },
            [gpvz](bool v){ gpvz->show_live_state = v; }),
        with_desc(submenu("Filter by Function", std::move(filter_menu)),
                  "Dim every pin whose function family doesn't match the "
                  "selected filter — quick way to spot all SPI lines or "
                  "all PWM-capable pins."),
        leaf("Export Pinout...", export_pinout),
    };
    MenuItem gpio_viz_item = with_panel(
        submenu("GPIO Visualizer", std::move(gpio_viz_menu)),
        "Pi 40-pin Header", draw_gpio_visualizer());
    gpio_viz_item.description =
        "Pi Foundation–style 2x20 view of the GPIO header.\n"
        "  • Squares are colour-coded by function family.\n"
        "  • Green outline = pin claimed by one thing in config.json. "
        "Red outline = conflict (multiple claimants).\n"
        "  • Hover a pin for its claimants, SPI bus speed, I\xc2\xb2""C addresses, "
        "and any user note assigned to it.\n"
        "  • Live State badge (toggle) reads /dev/gpiochip0 for pins not "
        "already claimed by another driver.\n"
        "  • User notes live at cfg[\"gpio_user_notes\"][\"<bcm>\"] = "
        "\"label\" and surface as a 4th column.\n"
        "  • Export Pinout… dumps the current view to /tmp/protohud_pinout.txt.\n"
        "  • Power-rail estimate (top of pane) sums known device current draws.\n"
        "  • Filter by Function dims everything not matching the selected family.";

    // ── GPIO pin picker (visual header page) ──────────────────────────────────
    // Helpers shared by the picker page + the slot list:
    //   gpio_hw_claimants  — non-GPIO peripherals holding a pin (HUB75/SPI/I²C…).
    //   gpio_other_slot    — another in-use slot bound to a pin (-1 = none).
    //   gpio_pin_selectable— a real GPIO line not held by hardware. Pins already
    //                        used by another *slot* stay selectable (override
    //                        allowed); the clash is flagged afterwards.
    //   gpio_slot_conflict — a slot whose pin collides with another slot or with
    //                        hardware; the slot list paints it red.
    auto gpio_hw_claimants = [gpio_pin_claims](int bcm) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (bcm < 0) return out;
        const PinClaims& pc = gpio_pin_claims();
        auto it = pc.claimants.find(bcm);
        if (it != pc.claimants.end())
            for (const auto& who : it->second)
                if (who.rfind("GPIO: ", 0) != 0) out.push_back(who);   // skip GPIO-map self-claims
        return out;
    };
    auto gpio_other_slot = [gpio_pins_p, gpio_slot_count](int bcm, int self_slot) -> int {
        if (bcm < 0 || !gpio_pins_p) return -1;
        for (int j = 0; j < gpio_slot_count; ++j) {
            if (j == self_slot) continue;
            const auto& s = gpio_pins_p[j];
            const bool used = s.short_fn != input::GpioFunc::None ||
                              s.long_fn  != input::GpioFunc::None;
            if (used && s.gpio == bcm) return j;
        }
        return -1;
    };
    auto gpio_pin_selectable = [gpio_hw_claimants](int bcm) -> bool {
        return bcm >= 0 && gpio_hw_claimants(bcm).empty();
    };
    auto gpio_slot_conflict = [gpio_pins_p, gpio_other_slot, gpio_hw_claimants](int i) -> bool {
        if (!gpio_pins_p) return false;
        const auto& s = gpio_pins_p[i];
        const bool used = s.short_fn != input::GpioFunc::None ||
                          s.long_fn  != input::GpioFunc::None;
        if (!used || s.gpio < 0) return false;
        return gpio_other_slot(s.gpio, i) >= 0 || !gpio_hw_claimants(s.gpio).empty();
    };

    // Context-panel renderer for the picker page. Mirrors the GPIO Visualizer's
    // look — a left info column plus a centred 2x20 header grid — on a light
    // rounded card with a white border. Each pin carries an easy-to-read label:
    // its claimant ("HUB75 R1"), "Slot N" if another slot uses it, else its
    // GPIO/alt-function name. Marking: green outline = this slot's pin, blue =
    // another slot, amber = hardware peripheral, dark = the cursor.
    auto draw_pin_picker_panel =
        [gpio_hw_claimants, gpio_other_slot, gpio_pin_selectable]
        (std::shared_ptr<int> focus, int self_slot,
         input::GpioPinCfg* slot_p) -> MenuContextPanelDraw {
        return [focus, self_slot, slot_p, gpio_hw_claimants, gpio_other_slot, gpio_pin_selectable]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs       = ImGui::GetFontSize();
            const float label_fs = std::max(9.f, fs * 0.72f);
            const int   fbcm     = focus  ? *focus      : -1;
            const int   assigned = slot_p ? slot_p->gpio : -1;

            // Light rounded card + white border behind the picker content.
            const ImU32 card_bg     = IM_COL32(214, 221, 228, 252);
            const ImU32 card_border = IM_COL32(255, 255, 255, 255);
            const ImU32 text_dark   = IM_COL32( 22,  26,  32, 255);
            const ImU32 text_dim    = IM_COL32( 78,  86,  96, 255);
            dl->AddRectFilled(o, {o.x + sz.x, o.y + sz.y}, card_bg, 10.f);
            dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, card_border, 10.f, 0, 2.f);
            const float cpad = 9.f;
            const ImVec2 co{o.x + cpad, o.y + cpad};
            const ImVec2 cs{sz.x - cpad * 2.f, sz.y - cpad * 2.f};

            // Per-pin state + claimant label, computed once.
            bool selectable[40]; int otherslot[40]; bool hardware[40];
            std::string claim[40];
            for (int idx = 0; idx < 40; ++idx) {
                const int b = sys::kPi40Pins[idx].bcm;
                auto hw = (b >= 0) ? gpio_hw_claimants(b) : std::vector<std::string>{};
                hardware[idx]   = !hw.empty();
                selectable[idx] = b >= 0 && !hardware[idx];
                otherslot[idx]  = (b >= 0) ? gpio_other_slot(b, self_slot) : -1;
                claim[idx]      = hw.empty() ? std::string() : hw.front();
            }

            const float info_w      = std::clamp(cs.x * 0.30f, 110.f, cs.x * 0.5f);
            const float info_x      = co.x;
            const float grid_pane_x = co.x + info_w + 6.f;
            const float grid_pane_w = std::max(120.f, cs.x - info_w - 6.f);

            // Vertically centre the 20-row grid with equal top/bottom margin,
            // zooming the rows up to a shared max so both panels match.
            const float vmargin = 12.f;
            const float usable  = std::max(80.f, cs.y - 2.f * vmargin);
            const float row_h   = std::max(13.f, std::min(34.f, usable / 20.f));
            const float grid_h  = row_h * 20.f;
            const float top     = co.y + vmargin + std::max(0.f, (usable - grid_h) * 0.5f);
            const float pin_sz  = row_h * 0.80f;
            const float center_x    = grid_pane_x + grid_pane_w * 0.5f;
            const float pin_left_x  = center_x - pin_sz - 2.f;
            const float pin_right_x = center_x + 2.f;
            const float side_w_l    = (pin_left_x - 4.f) - grid_pane_x;
            const float side_w_r    = (grid_pane_x + grid_pane_w) - (pin_right_x + pin_sz + 4.f);

            const ImU32 out_focus    = IM_COL32( 15,  18,  24, 255);   // dark = cursor (pops on light)
            const ImU32 out_assigned = IM_COL32( 40, 170,  60, 255);   // this slot
            const ImU32 out_other    = IM_COL32( 40, 110, 210, 255);   // another slot
            const ImU32 out_hardware = IM_COL32(205, 120,  20, 255);   // hardware peripheral

            // Three columns per pin, ordered outward from the indicator box:
            //   [GPIO #]  [primary function]  [user / predefined function]
            // Each is its own grey box; per-column widths are uniform across pins.
            std::string c1[40], c2[40], c3[40]; ImU32 c3col[40] = {};
            for (int idx = 0; idx < 40; ++idx) {
                const sys::GpioPin& p = sys::kPi40Pins[idx];
                c1[idx] = (p.bcm >= 0) ? ("GPIO" + std::to_string(p.bcm))
                                       : std::string(p.primary ? p.primary : "");
                c2[idx] = (p.secondary && p.secondary[0]) ? p.secondary : "";
                if (hardware[idx])            { c3[idx] = claim[idx];
                                                c3col[idx] = IM_COL32(235, 180, 90, 255); }
                else if (otherslot[idx] >= 0) { c3[idx] = "Slot " + std::to_string(otherslot[idx] + 1);
                                                c3col[idx] = IM_COL32(120, 170, 235, 255); }
            }
            auto colw = [&](std::string* arr) {
                float w = 0.f;
                for (int i = 0; i < 40; ++i)
                    if (!arr[i].empty())
                        w = std::max(w, font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, arr[i].c_str()).x);
                return w;
            };
            const float box_pad = 5.f, colgap = 3.f;
            const float r1 = colw(c1), r2 = colw(c2), r3 = colw(c3);
            float w1 = r1 > 0.f ? r1 + box_pad * 2.f : 0.f;
            float w2 = r2 > 0.f ? r2 + box_pad * 2.f : 0.f;
            float w3 = r3 > 0.f ? r3 + box_pad * 2.f : 0.f;
            {   // shrink to fit the side gap if the three columns are too wide
                const float avail = std::max(40.f, std::min(side_w_l, side_w_r));
                const float total = w1 + (w2 > 0.f ? w2 + colgap : 0.f) + (w3 > 0.f ? w3 + colgap : 0.f);
                if (total > avail && total > 0.f) { const float k = avail / total; w1 *= k; w2 *= k; w3 *= k; }
            }
            const ImU32 desc_bg = IM_COL32(56, 62, 70, 240);
            const ImU32 c1col   = IM_COL32(230, 234, 238, 255);   // GPIO #
            const ImU32 c2col   = IM_COL32(180, 188, 196, 255);   // primary function
            auto draw_cell = [&](float bx0, float w, float cy, const std::string& t, ImU32 col) {
                if (t.empty() || w < 6.f) return;
                dl->AddRectFilled({bx0, cy}, {bx0 + w, cy + pin_sz}, desc_bg, 3.f);
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, t.c_str());
                dl->PushClipRect({bx0, cy}, {bx0 + w, cy + pin_sz}, true);
                dl->AddText(font, label_fs, {bx0 + box_pad, cy + (pin_sz - ts.y) * 0.5f}, col, t.c_str());
                dl->PopClipRect();
            };
            auto draw_cols = [&](int idx, float cy, bool left) {
                if (left) {
                    float x = pin_left_x - 4.f;
                    draw_cell(x - w1, w1, cy, c1[idx], c1col);                 x -= w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x - w2, w2, cy, c2[idx], c2col); x -= w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x - w3, w3, cy, c3[idx], c3col[idx]);
                } else {
                    float x = pin_right_x + pin_sz + 4.f;
                    draw_cell(x, w1, cy, c1[idx], c1col);                       x += w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x, w2, cy, c2[idx], c2col);       x += w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x, w3, cy, c3[idx], c3col[idx]);
                }
            };

            // 8-direction black outline — kept for the white pin numbers so they
            // read on bright cells (descriptions use the grey box instead).
            auto otext = [&](float x, float y, ImU32 col, const char* s) {
                const ImU32 blk = IM_COL32(0, 0, 0, 225);
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        if (dx || dy) dl->AddText(font, label_fs, {x + dx, y + dy}, blk, s);
                dl->AddText(font, label_fs, {x, y}, col, s);
            };
            auto draw_pin_num = [&](float bx, float by, const char* s) {
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, s);
                otext(bx - ts.x * 0.5f, by - ts.y * 0.5f, IM_COL32(255, 255, 255, 255), s);
            };

            for (int row = 0; row < 20; ++row) {
                const int il = row * 2, ir = row * 2 + 1;
                const float y  = top + row * row_h;
                const float cy = y + (row_h - pin_sz) * 0.5f;
                auto draw_box = [&](float x, int idx) {
                    const sys::GpioPin& p = sys::kPi40Pins[idx];
                    ImU32 fill = sys::pin_kind_color(p.kind);
                    if (p.bcm >= 0 && !selectable[idx]) fill = (fill & 0x00FFFFFFu) | (120u << 24);
                    dl->AddRectFilled({x, cy}, {x + pin_sz, cy + pin_sz}, fill, 3.f);
                    ImU32 oc = 0; float ow = 2.f;
                    if      (p.bcm >= 0 && p.bcm == assigned) { oc = out_assigned; ow = 2.5f; }
                    else if (hardware[idx])                   { oc = out_hardware; }
                    else if (otherslot[idx] >= 0)             { oc = out_other; }
                    if (oc) dl->AddRect({x, cy}, {x + pin_sz, cy + pin_sz}, oc, 3.f, 0, ow);
                    if (p.bcm >= 0 && p.bcm == fbcm)
                        dl->AddRect({x - 1.5f, cy - 1.5f}, {x + pin_sz + 1.5f, cy + pin_sz + 1.5f},
                                    out_focus, 3.f, 0, 3.f);
                    char buf[6]; std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.physical));
                    draw_pin_num(x + pin_sz * 0.5f, cy + pin_sz * 0.5f, buf);
                };
                draw_box(pin_left_x,  il);
                draw_box(pin_right_x, ir);

                draw_cols(il, cy, true);
                draw_cols(ir, cy, false);
            }

            // Info column (dark text on the light card): focused pin + legend.
            dl->AddText(font, fs * 0.7f, {info_x, co.y}, text_dim, "Pi 5 / CM5 — 40-pin header");
            int fidx = -1;
            for (int idx = 0; idx < 40; ++idx)
                if (sys::kPi40Pins[idx].bcm >= 0 && sys::kPi40Pins[idx].bcm == fbcm) { fidx = idx; break; }
            float iy = co.y + fs * 1.5f;
            if (fidx >= 0) {
                const sys::GpioPin& fp = sys::kPi40Pins[fidx];
                char hdr[24]; std::snprintf(hdr, sizeof(hdr), "GPIO %d", fp.bcm);
                dl->AddText(font, fs * 0.95f, {info_x, iy}, text_dark, hdr);
                iy += fs * 1.1f;
                std::string fns = fp.primary;
                if (fp.secondary && fp.secondary[0]) fns += " / " + std::string(fp.secondary);
                if (fp.tertiary  && fp.tertiary[0])  fns += " / " + std::string(fp.tertiary);
                dl->AddText(font, fs * 0.7f, {info_x, iy}, text_dim, fns.c_str());
                iy += fs * 1.15f;
                if (hardware[fidx]) {
                    dl->AddText(font, fs * 0.74f, {info_x, iy}, out_hardware, "In use (hardware):");
                    iy += fs * 0.92f;
                    for (const auto& w : gpio_hw_claimants(fp.bcm)) {
                        dl->AddText(font, fs * 0.7f, {info_x + 6.f, iy}, text_dark, w.c_str());
                        iy += fs * 0.86f;
                    }
                } else if (fp.bcm == assigned) {
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, out_assigned, "This slot's pin");
                    iy += fs * 1.0f;
                } else if (otherslot[fidx] >= 0) {
                    char b[44]; std::snprintf(b, sizeof(b), "Also on Slot %d", otherslot[fidx] + 1);
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, out_other, b);
                    iy += fs * 0.9f;
                    dl->AddText(font, fs * 0.66f, {info_x, iy}, text_dim, "override → fix later");
                    iy += fs * 1.0f;
                } else {
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, IM_COL32(30, 150, 50, 255), "Available");
                    iy += fs * 1.0f;
                }
                iy += fs * 0.4f;
            }
            auto legend = [&](ImU32 c, const char* t) {
                dl->AddRect({info_x, iy + 2.f}, {info_x + 11.f, iy + 13.f}, c, 2.f, 0, 2.f);
                dl->AddText(font, fs * 0.66f, {info_x + 16.f, iy}, IM_COL32(20, 24, 30, 255), t);
                iy += fs * 0.98f;
            };
            legend(out_assigned, "this slot");
            legend(out_other,    "other slot");
            legend(out_hardware, "hardware");
            legend(out_focus,    "cursor");
        };
    };

    // ── GPIO Buttons (configurable pin → function map) ────────────────────────
    // ── RP2350 GPIO Expander (button coprocessor) ────────────────────────────
    // Built by build_coproc_expander_menu (file scope, above) so it's fully
    // independent of the on-board GPIO map. Routed to the top-level GPIO tab
    // when build_menu wires gpio_expander_out; otherwise it lands under the
    // GPIO Buttons submenu below (legacy placement).
    std::vector<MenuItem> coproc_menu;
    if (coproc_enabled_p) coproc_menu = build_coproc_expander_menu(ctx);
    if (ctx.gpio_expander_out && !coproc_menu.empty()) {
        for (auto& it : coproc_menu) ctx.gpio_expander_out->push_back(std::move(it));
        coproc_menu.clear();
    }

    // One submenu per assignable slot: BCM pin, short/long-press function, pull
    // bias, polarity, and long-press threshold. Edits the live gpio_pins array;
    // persisted to cfg["gpio"]["pins"] and applied on the next launch.
    MenuItem gpio_buttons_item;
    if (gpio_pins_p) {
        input::GpioPinCfg* GP = gpio_pins_p;
        // Build a function-picker submenu (all GpioFunc options) writing *slot.
        auto fn_picker = [](input::GpioFunc* slot) {
            std::vector<MenuItem> items;
            for (int i = 0; i < input::gpio_func_count(); ++i) {
                const auto f = static_cast<input::GpioFunc>(i);
                items.push_back(leaf_sel(input::gpio_func_name(f),
                    [slot, f]{ *slot = f; },
                    [slot, f]{ return *slot == f; }));
            }
            return items;
        };

        std::vector<MenuItem> gpio_btn_menu;
        if (gpio_inputs_enabled_p)
            gpio_btn_menu.push_back(with_desc(toggle("Enabled",
                [gpio_inputs_enabled_p]{ return *gpio_inputs_enabled_p; },
                [gpio_inputs_enabled_p, gpio_reload](bool v){
                    *gpio_inputs_enabled_p = v;
                    if (gpio_reload && *gpio_reload) (*gpio_reload)();  // apply now
                }),
                "Master switch for the GPIO button map. Applies immediately."));
        if (gpio_reload)
            gpio_btn_menu.push_back(with_desc(
                leaf("Apply Changes Now",
                     [gpio_reload]{ if (*gpio_reload) (*gpio_reload)(); }),
                "Rebuild the GPIO poller from the current slots so pin / "
                "function / pull / polarity edits take effect right away."));

        // RP2350 GPIO Expander fallback: when build_menu didn't wire the GPIO
        // tab, nest the expander controls (built near the top of this function)
        // here instead. Normally coproc_menu was already routed to
        // ctx.gpio_expander_out and this is empty.
        if (!coproc_menu.empty()) {
            gpio_btn_menu.push_back(with_desc(
                submenu("Button Coprocessor", std::move(coproc_menu)),
                "Optional external MCU (RP2350/RP2040) that handles button "
                "debounce and streams presses to the Pi \xE2\x80\x94 frees GPIO "
                "and offloads timing. See docs/coprocessor-input.md."));
        }

        for (int i = 0; i < gpio_slot_count; ++i) {
            input::GpioPinCfg* p = &GP[i];

            // Pin picker page for this slot: an "(unused)" entry plus one leaf per
            // header GPIO line, gated by gpio_pin_available so only free pins show.
            // Selecting a pin sets it and pops back here to the slot's options.
            auto picker_focus = std::make_shared<int>(p->gpio);
            std::vector<MenuItem> picker_items;
            picker_items.push_back(leaf_sel("(Unused / disable)",
                [p, menu_sys_pp]{ p->gpio = -1;
                                  if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); },
                [p]{ return p->gpio < 0; }));
            for (const auto& gp : sys::kPi40Pins) {
                if (gp.bcm < 0) continue;   // skip power / ground / ID pins
                const int bcm = gp.bcm;
                const std::string alt = (gp.secondary && gp.secondary[0]) ? gp.secondary : "";
                MenuItem pm;
                pm.type         = MenuItemType::LEAF;
                pm.label        = "GPIO " + std::to_string(bcm);
                pm.label_fn     = [bcm, alt, i, gpio_other_slot]{
                    std::string s = "GPIO " + std::to_string(bcm);
                    if (!alt.empty()) s += "   (" + alt + ")";
                    const int other = gpio_other_slot(bcm, i);
                    if (other >= 0) s += "   ! Slot " + std::to_string(other + 1);
                    return s;
                };
                pm.get_state    = [p, bcm]{ return p->gpio == bcm; };
                pm.on_highlight = [picker_focus, bcm]{ *picker_focus = bcm; };
                // List every GPIO line not held by hardware. Pins already on
                // another slot stay listed (override allowed) and are flagged red.
                pm.visible_fn   = [bcm, gpio_pin_selectable]{ return gpio_pin_selectable(bcm); };
                pm.warn_fn      = [bcm, i, gpio_other_slot]{ return gpio_other_slot(bcm, i) >= 0; };
                pm.action       = [p, bcm, menu_sys_pp]{
                    p->gpio = bcm;
                    if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
                };
                picker_items.push_back(std::move(pm));
            }
            MenuItem pin_item = with_panel(submenu("Pin", std::move(picker_items)),
                                           "Select GPIO Pin",
                                           draw_pin_picker_panel(picker_focus, i, p));
            pin_item.action   = [picker_focus, p]{ *picker_focus = p->gpio; };  // sync focus on enter
            pin_item.label_fn = [p]{
                return p->gpio < 0 ? std::string("Pin: (unused)")
                                   : std::string("Pin: GPIO ") + std::to_string(p->gpio);
            };
            pin_item.description =
                "Pick which GPIO pin drives this slot (header view shown "
                "alongside). Pins held by hardware (HUB75/SPI/I\xC2\xB2""C…) are "
                "hidden; a pin already used by another slot stays selectable but "
                "is flagged \xE2\x80\x94 overriding it marks both slots red until "
                "you fix the clash. Selecting a pin returns here to set functions.";

            std::vector<MenuItem> pull_items = {
                leaf_sel("Pull Up",   [p]{ p->pull = 1; }, [p]{ return p->pull == 1; }),
                leaf_sel("Pull Down", [p]{ p->pull = 2; }, [p]{ return p->pull == 2; }),
                leaf_sel("None",      [p]{ p->pull = 0; }, [p]{ return p->pull == 0; }),
            };
            std::vector<MenuItem> items = {
                pin_item,
                submenu("Short Press", fn_picker(&p->short_fn)),
                submenu("Long Press",  fn_picker(&p->long_fn)),
                slider("Long-press Time", 200.f, 2000.f, 50.f, " ms",
                    [p]{ return static_cast<float>(p->long_ms); },
                    [p](float v){ p->long_ms = static_cast<int>(v); }),
                submenu("Pull", std::move(pull_items)),
                toggle("Active Low",
                    [p]{ return p->active_low; },
                    [p](bool v){ p->active_low = v; }),
            };
            MenuItem m = submenu(std::string("Slot ") + std::to_string(i + 1), std::move(items));
            m.label_fn = [p, i, gpio_slot_conflict]{
                std::string s = "Slot " + std::to_string(i + 1) + "  ";
                if (p->gpio < 0) return s + "(unused)";
                if (gpio_slot_conflict(i)) s = "! " + s;   // leading marker + red (warn_fn)
                s += "GPIO" + std::to_string(p->gpio) + " \xE2\x86\x92 ";
                s += input::gpio_func_name(p->short_fn);
                return s;
            };
            // Paint the row red when this slot's pin collides with another slot
            // or a hardware peripheral, so the user knows to go back and fix it.
            m.warn_fn = [i, gpio_slot_conflict]{ return gpio_slot_conflict(i); };
            gpio_btn_menu.push_back(std::move(m));
        }

        gpio_buttons_item = with_desc(
            submenu("GPIO Buttons", std::move(gpio_btn_menu)),
            "Assign functions to GPIO switches. Each slot's Pin page shows the "
            "40-pin header and offers only the pins free of other peripherals; "
            "pick one to set its Short-press and optional Long-press function, "
            "pull bias, and polarity (Active Low = switch wired to GND with a "
            "pull-up). Toggling Enabled applies instantly; after editing slots "
            "choose \"Apply Changes Now\" to reload the poller. Saved to "
            "cfg[\"gpio\"][\"pins\"].");
    } else {
        gpio_buttons_item.visible_fn = []{ return false; };
    }

    // ── Pi Settings helpers ──────────────────────────────────────────────────
    // Most of these shell out to systemd/raspi tools. The ones that mutate
    // system state (hostname, timezone, NTP, apt, power) need sudo; ship
    // /etc/sudoers.d/protohud (chmod 440) with:
    //
    //   <user> ALL=(ALL) NOPASSWD: /usr/bin/hostnamectl, /usr/bin/timedatectl, \
    //       /usr/bin/apt-get, /usr/sbin/poweroff, /usr/sbin/reboot, \
    //       /usr/bin/systemctl restart protohud.service
    //
    // …so the menu calls succeed without an interactive password prompt.
    auto run_sh = [push_notif](std::string title, std::string cmd) {
        std::thread([push_notif, title=std::move(title), cmd=std::move(cmd)]() {
            const std::string full = cmd + " 2>&1 | logger -t protohud";
            const int rc = std::system(full.c_str());
            push_notif(NotifType::App, title,
                       rc == 0 ? std::string("OK")
                               : ("Failed (rc " + std::to_string(rc) + ")"),
                       5.f);
        }).detach();
    };
    // Capture popen output synchronously (small reads only — used for
    // hostname, current timezone, df output).
    auto sh_read = [](const char* cmd) -> std::string {
        std::string out;
        FILE* f = ::popen(cmd, "r");
        if (!f) return out;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) out += buf;
        ::pclose(f);
        // Trim trailing whitespace/newlines.
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    };
    // get_state / get_toggle / context-panel hooks run for every visible row
    // every frame — raw sh_read() in one of those forks a process per row per
    // draw (the Timezone list alone was 11 forks/frame while open). A short
    // TTL keeps the shown values honest without the fork storm. Menu hooks
    // only run on the render thread, so the cache needs no lock.
    auto sh_read_cached = [sh_read](const char* cmd, double ttl_s) -> std::string {
        struct Entry { double t = -1e9; std::string out; };
        static std::map<std::string, Entry> cache;
        Entry& e = cache[cmd];
        const double now = glfwGetTime();
        if (now - e.t >= ttl_s) { e.out = sh_read(cmd); e.t = now; }
        return e.out;
    };

    // ── Pi Settings submenu ──────────────────────────────────────────────────
    std::vector<MenuItem> tz_items;
    {
        // A small curated set of common zones plus UTC. The user can add
        // more by hand-editing /etc/timezone; this list is just menu polish.
        static const char* kZones[] = {
            "UTC",
            "America/Los_Angeles", "America/Denver",
            "America/Chicago",     "America/New_York",
            "Europe/London",       "Europe/Berlin",
            "Europe/Paris",        "Asia/Tokyo",
            "Asia/Shanghai",       "Australia/Sydney",
        };
        for (const char* z : kZones) {
            tz_items.push_back(leaf_sel(z,
                [run_sh, z]{
                    run_sh(std::string("Timezone → ") + z,
                           std::string("sudo timedatectl set-timezone ") + z);
                },
                [sh_read_cached, z]{
                    return sh_read_cached("timedatectl show -p Timezone --value", 2.0) == z;
                }));
        }
    }
    std::vector<MenuItem> pi_settings_items = {
        // Hostname — current value shown in the context panel; descend to change it.
        with_panel(
            submenu("Hostname", std::vector<MenuItem>{
                with_desc(leaf("Set Hostname\xE2\x80\xA6",
                    [menu_sys_pp, run_sh]{
                        if (!menu_sys_pp || !*menu_sys_pp) return;
                        char host[256] = {0};
                        if (::gethostname(host, sizeof(host) - 1) != 0) host[0] = '\0';
                        (*menu_sys_pp)->open_keyboard("Hostname", host,
                            [run_sh](const std::string& nm){
                                if (nm.empty()) return;
                                std::string safe;   // keep only hostname-legal chars
                                for (char c : nm)
                                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                        || (c >= '0' && c <= '9') || c == '-' || c == '.')
                                        safe.push_back(c);
                                if (safe.empty()) return;
                                run_sh("Hostname", "sudo hostnamectl set-hostname " + safe);
                            });
                    }),
                    "Change the Pi's hostname (hostnamectl). Applies to new logins."),
            }),
            "Hostname",
            [](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                (void)sz;
                char host[256] = {0};
                const char* hn = (::gethostname(host, sizeof(host) - 1) == 0 && host[0])
                                   ? host : "(unknown)";
                ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
                dl->AddText(font, fs * 0.95f, {o.x, o.y},
                            IM_COL32(150, 160, 170, 210), "Current hostname");
                dl->AddText(font, fs * 1.4f, {o.x, o.y + fs * 1.4f},
                            IM_COL32(225, 235, 245, 240), hn);
            }),
        // Time — timezone, automatic sync, and clock display settings together.
        with_desc(submenu("Time", std::vector<MenuItem>{
            with_desc(submenu("Timezone", std::move(tz_items)),
                "System timezone (timedatectl). Affects the clock, log timestamps "
                "and scheduled tasks."),
            with_desc(toggle("Auto Time Sync (NTP)",
                [sh_read_cached]{ return sh_read_cached("timedatectl show -p NTP --value", 2.0) == "yes"; },
                [run_sh](bool v){
                    run_sh(v ? "NTP On" : "NTP Off",
                           std::string("sudo timedatectl set-ntp ") + (v ? "true" : "false"));
                }),
                "Automatic time sync via systemd-timesyncd. Off to set time manually."),
            submenu("Clock", std::vector<MenuItem>{
                toggle("24-Hour",   [&state]{ return state.clock_cfg.use_24h; },
                                    [&state](bool v){ state.clock_cfg.use_24h = v; }),
                toggle("Seconds",   [&state]{ return state.clock_cfg.show_seconds; },
                                    [&state](bool v){ state.clock_cfg.show_seconds = v; }),
                toggle("Show Date", [&state]{ return state.clock_cfg.show_date; },
                                    [&state](bool v){ state.clock_cfg.show_date = v; }),
                slider("Font Size", 1.f, 3.f, 0.25f, "x",
                    [&state]{ return state.clock_cfg.font_scale; },
                    [&state](float v){ state.clock_cfg.font_scale = v; }),
            }),
        }), "Timezone, automatic NTP sync, and clock display settings."),
        with_panel(
            // Storage usage context panel — df refreshes every few seconds,
            // not once per draw.
            submenu("Storage", std::vector<MenuItem>{}),
            "Disk Usage",
            [sh_read_cached](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                dl->AddText(font, fs * 1.0f, {o.x, o.y},
                            IM_COL32(230, 235, 240, 255), "Disk Usage");
                const std::string out = sh_read_cached("df -h /", 5.0);
                float y = o.y + fs * 1.5f;
                size_t pos = 0;
                while (pos < out.size()) {
                    size_t nl = out.find('\n', pos);
                    if (nl == std::string::npos) nl = out.size();
                    const std::string line = out.substr(pos, nl - pos);
                    dl->AddText(font, fs * 0.85f, {o.x, y},
                                IM_COL32(200, 210, 220, 220), line.c_str());
                    y += fs;
                    pos = nl + 1;
                }
            }),
        // (Restart ProtoHUD + Shutdown moved to System > Power; the apt updater
        //  moved to System > Software. GPIO visualizer/buttons + cooling fans are
        //  appended to this list further below.)
    };

    // ── Updates submenu (in-HUD updater — Phase 1, user-initiated only) ───────
    // Pings the repo over plain git (no GitHub API): lists remote branches,
    // shows their change-notes/log in the context panel, and applies a chosen
    // branch via scripts/update.sh <branch> --restart. A pre-update rollback
    // point is recorded by update.sh; "Rollback" returns to it via
    // scripts/rollback.sh (also runnable standalone over SSH if the HUD won't
    // boot). POLICY: there is deliberately NO automatic update path here —
    // nothing fetches, builds or restarts unless the user picks it.
    struct UpdaterState {
        std::mutex  mtx;
        std::string root;          // repo root (resolved from /proc/self/exe)
        std::string cur_short;     // current HEAD short hash
        std::string cur_subject;   // current HEAD commit subject
        std::string cur_branch;    // current branch name
        std::vector<std::string> all_remote; // every remote branch (authoritative)
        std::vector<std::string> branches;  // filtered view shown in the menu
        bool        show_all = false;       // curated (main + claude/*) vs all
        int         behind   = -1;          // commits behind origin/<branch>; -1 unknown
        std::string last_check;             // human time of last fetch; "" = never
        std::string status   = "Not checked yet.";
        bool        busy     = false;       // an async git/update op is running
        int         sel      = -1;          // highlighted branch index (log panel)
        std::string sel_name;               // highlighted branch name
        std::string sel_log;                // cached git log for the highlighted branch
        bool        rollback_avail = false; // state/update/last_good.env exists
        std::string rollback_target;        // "hash on branch" for the label
        // Live progress while an update is applying. Fed by a monitor thread that
        // tails /tmp/protohud-update.log (the detached update.sh writes there).
        bool        updating   = false;     // an apply/build is in flight
        std::string phase;                  // "Fetching" / "Building" / "Restarting" …
        int         build_done = 0;         // ninja [done/total]
        int         build_total = 0;
        std::string last_line;              // most recent log line (for detail)
    };
    auto upd = std::make_shared<UpdaterState>();
    {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            std::string p(exe);                                       // .../build/protohud
            auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
            s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
            upd->root = p;
        }
    }

    // Run `git -C <root> <args>` and capture stdout (local queries are fast;
    // network ones — fetch — are only ever called from a worker thread).
    auto git_q = [sh_read, upd](const std::string& args) -> std::string {
        std::string root;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
        if (root.empty()) return {};
        const std::string cmd = "git -C '" + root + "' " + args;
        return sh_read(cmd.c_str());
    };

    // Refresh the current-version fields (local, instant).
    auto upd_refresh_cur = [git_q, upd]{
        std::string sh = git_q("rev-parse --short HEAD");
        std::string su = git_q("log -1 --pretty=%s");
        std::string br = git_q("rev-parse --abbrev-ref HEAD");
        std::lock_guard<std::mutex> lk(upd->mtx);
        upd->cur_short = sh; upd->cur_subject = su; upd->cur_branch = br;
    };

    // Recompute the visible list from the cached full list + curated filter.
    // Instant (no git) — safe to call on the UI thread when toggling the view.
    auto upd_apply_filter = [upd]{
        std::lock_guard<std::mutex> lk(upd->mtx);
        std::vector<std::string> out;
        for (const auto& ref : upd->all_remote) {
            if (!upd->show_all) {
                // Curated view: main/master plus the working claude/* branches.
                const bool keep = (ref == "main" || ref == "master"
                                   || ref.rfind("claude/", 0) == 0);
                if (!keep) continue;
            }
            out.push_back(ref);
        }
        upd->branches = std::move(out);
    };

    // Populate the full remote-branch list, then re-filter. local=true reads
    // local tracking refs (instant, but only branches already fetched);
    // local=false runs `git ls-remote --heads origin` (network — authoritative,
    // lists EVERY branch on the repo even if it was never fetched locally).
    auto upd_load_branches = [git_q, upd, upd_apply_filter](bool local){
        const std::string raw = local
            ? git_q("for-each-ref --format=%(refname:short) --sort=-committerdate refs/remotes/origin")
            : git_q("ls-remote --heads origin");
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t nl = raw.find('\n', pos);
            if (nl == std::string::npos) nl = raw.size();
            std::string line = raw.substr(pos, nl - pos);
            pos = nl + 1;
            std::string ref;
            if (local) {
                ref = line;
                const std::string pfx = "origin/";
                if (ref.rfind(pfx, 0) == 0) ref = ref.substr(pfx.size());
            } else {
                // ls-remote line: "<sha>\trefs/heads/<branch>"
                const auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                ref = line.substr(tab + 1);
                const std::string pfx = "refs/heads/";
                if (ref.rfind(pfx, 0) != 0) continue;
                ref = ref.substr(pfx.size());
            }
            if (ref.empty() || ref == "HEAD" || ref.find("HEAD ->") != std::string::npos) continue;
            out.push_back(ref);
        }
        // Don't clobber a good cached list with an empty network result
        // (offline / fetch failed) — but always accept the local read.
        if (!out.empty() || local) {
            std::lock_guard<std::mutex> lk(upd->mtx);
            upd->all_remote = std::move(out);
        }
        upd_apply_filter();
    };

    // Refresh rollback availability from the marker update.sh writes. Parsed
    // directly (KEY=value lines) — sourcing it through /bin/sh printed
    // "sh: 1: Bad substitution" (${VAR:0:9} is a bashism; the Pi's sh is
    // dash) and "( unexpected" (the values used to be written unquoted, so a
    // date/path with shell metacharacters broke the parser) at every startup.
    auto upd_refresh_rollback = [upd]{
        std::string root;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
        std::string commit, branch;
        std::ifstream f(root + "/state/update/last_good.env");
        std::string line;
        auto strip = [](std::string v) {
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
                v.back() == v.front())
                v = v.substr(1, v.size() - 2);
            return v;
        };
        while (std::getline(f, line)) {
            if      (line.rfind("LAST_GOOD_COMMIT=", 0) == 0) commit = strip(line.substr(17));
            else if (line.rfind("LAST_GOOD_BRANCH=", 0) == 0) branch = strip(line.substr(17));
        }
        if (commit.size() > 9) commit.resize(9);
        std::lock_guard<std::mutex> lk(upd->mtx);
        if (commit.empty()) { upd->rollback_avail = false; upd->rollback_target.clear(); return; }
        upd->rollback_avail  = true;
        upd->rollback_target = branch.empty() ? commit : (commit + " on " + branch);
    };

    // Initial population (local only — never auto-fetches the network).
    upd_refresh_cur();
    upd_load_branches(/*local=*/true);
    upd_refresh_rollback();

    // "Check for Updates": async fetch + behind-count. The ONLY network call,
    // and only when the user picks it.
    auto upd_check = [git_q, upd_refresh_cur, upd_load_branches, sh_read, upd, push_notif]{
        { std::lock_guard<std::mutex> lk(upd->mtx);
          if (upd->busy) return; upd->busy = true; upd->status = "Checking\xe2\x80\xa6"; }
        std::thread([git_q, upd_refresh_cur, upd_load_branches, sh_read, upd, push_notif]{
            git_q("fetch --prune origin");                 // network (retry handled by git config / best-effort)
            upd_refresh_cur();
            upd_load_branches(/*local=*/false);            // authoritative remote list
            std::string br; { std::lock_guard<std::mutex> lk(upd->mtx); br = upd->cur_branch; }
            const std::string beh = git_q("rev-list --count HEAD..origin/" + br);
            int behind = beh.empty() ? -1 : std::atoi(beh.c_str());
            const std::string now = sh_read("date '+%a %H:%M'");
            std::string msg = behind > 0
                ? (std::to_string(behind) + " new commit(s) on " + br)
                : (behind == 0 ? "Up to date." : "Checked (branch not on remote).");
            { std::lock_guard<std::mutex> lk(upd->mtx);
              upd->behind = behind; upd->last_check = now; upd->status = msg; upd->busy = false; }
            push_notif(NotifType::App, "Updates",
                       behind > 0 ? (std::to_string(behind) + " update(s) available")
                                  : "Already up to date", 5.f);
        }).detach();
    };

    // Apply a branch via update.sh <branch> --restart, detached so the build +
    // restart survive ProtoHUD being killed during the restart step.
    auto upd_apply = [upd, push_notif](const std::string& branch){
        std::string root; bool busy, updating;
        { std::lock_guard<std::mutex> lk(upd->mtx);
          root = upd->root; busy = upd->busy; updating = upd->updating; }
        if (busy || updating || root.empty() || branch.empty()) return;
        const std::string sp = root + "/scripts/update.sh";
        // setsid + bash -c '"$0" "$1" --restart' keeps the script in its own
        // session; $0/$1 carry the (quoted) script path + branch so neither
        // needs escaping in the -c string.
        const std::string cmd =
            "setsid bash -c '\"$0\" \"$1\" --restart' '" + sp + "' '" + branch +
            "' </dev/null >/tmp/protohud-update.log 2>&1 &";
        { std::lock_guard<std::mutex> lk(upd->mtx);
          upd->updating = true; upd->phase = "Starting\xe2\x80\xa6";
          upd->build_done = 0; upd->build_total = 0; upd->last_line.clear();
          upd->status = "Update starting\xe2\x80\xa6"; }
        // Clear any stale log first so the monitor can't read a previous run's
        // output (e.g. an old ERROR line) before update.sh truncates it.
        if (FILE* f = ::fopen("/tmp/protohud-update.log", "w")) ::fclose(f);
        std::system(cmd.c_str());
        push_notif(NotifType::App, "Update started",
                   "Pulling " + branch + " and rebuilding. Progress shows under "
                   "Updates \xe2\x86\x92 Update Status; the HUD restarts when the build "
                   "succeeds. Log: /tmp/protohud-update.log", 0.f);

        // Monitor thread: tail the update log and surface phase + build %. The
        // build runs BEFORE the restart, so this HUD is alive to show it; on a
        // successful build the restart kills us and the thread dies with it. On
        // failure update.sh exits without restarting, which we detect here.
        std::thread([upd, push_notif]{
            const char* logp = "/tmp/protohud-update.log";
            for (int i = 0; i < 8000; ++i) {                 // ~60 min safety cap
                { std::lock_guard<std::mutex> lk(upd->mtx); if (!upd->updating) break; }

                std::string tail;
                if (FILE* f = ::fopen(logp, "rb")) {
                    ::fseek(f, 0, SEEK_END);
                    long sz = ::ftell(f);
                    long want = sz > 16384 ? 16384 : (sz < 0 ? 0 : sz);
                    if (want > 0) {
                        ::fseek(f, sz - want, SEEK_SET);
                        tail.resize(static_cast<size_t>(want));
                        size_t rd = ::fread(&tail[0], 1, static_cast<size_t>(want), f);
                        tail.resize(rd);
                    }
                    ::fclose(f);
                }

                std::string phase, last_line;
                int bd = 0, bt = 0;
                bool failed = false;
                size_t pos = 0;
                while (pos < tail.size()) {
                    size_t nl = tail.find('\n', pos);
                    if (nl == std::string::npos) nl = tail.size();
                    std::string ln = tail.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (ln.empty()) continue;
                    last_line = ln;
                    // ninja progress: "[<done>/<total>] …"
                    if (ln[0] == '[') {
                        int d = 0, t = 0;
                        if (std::sscanf(ln.c_str(), "[%d/%d]", &d, &t) == 2 && t > 0) {
                            bd = d; bt = t; phase = "Building";
                        }
                    }
                    // update.sh phase markers (it prefixes "[protohud-update]")
                    if (ln.find("[protohud-update]") != std::string::npos) {
                        if (ln.find("ERROR") != std::string::npos ||
                            ln.find("failed") != std::string::npos)            { phase = "Failed"; failed = true; }
                        else if (ln.find("stopping current") != std::string::npos ||
                                 ln.find("starting updated") != std::string::npos) phase = "Restarting";
                        else if (ln.find("building") != std::string::npos)        phase = "Building";
                        else if (ln.find("updating branch") != std::string::npos ||
                                 ln.find("now at") != std::string::npos)          phase = "Fetching";
                    }
                }

                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (!phase.empty()) upd->phase = phase;
                  if (bt > 0) { upd->build_done = bd; upd->build_total = bt; }
                  if (!last_line.empty()) upd->last_line = last_line;
                  if (bt > 0)
                      upd->status = "Building " + std::to_string(bd * 100 / bt) + "%  ("
                                  + std::to_string(bd) + "/" + std::to_string(bt) + ")";
                  else if (!phase.empty())
                      upd->status = phase + "\xe2\x80\xa6";
                }

                if (failed) {
                    { std::lock_guard<std::mutex> lk(upd->mtx); upd->updating = false; }
                    push_notif(NotifType::App, "Update failed",
                               "Build failed \xe2\x80\x94 staying on the current build. "
                               "See /tmp/protohud-update.log", 8.f);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(450));
            }
        }).detach();
    };

    // Roll back to the recorded known-good build via the standalone script.
    auto upd_rollback = [upd, push_notif]{
        std::string root; bool busy, avail;
        { std::lock_guard<std::mutex> lk(upd->mtx);
          root = upd->root; busy = upd->busy; avail = upd->rollback_avail; }
        if (busy || root.empty() || !avail) return;
        const std::string sp = root + "/scripts/rollback.sh";
        const std::string cmd =
            "setsid bash -c '\"$0\" --restart' '" + sp +
            "' </dev/null >/tmp/protohud-rollback.log 2>&1 &";
        std::system(cmd.c_str());
        push_notif(NotifType::App, "Rollback started",
                   "Restoring the last known-good build + config, then restarting. "
                   "Log: /tmp/protohud-rollback.log", 0.f);
    };

    std::vector<MenuItem> updates_menu;
    {
        // Current version + status panel.
        MenuItem cur = leaf("Current Version", []{});
        cur.label_fn = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            std::string s = "On " + (upd->cur_branch.empty() ? "?" : upd->cur_branch)
                          + " @ " + (upd->cur_short.empty() ? "?" : upd->cur_short);
            if (upd->behind > 0) s += "  (" + std::to_string(upd->behind) + " behind)";
            return s;
        };
        updates_menu.push_back(with_panel(std::move(cur), "Update Status",
            [upd](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                std::lock_guard<std::mutex> lk(upd->mtx);
                float y = o.y;
                auto line = [&](const std::string& t, ImU32 col, float scale){
                    dl->AddText(font, fs * scale, {o.x, y}, col, t.c_str());
                    y += fs * (scale + 0.25f);
                };
                line("Current build", IM_COL32(230, 235, 240, 255), 1.0f);
                line(upd->cur_branch.empty() ? "branch ?" : upd->cur_branch,
                     IM_COL32(200, 210, 220, 220), 0.85f);
                line((upd->cur_short.empty() ? "?" : upd->cur_short) + "  " + upd->cur_subject,
                     IM_COL32(180, 190, 200, 210), 0.85f);
                y += fs * 0.4f;
                line(upd->last_check.empty() ? "Last checked: never"
                                             : ("Last checked: " + upd->last_check),
                     IM_COL32(200, 210, 220, 220), 0.85f);
                line(upd->status, upd->behind > 0 ? IM_COL32(120, 220, 140, 235)
                                                  : IM_COL32(200, 210, 220, 220), 0.85f);

                // ── Live update progress ─────────────────────────────────────
                if (upd->updating) {
                    y += fs * 0.5f;
                    line(upd->phase.empty() ? "Working\xe2\x80\xa6" : upd->phase,
                         IM_COL32(120, 200, 255, 255), 0.95f);
                    if (upd->build_total > 0) {
                        float frac = static_cast<float>(upd->build_done) /
                                     static_cast<float>(upd->build_total);
                        if (frac < 0.f) frac = 0.f; if (frac > 1.f) frac = 1.f;
                        const float barw = (sz.x > 16.f ? sz.x - 8.f : 220.f);
                        const float bh   = fs * 0.8f;
                        const float bx = o.x, by = y;
                        dl->AddRect({bx, by}, {bx + barw, by + bh},
                                    IM_COL32(120, 130, 140, 200), 2.f);
                        dl->AddRectFilled({bx + 1, by + 1},
                                          {bx + 1 + (barw - 2) * frac, by + bh - 1},
                                          IM_COL32(90, 180, 120, 235), 2.f);
                        y += bh + fs * 0.35f;
                        char pc[40];
                        snprintf(pc, sizeof(pc), "%d%%   (%d/%d)",
                                 static_cast<int>(frac * 100.f),
                                 upd->build_done, upd->build_total);
                        line(pc, IM_COL32(210, 230, 240, 235), 0.8f);
                    }
                    if (!upd->last_line.empty()) {
                        std::string ll = upd->last_line;
                        if (ll.size() > 46) ll = ll.substr(0, 46) + "\xe2\x80\xa6";
                        line(ll, IM_COL32(150, 160, 170, 200), 0.72f);
                    }
                }
            }));

        // Check for Updates (the only network action).
        MenuItem chk = with_desc(leaf("Check for Updates", [upd_check]{ upd_check(); }),
            "Fetch the latest commit list from the repo (git fetch) and report how "
            "far behind the current branch is. Does NOT change any code \xe2\x80\x94 nothing "
            "updates until you pick a branch below. This is the only step that uses "
            "the network.");
        chk.label_fn = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            return upd->busy ? std::string("Checking\xe2\x80\xa6")
                             : std::string("Check for Updates");
        };
        updates_menu.push_back(std::move(chk));

        // Update current branch in place.
        updates_menu.push_back(with_desc(
            leaf("Update This Branch & Restart",
                 [upd, upd_apply]{
                     std::string br; { std::lock_guard<std::mutex> lk(upd->mtx); br = upd->cur_branch; }
                     upd_apply(br);
                 }),
            "Pull the latest commits for the CURRENT branch, rebuild, and restart. "
            "Your settings (config.json) and custom faces/effects are preserved \xe2\x80\x94 "
            "they're outside version control. A rollback point is saved first."));

        // Select Branch submenu — dynamic slots fed by upd->branches, with a
        // level context panel showing the highlighted branch's change log.
        std::vector<MenuItem> branch_items;
        branch_items.push_back(with_desc(toggle("Show All Branches",
            [upd]{ std::lock_guard<std::mutex> lk(upd->mtx); return upd->show_all; },
            [upd, upd_apply_filter, upd_load_branches](bool v){
                { std::lock_guard<std::mutex> lk(upd->mtx); upd->show_all = v; }
                upd_apply_filter();   // instant re-filter of the cached list
                // …then refresh the authoritative list from the repo so branches
                // that were never fetched locally also appear (network).
                std::thread([upd_load_branches]{ upd_load_branches(/*local=*/false); }).detach();
            }),
            "OFF: show only main + the claude/* working branches (recommended). "
            "ON: list every branch on the repo (refreshed over the network)."));
        branch_items.push_back(with_desc(
            leaf("Refresh List", [upd_load_branches]{
                std::thread([upd_load_branches]{ upd_load_branches(/*local=*/false); }).detach();
            }),
            "Re-query the repo for its full branch list (git ls-remote). Shows "
            "every branch, including ones not yet downloaded to this Pi."));

        static constexpr int kBranchSlots = 48;
        for (int i = 0; i < kBranchSlots; ++i) {
            MenuItem b = leaf("branch", []{});
            b.visible_fn = [upd, i]{
                std::lock_guard<std::mutex> lk(upd->mtx);
                return i < static_cast<int>(upd->branches.size());
            };
            b.label_fn = [upd, i]{
                std::lock_guard<std::mutex> lk(upd->mtx);
                if (i >= static_cast<int>(upd->branches.size())) return std::string();
                std::string nm = upd->branches[i];
                if (nm == upd->cur_branch) nm += "  (current)";
                return nm;
            };
            // Load the change-log into the panel cache when highlighted.
            b.on_highlight = [upd, git_q, i]{
                std::string nm;
                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (i >= static_cast<int>(upd->branches.size())) return;
                  if (upd->sel == i) return;          // already cached
                  nm = upd->branches[i]; upd->sel = i; upd->sel_name = nm;
                  upd->sel_log = "Loading\xe2\x80\xa6"; }
                // Richer changelog: short hash + author-date + subject.
                std::string log = git_q(
                    "log --pretty=format:'%h %ad  %s' --date=short -n 20 origin/" + nm);
                std::string ahead  = git_q("rev-list --count HEAD..origin/" + nm);
                std::string behind = git_q("rev-list --count origin/" + nm + "..HEAD");
                std::lock_guard<std::mutex> lk(upd->mtx);
                if (upd->sel != i) return;            // moved on while loading
                std::string hdr;
                if (!ahead.empty() && ahead != "0")  hdr += "+" + ahead + " new commit(s) to apply";
                if (!behind.empty() && behind != "0") hdr += (hdr.empty() ? "" : "  ") +
                                                             ("-" + behind + " not on this branch");
                if (hdr.empty() && !ahead.empty())   hdr = "Even with your current build";
                upd->sel_log = (hdr.empty() ? "" : (hdr + "\n\n"))
                             + (log.empty() ? "(no log \xe2\x80\x94 run Check for Updates to fetch it)" : log);
            };
            b.action = [upd, upd_apply, i]{
                std::string nm;
                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (i >= static_cast<int>(upd->branches.size())) return;
                  nm = upd->branches[i]; }
                upd_apply(nm);
            };
            b.description = "Select to update to this branch, rebuild, and restart. "
                            "The panel shows its recent commits. Settings + custom "
                            "content are preserved; a rollback point is saved first.";
            branch_items.push_back(std::move(b));
        }

        MenuItem branch_sub = submenu("Select Branch", std::move(branch_items));
        updates_menu.push_back(with_desc(
            with_panel(std::move(branch_sub), "Branch Changes",
                [upd](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                    (void)sz;
                    ImFont* font = ImGui::GetFont();
                    const float fs = ImGui::GetFontSize();
                    std::lock_guard<std::mutex> lk(upd->mtx);
                    dl->AddText(font, fs * 1.0f, {o.x, o.y}, IM_COL32(230, 235, 240, 255),
                                upd->sel_name.empty() ? "Highlight a branch"
                                                      : upd->sel_name.c_str());
                    float y = o.y + fs * 1.5f;
                    const std::string& t = upd->sel_log;
                    size_t pos = 0;
                    while (pos < t.size()) {
                        size_t nl = t.find('\n', pos);
                        if (nl == std::string::npos) nl = t.size();
                        const std::string ln = t.substr(pos, nl - pos);
                        dl->AddText(font, fs * 0.8f, {o.x, y},
                                    IM_COL32(200, 210, 220, 220), ln.c_str());
                        y += fs * 0.95f;
                        pos = nl + 1;
                    }
                }),
            "Choose a branch to update to. Highlight one to read its recent commits "
            "in the panel, then select to apply (rebuild + restart)."));

        // Rollback — visible only when a recorded known-good point exists.
        MenuItem rb = with_desc(leaf("Rollback Last Update",
            [upd_rollback]{ upd_rollback(); }),
            "Restore the build + config saved just before your last update, then "
            "rebuild and restart. If the HUD won't boot, run scripts/rollback.sh "
            "over SSH to do the same from outside ProtoHUD.");
        rb.visible_fn = [upd]{ std::lock_guard<std::mutex> lk(upd->mtx); return upd->rollback_avail; };
        rb.label_fn   = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            return upd->rollback_target.empty()
                ? std::string("Rollback Last Update")
                : ("Rollback to " + upd->rollback_target);
        };
        updates_menu.push_back(std::move(rb));

        // Update History — reads state/update/history.log (written by update.sh),
        // newest first. Each line: ISO-date \t branch \t before..after \t subject.
        MenuItem hist = leaf("Update History", []{});
        hist.type = MenuItemType::SUBMENU;   // submenu so its context panel shows
        updates_menu.push_back(with_panel(std::move(hist), "Update History",
            [upd, sh_read](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                std::string root;
                { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
                const std::string log = root.empty() ? std::string() : sh_read(
                    ("f='" + root + "/state/update/history.log'; "
                     "[ -f \"$f\" ] && tac \"$f\" | head -n 18").c_str());
                dl->AddText(font, fs * 1.0f, {o.x, o.y}, IM_COL32(230, 235, 240, 255),
                            "Recent updates (newest first)");
                float y = o.y + fs * 1.6f;
                if (log.empty()) {
                    dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(180, 190, 200, 210),
                                "No updates recorded yet.");
                    return;
                }
                size_t pos = 0;
                while (pos < log.size()) {
                    size_t nl = log.find('\n', pos);
                    if (nl == std::string::npos) nl = log.size();
                    std::string ln = log.substr(pos, nl - pos);
                    pos = nl + 1;
                    // Split the tab-separated record into a compact two-line entry.
                    std::vector<std::string> f; size_t p = 0;
                    while (true) { size_t t = ln.find('\t', p);
                        f.push_back(ln.substr(p, t == std::string::npos ? std::string::npos : t - p));
                        if (t == std::string::npos) break; p = t + 1; }
                    std::string when = f.size() > 0 ? f[0] : ln;
                    if (when.size() >= 16) when = when.substr(0, 16).replace(10, 1, " "); // YYYY-MM-DD HH:MM
                    const std::string br  = f.size() > 1 ? f[1] : "";
                    const std::string rng = f.size() > 2 ? f[2] : "";
                    const std::string sub = f.size() > 3 ? f[3] : "";
                    dl->AddText(font, fs * 0.82f, {o.x, y}, IM_COL32(210, 220, 230, 230),
                                (when + "   " + br + "  " + rng).c_str());
                    y += fs * 0.9f;
                    if (!sub.empty()) {
                        dl->AddText(font, fs * 0.78f, {o.x + fs * 0.8f, y},
                                    IM_COL32(170, 180, 190, 200), sub.c_str());
                        y += fs * 0.95f;
                    }
                    y += fs * 0.15f;
                }
            }));

        // Settings & Data Safety — a static explainer of what survives an update.
        MenuItem safe = leaf("Settings & Data Safety", []{});
        safe.type = MenuItemType::SUBMENU;
        updates_menu.push_back(with_panel(std::move(safe), "What's Preserved",
            [](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                static const char* kLines[] = {
                    "Updates never touch your data:",
                    "",
                    "\xe2\x80\xa2 config.json (all menu settings) is outside",
                    "  version control \xe2\x80\x94 git can't overwrite it.",
                    "\xe2\x80\xa2 New defaults from a release are merged IN;",
                    "  your existing values always win.",
                    "\xe2\x80\xa2 Custom faces/effects + IMU calibration",
                    "  live in config.json / Protoface and persist.",
                    "\xe2\x80\xa2 Protoface (imported faces) updates without",
                    "  deleting anything you imported.",
                    "",
                    "Before each update a rollback point is saved",
                    "(commit + a config backup under state/update/).",
                    "Recover anytime via Rollback, or over SSH with",
                    "scripts/rollback.sh --restart.",
                };
                float y = o.y;
                for (const char* ln : kLines) {
                    const bool head = (ln[0] != '\0' && ln[0] != ' ' && ln[0] != '\xe2');
                    dl->AddText(font, fs * (head ? 0.92f : 0.82f), {o.x, y},
                                head ? IM_COL32(230, 235, 240, 255)
                                     : IM_COL32(195, 205, 215, 220), ln);
                    y += fs * 0.95f;
                }
            }));
    }

    // ── Power submenu (existing reboot/quit + new shutdown moved here) ──────
    // Confirm wrapper: a destructive action becomes a submenu the user has to
    // descend into and pick "Confirm …" before it fires — guards against an
    // accidental select on Reboot Pi / Shutdown.
    auto confirm_action = [&](const char* label, const char* warn,
                              std::function<void()> act) -> MenuItem {
        std::vector<MenuItem> kids;
        kids.push_back(with_desc(leaf(std::string("Confirm: ") + label, std::move(act)), warn));
        MenuItem m = submenu(label, std::move(kids));
        m.description = warn;
        return m;
    };
    // Relaunch ProtoHUD via scripts/restart.sh (resolved from /proc/self/exe),
    // detached in its own session so it survives this process being killed.
    auto restart_protohud = [] {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return;
        exe[n] = '\0';
        std::string p(exe);
        auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
        s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
        const std::string script = p + "/scripts/restart.sh";
        std::system(("setsid \"" + script + "\" >/tmp/protohud.log 2>&1 </dev/null &").c_str());
    };
    std::vector<MenuItem> power_menu = {
        with_desc(leaf("Reboot ProtoHUD", restart_protohud),
            "Relaunch ProtoHUD (stop + start via scripts/restart.sh). The display "
            "blanks briefly while it comes back."),
        with_desc(leaf("Reboot ProtoFace", [teensy]{ if (teensy) teensy->restart(); }),
            "Restart the face backend (Protoface daemon / native renderer) without "
            "touching the rest of the HUD."),
        confirm_action("Reboot Pi",
            "Reboots the whole Raspberry Pi after a short delay. Unsaved work is lost.",
            [&state]{
                state.quit = true;
                std::thread([]{
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::system("sudo -n reboot 2>/dev/null || reboot 2>/dev/null &");
                }).detach();
            }),
        confirm_action("Shutdown",
            "Powers off the Raspberry Pi. You'll need to cycle power to start it again.",
            [&state]{
                state.quit = true;
                std::thread([]{
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::system("sudo -n poweroff 2>/dev/null || poweroff 2>/dev/null &");
                }).detach();
            }),
        with_desc(leaf("Close Program", [&state]{ state.quit = true; }),
            "Exit ProtoHUD without rebooting the system."),
        with_desc(toggle("Skip Startup Screen",
            [&state]{ return state.skip_landing; },
            [&state](bool v){ state.skip_landing = v; }),
            "Bypass the profile/continue landing screen at boot and run the current "
            "config directly. Takes effect on the next launch."),
    };

    // ── Display & HUD submenu (cosmetic / per-window controls) ───────────────
    std::vector<MenuItem> display_hud_menu = {
        with_desc(submenu("HUD / Menu Presets", std::move(hud_presets_menu)),
                  "Visual presets: built-in themes plus your own saved HUD color + "
                  "menu style combinations. Applied live."),
        with_desc(toggle("Radial Quick Menu",
            [menu_sys_pp]{ return menu_sys_pp && *menu_sys_pp
                              && (*menu_sys_pp)->quick_style() == QuickStyle::Radial; },
            [menu_sys_pp](bool v){ if (menu_sys_pp && *menu_sys_pp)
                (*menu_sys_pp)->set_quick_style(v ? QuickStyle::Radial : QuickStyle::List); }),
            "ON: the quick menu is a radial wheel encircling the minimap. OFF: the "
            "legacy compact corner list. The minimap's position (HUD > Map) sets where "
            "the wheel sits."),
        with_desc(slider("Menu Tilt", 0.f, 0.8f, 0.05f, "",
            [menu_sys_pp]{ return menu_sys_pp && *menu_sys_pp ? (*menu_sys_pp)->radial_tilt() : 0.f; },
            [menu_sys_pp](float v){ if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->set_radial_tilt(v); }),
            "Helmet-style inward perspective tilt for the radial menu. 0 = flat on "
            "the glass; higher = the wheel curves away at the top like a visor."),
        with_desc(slider("Text Size", 0.7f, 2.0f, 0.1f, "x",
            [hud_cfg]{ return hud_cfg->text_scale; },
            [hud_cfg](float v){ hud_cfg->text_scale = v; }),
            "Scale factor applied to all HUD text. Lower = more on-screen at once."),
        // Window mode (Fullscreen/Frameless) + Resolution moved to System > Display.
        // Legacy HUD toggle moved to HUD > Legacy HUD (grouped with its settings).
    };

    // ── Display submenu (window mode + windowed resolution) ───────────────────
    // Desktop/dev only — ignored while the glasses drive the window.
    std::vector<MenuItem> resolution_menu;
    {
        static const struct { const char* l; int w, h; } kRes[] = {
            {"1280 x 720",  1280,  720}, {"1600 x 900",  1600,  900},
            {"1920 x 1080", 1920, 1080}, {"2560 x 1440", 2560, 1440},
            {"3840 x 1080 (SBS)", 3840, 1080}, {"3840 x 2160", 3840, 2160},
        };
        for (const auto& r : kRes) {
            const int w = r.w, h = r.h;
            resolution_menu.push_back(leaf(r.l, [&state, w, h]{
                state.win_resize_w.store(w); state.win_resize_h.store(h);
                state.win_resize_dirty.store(true);
            }));
        }
    }
    std::vector<MenuItem> display_menu = {
        with_desc(toggle("Fullscreen",
            [&state]{ return state.win_fullscreen.load(); },
            [&state](bool v){ state.win_fullscreen.store(v); state.win_mode_dirty.store(true); }),
            "Borderless fullscreen covering the whole screen. Desktop/dev only \xe2\x80\x94 "
            "ignored while the glasses are connected. Applied live."),
        with_desc(toggle("Frameless Window",
            [&state]{ return state.win_frameless.load(); },
            [&state](bool v){ state.win_frameless.store(v); state.win_mode_dirty.store(true); }),
            "Remove the OS window title bar and borders (windowed mode). Desktop/dev "
            "only. Applied live."),
        with_desc(submenu("Resolution", std::move(resolution_menu)),
            "Resize the windowed (non-fullscreen) output. Desktop/dev only; ignored "
            "while the glasses drive the display."),
    };

    // ── Connectivity submenu ─────────────────────────────────────────────────
    // ── Bluetooth controls (bluetoothctl shell-out) ──────────────────────────
    // Scan/power + per-device connect/pair/trust/remove. Devices come from
    // BtMonitor (state.bt_devices), which now also lists discovered-but-unpaired
    // devices after a scan so they can be paired here.
    auto bt_cmd = [&state, bt_mon](int i, const char* verb) {
        std::string mac;
        { std::lock_guard<std::mutex> lk(state.mtx);
          if (i >= 0 && i < (int)state.bt_devices.size()) mac = state.bt_devices[i].mac; }
        if (mac.empty()) return;
        std::system((std::string("bluetoothctl ") + verb + " " + mac +
                     " >/dev/null 2>&1 &").c_str());
        if (bt_mon) bt_mon->refresh();
    };
    std::vector<MenuItem> bluetooth_menu;
    bluetooth_menu.push_back(with_desc(leaf("Scan for Devices", [bt_mon]{
        std::system("bluetoothctl --timeout 20 scan on >/dev/null 2>&1 &");
        if (bt_mon) bt_mon->refresh();
    }), "Discover nearby devices for ~20s; new ones appear in the list below to pair."));
    bluetooth_menu.push_back(with_desc(leaf("Power On", [bt_mon]{
        std::system("bluetoothctl power on >/dev/null 2>&1 &"); if (bt_mon) bt_mon->refresh(); }),
        "Turn the Bluetooth adapter on."));
    bluetooth_menu.push_back(with_desc(leaf("Power Off", [bt_mon]{
        std::system("bluetoothctl power off >/dev/null 2>&1 &"); if (bt_mon) bt_mon->refresh(); }),
        "Turn the Bluetooth adapter off."));
    bluetooth_menu.push_back(with_desc(leaf("Refresh", [bt_mon]{ if (bt_mon) bt_mon->refresh(); }),
        "Re-scan known devices and update the list now."));
    for (int i = 0; i < 12; ++i) {
        MenuItem dev; dev.type = MenuItemType::SUBMENU; dev.label = "btdev";
        dev.label_fn = [&state, i]{
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i >= (int)state.bt_devices.size()) return std::string();
            const auto& d = state.bt_devices[i];
            return d.name + (d.connected ? "  \xE2\x9C\x93" : d.paired ? "  (paired)" : "  (new)");
        };
        dev.visible_fn = [&state, i]{
            std::lock_guard<std::mutex> lk(state.mtx); return i < (int)state.bt_devices.size(); };
        dev.children = {
            with_desc(leaf("Connect",    [bt_cmd, i]{ bt_cmd(i, "connect"); }),
                "Connect to this device."),
            with_desc(leaf("Disconnect", [bt_cmd, i]{ bt_cmd(i, "disconnect"); }),
                "Disconnect this device."),
            with_desc(leaf("Pair",       [bt_cmd, i]{ bt_cmd(i, "pair"); }),
                "Pair with this device (confirm any prompt on both ends)."),
            with_desc(leaf("Trust",      [bt_cmd, i]{ bt_cmd(i, "trust"); }),
                "Mark trusted so it can auto-reconnect."),
            with_desc(leaf("Remove",     [bt_cmd, i]{ bt_cmd(i, "remove"); }),
                "Unpair and forget this device."),
        };
        bluetooth_menu.push_back(std::move(dev));
    }

    std::vector<MenuItem> connectivity_menu = {
        with_desc(toggle("SSH Access",
            [state_ptr]{ return state_ptr && state_ptr->ssh.active; },
            [state_ptr](bool v){
                system(v ? "systemctl start ssh 2>&1 | logger -t protohud &"
                         : "systemctl stop ssh 2>&1 | logger -t protohud &");
                if (state_ptr) {
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->ssh.active = v;
                }
            }),
            "Toggle the sshd service. ON exposes the Pi to network logins on TCP 22."),
        with_desc(submenu("Bluetooth", std::move(bluetooth_menu)),
                  "Scan, pair, connect and manage Bluetooth devices."),
    };

    // ── Diagnostics submenu (probes + overlays + per-tab tools) ──────────────
    std::vector<MenuItem> diagnostics_group_menu = {
        with_desc(menu_shared::system_panel_toggle(sys_panel_active),
            "Live system overlay (CPU, RAM, GPU temp, network ping)."),
        with_desc(menu_shared::fps_overlay_toggle(fps_overlay_active),
            "Show the current frame rate as an overlay."),
        with_desc(submenu("FPS Average",   std::move(fps_interval_menu)),
                  "Averaging window for the FPS readout."),
        with_desc(submenu("Diagnostics",   std::move(diagnostics_menu)),
                  "Camera / network / Bluetooth / GPIO probes."),
        with_desc(leaf("Request Status", [teensy]{ teensy->request_status(); }),
                  "Poll the face controller for a fresh status frame."),
    };
    // The GPIO visualizer + on-board button map now live in the top-level "GPIO"
    // tab (On-Board GPIO submenu). When build_menu wires gpio_onboard_out, route
    // them there; otherwise keep the legacy Pi Settings placement.
    if (ctx.gpio_onboard_out) {
        ctx.gpio_onboard_out->push_back(std::move(gpio_viz_item));
        ctx.gpio_onboard_out->push_back(std::move(gpio_buttons_item));
    } else {
        pi_settings_items.push_back(std::move(gpio_viz_item));
        pi_settings_items.push_back(std::move(gpio_buttons_item));
    }
    // Cooling fans (Pi-driven PWM). Hidden when no FanController is wired.
    pi_settings_items.push_back([&]() -> MenuItem {
            if (!fans || fans->zone_count() == 0) {
                MenuItem m = leaf("Cooling Fans", []{}); m.visible_fn = []{ return false; }; return m;
            }
            // Per-zone speed/mode/curve submenu.
            auto build_zone = [&](int z) -> MenuItem {
                std::vector<MenuItem> mode_items = {
                    leaf_sel("Manual", [fans, z]{ fans->set_zone_auto(z, false); },
                                       [fans, z]{ return !fans->zone_auto(z); }),
                    leaf_sel("Auto (by CPU temp)", [fans, z]{ fans->set_zone_auto(z, true); },
                                       [fans, z]{ return fans->zone_auto(z); }),
                };
                MenuItem speed = slider("Speed", 0.f, 100.f, 5.f, "%",
                    [fans, z]{ return static_cast<float>(fans->zone_speed(z) * 100.0); },
                    [fans, z](float v){ fans->set_zone_speed(z, v / 100.0); });
                speed.visible_fn = [fans, z]{ return !fans->zone_auto(z); };
                MenuItem amin = slider("Auto Min Temp", 30.f, 80.f, 1.f, "\xc2\xb0""C",
                    [fans, z]{ return static_cast<float>(fans->zone_auto_min(z)); },
                    [fans, z](float v){ fans->set_zone_auto_range(z, v, fans->zone_auto_max(z)); });
                amin.visible_fn = [fans, z]{ return fans->zone_auto(z); };
                MenuItem amax = slider("Auto Max Temp", 40.f, 90.f, 1.f, "\xc2\xb0""C",
                    [fans, z]{ return static_cast<float>(fans->zone_auto_max(z)); },
                    [fans, z](float v){ fans->set_zone_auto_range(z, fans->zone_auto_min(z), v); });
                amax.visible_fn = [fans, z]{ return fans->zone_auto(z); };
                MenuItem status = leaf("Output", []{});
                status.label_fn = [fans, z]{
                    char b[40];
                    std::snprintf(b, sizeof(b), "Output: %d%%",
                                  (int)std::lround(fans->zone_duty(z) * 100.0));
                    return std::string(b);
                };
                std::vector<MenuItem> zi = {
                    submenu("Mode", std::move(mode_items)),
                    std::move(speed), std::move(amin), std::move(amax), std::move(status),
                };
                MenuItem m = submenu(fans->zone_name(z), std::move(zi));
                m.label_fn = [fans, z]{
                    char b[48];
                    std::snprintf(b, sizeof(b), "%s  [%d%%%s]", fans->zone_name(z).c_str(),
                                  (int)std::lround(fans->zone_duty(z) * 100.0),
                                  fans->zone_auto(z) ? " auto" : "");
                    return std::string(b);
                };
                return m;
            };
            std::vector<MenuItem> fan_items;
            fan_items.push_back(with_desc(toggle("Enabled",
                [fans]{ return fans->running(); },
                [fans](bool v){ if (v) fans->start(); else fans->stop(); }),
                "Drive the cooling fans on their configured GPIO. Off releases the lines."));
            for (int z = 0; z < fans->zone_count(); ++z)
                fan_items.push_back(build_zone(z));
            MenuItem temp = leaf("CPU Temp", []{});
            temp.label_fn = [fans]{
                char b[32]; std::snprintf(b, sizeof(b), "CPU Temp: %.0f\xc2\xb0""C",
                                          fans->current_temp_c());
                return std::string(b);
            };
            fan_items.push_back(std::move(temp));
            return with_desc(submenu("Cooling Fans", std::move(fan_items)),
                "Pi-driven PWM cooling fans \xe2\x80\x94 up to 4 fans in 2 zones, each "
                "with its own speed/mode. Pins in config[\"fans\"][\"zones\"]; use "
                "GPIO clear of HUB75 (see carrier PINMAP).");
        }());
    software_menu.push_back(with_desc(submenu("Demo Mode", std::move(demo_menu)),
        "Cycle prefab scenes for screenshots / video."));

    // KDE Connect now lives under System > Connectivity (alongside SSH/Bluetooth);
    // Communications keeps the notification log + QR codes. The item itself is
    // built by the Communications builder and handed over through the context.
    connectivity_menu.push_back(std::move(ctx.phone_item));

    // Audio submenu wrapped with its existing live-status context panel.
    MenuItem audio_item = with_panel(
        submenu("Audio", std::move(audio_menu)),
        "Audio Status",
        [&state, menu_sys_pp]
        (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            (void)sz;
            const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                ? (*menu_sys_pp)->accent_color()
                : IM_COL32(255, 255, 255, 255);
            const float lh = 18.f;
            ImVec2 p = o;
            auto row = [&](const char* k, const char* v, ImU32 vc) {
                dl->AddText({ p.x, p.y }, IM_COL32(180, 180, 180, 200), k);
                dl->AddText({ p.x + 130.f, p.y }, vc, v);
                p.y += lh;
            };
            bool enabled;  int  out_idx;  float cpu;  int  xruns;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                enabled = state.audio.enabled;
                out_idx = state.audio.output;
                cpu     = state.audio.cpu_load;
                xruns   = state.audio.xrun_count;
            }
            const char* out_name =
                out_idx == 0 ? "VITURE" :
                out_idx == 1 ? "Headphones" :
                out_idx == 2 ? "HDMI" : "?";
            row("State",  enabled ? "Enabled" : "Muted",
                enabled ? ((accent & 0x00FFFFFFu) | (220u << 24))
                        : IM_COL32(200, 90, 90, 220));
            row("Output", out_name, IM_COL32(230, 230, 230, 230));
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f %%", cpu * 100.f);
            row("CPU load", buf, IM_COL32(230, 230, 230, 230));
            snprintf(buf, sizeof(buf), "%d", xruns);
            row("XRuns", buf,
                xruns == 0 ? IM_COL32(160, 220, 160, 220)
                           : IM_COL32(220, 160, 90, 220));
        });

    // ── System root: grouped, one screen tall ────────────────────────────────
    // ── Temperature readout ──────────────────────────────────────────────────
    // Live rows from state.temps (DS18B20 probes published by sensor::TempSensors).
    auto temp_count = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                return static_cast<int>(state.temps.size()); };
    std::vector<MenuItem> temp_rows = make_dynamic_rows(8, temp_count,
        [&state](int i) -> MenuItem {
            MenuItem m = leaf("Temp", []{});
            m.label_fn = [&state, i]() -> std::string {
                std::lock_guard<std::mutex> lk(state.mtx);
                if (i >= static_cast<int>(state.temps.size())) return "";
                const TempReading& t = state.temps[i];
                char b[80];
                if (!t.ok)
                    std::snprintf(b, sizeof b, "%s:  --", t.label.c_str());
                else
                    std::snprintf(b, sizeof b, "%s:  %.1f\xc2\xb0""C%s", t.label.c_str(),
                                  t.c, t.crit ? "  CRIT" : t.warn ? "  warn" : "");
                return std::string(b);
            };
            m.warn_fn = [&state, i]{ std::lock_guard<std::mutex> lk(state.mtx);
                return i < static_cast<int>(state.temps.size()) && state.temps[i].warn; };
            return m;
        });
    // BME280 environment rows (hidden until the sensor reports).
    {
        auto env_row = [&state](const char* fmt, int which) {
            MenuItem m = leaf("Env", []{});
            m.label_fn = [&state, fmt, which]() -> std::string {
                std::lock_guard<std::mutex> lk(state.mtx);
                if (!state.env.ok) return "";
                const float v = which == 0 ? state.env.temp_c
                              : which == 1 ? state.env.humidity_pct
                                           : state.env.pressure_hpa;
                char b[64];
                std::snprintf(b, sizeof b, fmt, static_cast<double>(v));
                return std::string(b);
            };
            m.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                     return state.env.ok; };
            return m;
        };
        temp_rows.push_back(env_row("Env Temp:  %.1f\xc2\xb0""C", 0));
        temp_rows.push_back(env_row("Humidity:  %.0f %%", 1));
        temp_rows.push_back(env_row("Pressure:  %.1f hPa", 2));
    }
    MenuItem temperature_item = with_desc(
        submenu("Temperature", std::move(temp_rows)),
        "Live temperature probes (DS18B20 1-Wire). Rows turn amber/red at the "
        "warn/crit thresholds. Configure the probe list in cfg[\"temperature\"] "
        "(ids from `ls /sys/bus/w1/devices/`); a probe file can also drive the "
        "fan curve via cfg[\"fans\"][\"temp_path\"].");
    temperature_item.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                            return !state.temps.empty() || state.env.ok; };

    std::vector<MenuItem> system_menu = {
        with_desc(submenu("Display", std::move(display_menu)),
                  "Fullscreen / frameless window mode and windowed resolution."),
        with_desc(submenu("HUD & Menu", std::move(display_hud_menu)),
                  "Text scale, HUD/menu theme + presets, radial-menu options."),
        std::move(audio_item),
        with_desc(submenu("Connectivity",     std::move(connectivity_menu)),
                  "SSH, Bluetooth and other network/peripheral toggles."),
        with_desc(submenu("Pi Settings",      std::move(pi_settings_items)),
                  "Hostname, time, storage and cooling fans. (GPIO buttons + the "
                  "pin visualizer moved to the top-level GPIO menu.)"),
        with_desc(submenu("XR Headset (Viture Beast)", std::move(headset_menu)),
                  "Electrochromic transparency, HUD/backlight brightness, recenter, "
                  "gaze lock and 3D side-by-side \xe2\x80\x94 specific to the glasses."),
        with_desc(submenu("Timers and Alarm",   std::move(timers_alarm_menu)),
                  "Stopwatches, countdowns and one-shot alarms."),
        with_desc(submenu("Diagnostics",        std::move(diagnostics_group_menu)),
                  "Probes, overlays and per-tab tools."),
        std::move(temperature_item),
        with_desc(submenu("Power",              std::move(power_menu)),
                  "Restart ProtoHUD / ProtoFace, reboot or shut down the Pi."),
    };

    // ── Profiles ────────────────────────────────────────────────────────────────
    // Save the current setup as a named snapshot, load one (relaunches ProtoHUD),
    // or delete one. The load/delete lists are dynamic (make_dynamic_rows): the
    // rows read the ProfileManager live, so newly-saved profiles appear without
    // rebuilding the menu.
    // (kProfileSlots declared above, near the HUD/Menu Presets section.)
    auto profiles_count = [profiles]{ return profiles ? profiles->count() : 0; };
    std::vector<MenuItem> profile_delete_menu = make_dynamic_rows(
        kProfileSlots, profiles_count, [&](int i) -> MenuItem {
        MenuItem m;
        m.type       = MenuItemType::LEAF;
        m.label      = "profile";
        m.label_fn   = [profiles, i]{ return profiles ? profiles->name(i) : std::string(); };
        m.description = "Delete this profile permanently.";
        m.action = [state_ptr, profiles, i]{
            if (!state_ptr || !profiles) return;
            std::string nm = profiles->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->profile_delete_name = nm;
        };
        return m;
    });

    std::vector<MenuItem> profiles_menu;
    profiles_menu.push_back(with_desc(
        leaf("Save Current As...", [menu_sys_pp, state_ptr]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Profile Name", std::string(),
                [state_ptr](const std::string& name){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->profile_save_name = name;
                });
        }),
        "Save every current setting (HUD layout, menu style, camera/vision, "
        "Protoface look) as a named profile you can switch to later."));
    for (auto& m : make_dynamic_rows(kProfileSlots, profiles_count,
                                     [&](int i) -> MenuItem {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "profile";
        m.label_fn    = [profiles, i]{ return profiles ? profiles->name(i) : std::string(); };
        m.description = "Load this profile. ProtoHUD restarts to apply it.";
        m.action = [state_ptr, profiles, i]{
            if (!state_ptr || !profiles) return;
            std::string nm = profiles->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->profile_load_name = nm;
        };
        return m;
    }))
        profiles_menu.push_back(std::move(m));
    profiles_menu.push_back(with_desc(submenu("Delete Profile", std::move(profile_delete_menu)),
        "Remove a saved profile permanently."));

    // Assemble Software last, now that profiles_menu exists: the in-HUD Updater
    // first, then Demo Mode (appended earlier), then Profiles & Backup. Added to
    // the System root as a single "Software" tab.
    software_menu.insert(software_menu.begin(),
        with_desc(submenu("Updates", std::move(updates_menu)),
            "Check the repo for new commits, switch/update branches, and roll back "
            "\xe2\x80\x94 all user-initiated. ProtoHUD never auto-updates."));
    // Pi system (apt) update — confirm-gated, since it changes the OS. Runs in
    // the background and toasts on completion.
    software_menu.push_back(confirm_action("Pi System Update",
        "Runs apt-get update && apt-get -y upgrade in the background (30 s to "
        "several minutes). Don't power off while it runs.",
        [run_sh]{ run_sh("System Update",
            "sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get -y upgrade"); }));
    // Submit Issue — compose a short report; saved under state/issues/ and (when a
    // phone is connected) shared to it so it can be filed on GitHub. No network /
    // token is used on the Pi, so it can't post directly.
    software_menu.push_back(with_desc(leaf("Submit Issue\xE2\x80\xA6",
        [menu_sys_pp, &state, kdc_p]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Describe the issue", "",
                [&state, kdc_p](const std::string& text){
                    if (text.empty()) return;
                    char exe[4096]; ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
                    std::string root;
                    if (n > 0) { exe[n] = '\0'; std::string p(exe);
                        auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);
                        s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);
                        root = p; }
                    const std::string dir = (root.empty() ? std::string("/tmp")
                                                          : root + "/state/issues");
                    std::error_code ec; std::filesystem::create_directories(dir, ec);
                    std::time_t t = std::time(nullptr); char ts[32];
                    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
                    const std::string path = dir + "/issue_" + ts + ".txt";
                    { std::ofstream f(path);
                      if (f) f << "ProtoHUD issue (" << ts << ")\n\n" << text << "\n\n"
                               << "File at: https://github.com/brooksthemaker/protohud/issues/new\n"; }
                    if (kdc_p) kdc_p->share_file(path);   // hand off to the phone if paired
                    Notification nt; nt.type = NotifType::App; nt.icon = "message";
                    nt.title = "Issue saved"; nt.body = path; nt.auto_dismiss_s = 6.f;
                    std::lock_guard<std::mutex> lk(state.mtx); state.notifs.push(std::move(nt));
                });
        }),
        "Write a short bug report. Saved under state/issues/ and shared to the "
        "phone (if connected) so you can file it on GitHub."));
    software_menu.push_back(with_desc(submenu("Profiles & Backup", std::move(profiles_menu)),
        "Save and load full-setup snapshots. Loading a profile restarts ProtoHUD "
        "with that config."));
    system_menu.push_back(with_desc(submenu("Software", std::move(software_menu)),
        "In-HUD updater, Pi system update, demo scenes, issue reports, and "
        "full-setup profiles & backups."));

    return system_menu;
}

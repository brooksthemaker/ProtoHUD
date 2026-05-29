#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../serial/face_controller.h"

struct AppState;

// ── ProtoHUD companion-app control server ─────────────────────────────────────
//
// A small REST + JSON HTTP server that lets the ProtoHUD companion app (Flutter)
// drive the helmet over IP. It is reachable two ways, both of which terminate at
// the same TCP socket bound on the CM5:
//   (a) the Pi's own broadcast Wi-Fi network (AP mode — see scripts/setup_companion_ap.sh)
//   (b) a USB tether (USB-ethernet gadget — see scripts/setup_companion_usb.sh)
//
// DESIGN — mirrors WirelessController exactly:
//   • Face commands (color / effect / gif / brightness / palette / expression)
//     are forwarded straight to the FaceProxy, just like the menu leaf lambdas
//     and the WirelessController's on_select path already do from non-render
//     threads. No new locking is introduced.
//   • "Pad" actions (nav / select / back / menu / af / capture) fire std::function
//     callbacks on the HTTP worker thread. main.cpp wires these to the SAME
//     lambdas used for the WirelessController, so behaviour is identical no matter
//     which transport drove the press.
//
// THREADING: handlers run on cpp-httplib worker threads. Pad callbacks must be
// safe to call off the render thread (the menu/hud lambdas already are — that is
// how the SmartKnob and WirelessController call them today). Status reads take
// AppState::mtx.
//
// The server is entirely optional: when Config::enabled is false, start() is a
// no-op and no socket is opened.
class RemoteControlServer {
public:
    using Cb = std::function<void()>;

    struct Config {
        bool        enabled = false;
        std::string host    = "0.0.0.0";  // bind all interfaces (AP + USB + LAN)
        int         port    = 8780;
        // Optional shared secret. When non-empty, every request must carry a
        // matching `X-ProtoHUD-Token` header. Empty disables auth — fine for the
        // isolated AP / USB-tether links the companion app uses.
        std::string token;
    };

    RemoteControlServer(AppState& state, FaceProxy* face, Config cfg);
    ~RemoteControlServer();

    RemoteControlServer(const RemoteControlServer&)            = delete;
    RemoteControlServer& operator=(const RemoteControlServer&) = delete;

    // Spawns the listener thread. Returns false (and does nothing) when disabled.
    bool start();
    void stop();

    bool running() const { return running_.load(); }
    int  port()    const { return cfg_.port; }

    // ── Pad action callbacks — fire on an HTTP worker thread ──────────────────
    // (Wire these to the same menu/hud lambdas as WirelessController in main.cpp.)
    void on_select   (Cb cb) { select_cb_   = std::move(cb); }
    void on_back     (Cb cb) { back_cb_     = std::move(cb); }
    void on_menu     (Cb cb) { menu_cb_     = std::move(cb); }
    void on_af       (Cb cb) { af_cb_       = std::move(cb); }
    void on_capture  (Cb cb) { capture_cb_  = std::move(cb); }
    void on_nav_up   (Cb cb) { nav_up_cb_   = std::move(cb); }
    void on_nav_down (Cb cb) { nav_down_cb_ = std::move(cb); }
    void on_nav_left (Cb cb) { nav_left_cb_ = std::move(cb); }
    void on_nav_right(Cb cb) { nav_right_cb_= std::move(cb); }

private:
    void run();
    bool dispatch_pad(const std::string& button);

    AppState&   state_;
    FaceProxy*  face_ = nullptr;
    Config      cfg_;

    std::thread        thread_;
    std::atomic<bool>  running_ { false };

    // Opaque httplib::Server, hidden from this header so cpp-httplib only needs
    // to be included in the .cpp (keeps the heavy single-header out of the build
    // graph for every other translation unit).
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Cb select_cb_, back_cb_, menu_cb_, af_cb_, capture_cb_;
    Cb nav_up_cb_, nav_down_cb_, nav_left_cb_, nav_right_cb_;
};

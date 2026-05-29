#pragma once
// ── kdeconnect_bridge.h ───────────────────────────────────────────────────────
// Subscribes to the paired phone's KDE Connect notifications (DBus session bus)
// and pushes them into AppState::notifs so they surface in the same toast +
// log pipeline as scheduler reminders. Phase 1 covers RX only — the public
// surface is sized for the later phases (TX notifications, battery, media,
// run-command) but those methods are stubbed until we get there.
//
// Threading: a single worker thread owns the DBus connection; the public
// methods just poke atomics or post requests it picks up on the next dispatch.
// Safe to construct + start() before AppState is fully populated; the worker
// no-ops cleanly when kdeconnectd isn't reachable so dev hosts without it
// installed don't error.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct AppState;

namespace integrations {

struct KdeConnectConfig {
    bool        enabled         = true;
    // Preferred device id (KDE Connect assigns each pairing a stable id).
    // Empty → use the first paired + reachable device the daemon reports.
    std::string device_id;
    // Auto-dismiss applied to each phone notification when pushed into
    // AppState::notifs. 0 keeps the toast until manually dismissed.
    float       auto_dismiss_s  = 8.f;
    // Comma-separated app-name substrings that should NOT be forwarded
    // (e.g. "KDE Connect" so the daemon's own status pings don't loop).
    std::string app_blocklist   = "KDE Connect";
};

class KdeConnectBridge {
public:
    KdeConnectBridge(AppState& state, KdeConnectConfig cfg);
    ~KdeConnectBridge();

    KdeConnectBridge(const KdeConnectBridge&)            = delete;
    KdeConnectBridge& operator=(const KdeConnectBridge&) = delete;

    // Open the session-bus connection and spawn the worker. Returns false
    // when the daemon isn't reachable or libdbus isn't compiled in; the
    // object stays usable (stop()/destroy are safe) but no events flow.
    bool start();
    void stop();

    bool running()        const { return running_.load(); }
    bool daemon_present() const { return daemon_ok_.load(); }   // kdeconnectd visible
    bool device_ready()   const { return device_ok_.load(); }   // a paired+reachable device picked

    // What the worker resolved on its most recent scan — useful for the
    // System menu's KDE Connect status row. Empty until the worker has
    // run a discovery pass.
    std::string active_device_name() const;
    std::string active_device_id()   const;

    // Live-tunable config — applied on the next worker dispatch.
    void set_auto_dismiss(float seconds) { cfg_.auto_dismiss_s = seconds; }
    void set_app_blocklist(std::string csv);

private:
    void worker();

    AppState&            state_;
    KdeConnectConfig     cfg_;

    std::atomic<bool>    running_{false};
    std::atomic<bool>    daemon_ok_{false};
    std::atomic<bool>    device_ok_{false};

    mutable std::mutex   info_mtx_;
    std::string          active_device_name_;
    std::string          active_device_id_;

    // Notification IDs we've already pushed this session, so reposted /
    // re-emitted signals don't duplicate. Cleared on stop().
    mutable std::mutex          seen_mtx_;
    std::unordered_set<std::string> seen_;

    std::thread          thread_;

    // Opaque pointer to DBusConnection — kept void* in the header so we
    // don't drag dbus headers into translation units that don't need
    // them. cpp file casts back to DBusConnection*.
    void*                conn_ = nullptr;
};

} // namespace integrations

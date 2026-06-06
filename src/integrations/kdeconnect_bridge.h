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
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
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
    // Comma-separated app-name substrings treated as chat/DM messages: these
    // get a larger toast (sender as the title, message wrapped below).
    std::string message_apps    = "Discord,Messages,Messenger,Signal,WhatsApp,Telegram,SMS,Slack";
    // Comma-separated substrings matched against a notification's title + text
    // (case-insensitive). Anything matching is dropped — use it to mute noisy
    // Discord servers / group chats by name.
    std::string ignore_list;
    // Push a HUD warning when the phone's battery drops to/below this percent
    // while discharging (raised once per crossing). 0 disables the alert.
    int         low_battery_pct = 20;
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

    // Ring the paired phone (KDE Connect findmyphone plugin) so it plays its
    // ringtone — useful for locating it. Returns false when no device is bound
    // or DBus isn't available. Safe to call from any thread.
    bool ring_phone();

    // Share a file with the paired phone (KDE Connect Share plugin → it lands in
    // the phone's Downloads). Queues the path for the worker thread; returns
    // false when the bridge isn't running. Safe to call from any thread.
    bool share_file(const std::string& path);

    // Share a URL/text with the paired phone (KDE Connect opens http(s) links in
    // the browser). Same queue/threading as share_file.
    bool share_url(const std::string& url);

    // Post a notification on the paired phone with a custom message (KDE Connect
    // ping plugin). URLs in the message are tappable in the phone's notification
    // shade and nothing auto-opens. Same queue/threading as share_file.
    bool send_ping(const std::string& message);

    // ── Snapshots read by the menu / HUD (mutex-guarded copies) ───────────────
    // A phone notification we've seen this session — used for the in-HUD reply /
    // dismiss list. `id` is the KDE Connect notification id (object suffix).
    struct PhoneNotif {
        std::string id, app, title, text;
        bool repliable = false;
    };
    // Now-playing snapshot from the phone's media session (mprisremote plugin).
    struct MediaStatus {
        bool        has_player = false;
        bool        playing    = false;
        std::string player, title, artist, album;
        int         volume = -1;        // 0..100, -1 = unknown
        std::vector<std::string> players;
    };
    // Cellular connectivity (connectivity_report plugin).
    struct Connectivity {
        bool        ok = false;
        std::string network_type;       // "LTE", "5G", "HSPA", …
        int         strength = -1;      // 0..4 bars, -1 = unknown
    };
    struct RunCommand { std::string key, name; };

    std::vector<PhoneNotif> phone_notifications() const;
    MediaStatus             media_status()        const;
    Connectivity            connectivity()         const;
    std::vector<RunCommand> run_commands()        const;
    // Roster for the grouped ignore picker: app → sorted distinct senders seen.
    std::vector<std::pair<std::string, std::vector<std::string>>> notif_roster() const;
    // Is `needle` currently muted by the ignore list? (case-insensitive substring
    // against the live list — lets the picker show ignored rows in red).
    bool is_ignored(const std::string& needle) const;
    // Toggle a sender/app substring in the ignore list (adds if absent, removes
    // the exact entry if present). Returns the new "ignored?" state.
    bool toggle_ignore(const std::string& needle);

    // ── TX actions (queued; the worker drains them on its next loop) ──────────
    // Reply to a repliable phone notification (messaging apps). Empty message is
    // ignored. Returns false when the bridge isn't running.
    bool reply_notification(const std::string& notif_id, const std::string& message);
    // Dismiss a phone notification (empty id → dismiss every active one).
    bool dismiss_notification(const std::string& notif_id);
    // Trigger one of the phone's saved commands (remotecommands plugin).
    bool run_command(const std::string& key);
    // Media transport: "Play","Pause","PlayPause","Next","Previous","Stop".
    bool media_action(const std::string& action);
    bool media_set_volume(int volume);                 // 0..100
    bool media_set_player(const std::string& player);  // switch active session
    // Send an SMS via the phone (sms plugin). address = phone number.
    bool send_sms(const std::string& address, const std::string& message);
    // Silence an incoming call's ringer (telephony plugin; best-effort).
    bool mute_ringer();

    // Live-tunable config — applied on the next worker dispatch.
    void set_auto_dismiss(float seconds) { cfg_.auto_dismiss_s = seconds; }
    void set_app_blocklist(std::string csv);
    void set_message_apps(std::string csv);
    void set_ignore_list(std::string csv);

private:
    void worker();

    AppState&            state_;
    KdeConnectConfig     cfg_;

    std::atomic<bool>    running_{false};
    std::atomic<bool>    daemon_ok_{false};
    std::atomic<bool>    device_ok_{false};
    std::atomic<bool>    ring_request_{false};   // menu → worker: ring the phone
    int                  ring_attempts_ = 0;     // worker-only: retry budget while no device

    std::mutex               share_mtx_;         // guards share_queue_ + ping_queue_
    std::vector<std::string> share_queue_;       // menu → worker: URLs/files to share
    std::vector<std::string> ping_queue_;        // menu → worker: ping messages

    // ── Extra TX queues (menu → worker), guarded by tx_mtx_ ───────────────────
    struct ReplyReq { std::string id, msg; };
    struct SmsReq   { std::string addr, msg; };
    struct MediaReq { std::string kind, arg; };   // kind: "action"/"volume"/"player"
    std::mutex               tx_mtx_;
    std::vector<ReplyReq>    reply_q_;
    std::vector<std::string> dismiss_q_;          // notif id; "" = dismiss all
    std::vector<std::string> runcmd_q_;
    std::vector<MediaReq>    media_q_;
    std::vector<SmsReq>      sms_q_;
    std::atomic<bool>        mute_ringer_req_{false};

    // ── Snapshot state (worker writes, menu/HUD read), guarded by snap_mtx_ ────
    mutable std::mutex                                 snap_mtx_;
    std::vector<PhoneNotif>                            phone_notifs_;
    MediaStatus                                        media_;
    Connectivity                                       connectivity_;
    std::vector<RunCommand>                            commands_;
    bool                                               commands_fetched_ = false;
    std::map<std::string, std::set<std::string>>       roster_;   // app → senders
    int                                                last_batt_alert_pct_ = 200;  // worker-only

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

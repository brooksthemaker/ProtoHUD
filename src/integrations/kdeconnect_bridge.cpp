#include "kdeconnect_bridge.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <dbus/dbus.h>

#include "../app_state.h"

namespace integrations {

namespace {

// Standard KDE Connect bus locations.
constexpr const char* kKdeService           = "org.kde.kdeconnect";
constexpr const char* kDaemonObject         = "/modules/kdeconnect";
constexpr const char* kDaemonInterface      = "org.kde.kdeconnect.daemon";
constexpr const char* kDeviceInterface      = "org.kde.kdeconnect.device";
constexpr const char* kNotificationsIface   = "org.kde.kdeconnect.device.notifications";
constexpr const char* kNotificationIface    = "org.kde.kdeconnect.device.notifications.notification";
constexpr const char* kBatteryIface         = "org.kde.kdeconnect.device.battery";
constexpr const char* kFindMyPhoneIface     = "org.kde.kdeconnect.device.findmyphone";
constexpr const char* kPropertiesIface      = "org.freedesktop.DBus.Properties";

// Helper: append a single string argument to a DBus message.
void msg_append_str(DBusMessage* msg, const char* s) {
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s);
}

// Method call returning a single string reply (e.g. property Get on a string).
// Returns empty on any error so callers can keep going without crashing on
// missing fields.
std::string call_get_str_prop(DBusConnection* conn,
                              const char* path,
                              const char* iface_for_prop,
                              const char* prop) {
    DBusMessage* msg = dbus_message_new_method_call(
        kKdeService, path, kPropertiesIface, "Get");
    if (!msg) return {};
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_for_prop);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);

    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); if (reply) dbus_message_unref(reply); return {}; }
    if (!reply) return {};

    // Reply is a variant carrying a string.
    DBusMessageIter rit, vit;
    if (!dbus_message_iter_init(reply, &rit) ||
        dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply); return {};
    }
    dbus_message_iter_recurse(&rit, &vit);
    if (dbus_message_iter_get_arg_type(&vit) != DBUS_TYPE_STRING) {
        dbus_message_unref(reply); return {};
    }
    const char* val = nullptr;
    dbus_message_iter_get_basic(&vit, &val);
    std::string out = val ? std::string(val) : std::string();
    dbus_message_unref(reply);
    return out;
}

// Variant of call_get_str_prop for int / bool properties. Returns the
// caller's fallback when the property is missing or the wrong type so
// "not bound yet" cleanly stays the empty state.
int call_get_int_prop(DBusConnection* conn,
                      const char* path,
                      const char* iface_for_prop,
                      const char* prop, int fallback) {
    DBusMessage* msg = dbus_message_new_method_call(
        kKdeService, path, kPropertiesIface, "Get");
    if (!msg) return fallback;
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_for_prop);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); if (reply) dbus_message_unref(reply); return fallback; }
    if (!reply) return fallback;
    DBusMessageIter rit, vit;
    if (!dbus_message_iter_init(reply, &rit) ||
        dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply); return fallback;
    }
    dbus_message_iter_recurse(&rit, &vit);
    const int t = dbus_message_iter_get_arg_type(&vit);
    int out = fallback;
    if (t == DBUS_TYPE_INT32 || t == DBUS_TYPE_UINT32) {
        dbus_uint32_t v = 0;
        dbus_message_iter_get_basic(&vit, &v);
        out = static_cast<int>(v);
    }
    dbus_message_unref(reply);
    return out;
}

bool call_get_bool_prop(DBusConnection* conn,
                        const char* path,
                        const char* iface_for_prop,
                        const char* prop, bool fallback) {
    DBusMessage* msg = dbus_message_new_method_call(
        kKdeService, path, kPropertiesIface, "Get");
    if (!msg) return fallback;
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_for_prop);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); if (reply) dbus_message_unref(reply); return fallback; }
    if (!reply) return fallback;
    DBusMessageIter rit, vit;
    if (!dbus_message_iter_init(reply, &rit) ||
        dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply); return fallback;
    }
    dbus_message_iter_recurse(&rit, &vit);
    bool out = fallback;
    if (dbus_message_iter_get_arg_type(&vit) == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t v = FALSE;
        dbus_message_iter_get_basic(&vit, &v);
        out = (v == TRUE);
    }
    dbus_message_unref(reply);
    return out;
}

// Call a method returning array of strings (e.g. daemon.devices, notifications.activeNotifications).
std::vector<std::string> call_get_str_array(DBusConnection* conn,
                                            const char* path,
                                            const char* iface,
                                            const char* method,
                                            // Optional two bool args for daemon.devices(visible, paired)
                                            bool has_bool_args = false,
                                            bool b1 = true, bool b2 = true) {
    DBusMessage* msg = dbus_message_new_method_call(
        kKdeService, path, iface, method);
    if (!msg) return {};
    if (has_bool_args) {
        DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
        dbus_bool_t bv1 = b1 ? TRUE : FALSE;
        dbus_bool_t bv2 = b2 ? TRUE : FALSE;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &bv1);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &bv2);
    }
    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); if (reply) dbus_message_unref(reply); return {}; }
    if (!reply) return {};

    std::vector<std::string> out;
    DBusMessageIter rit, ait;
    if (!dbus_message_iter_init(reply, &rit) ||
        dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply); return out;
    }
    dbus_message_iter_recurse(&rit, &ait);
    while (dbus_message_iter_get_arg_type(&ait) == DBUS_TYPE_STRING) {
        const char* s = nullptr;
        dbus_message_iter_get_basic(&ait, &s);
        if (s) out.emplace_back(s);
        dbus_message_iter_next(&ait);
    }
    dbus_message_unref(reply);
    return out;
}

// Whether the well-known service is currently owned by anyone — quick way
// to tell if kdeconnectd is running without a method call that might block.
bool service_present(DBusConnection* conn) {
    DBusError err; dbus_error_init(&err);
    dbus_bool_t has = dbus_bus_name_has_owner(conn, kKdeService, &err);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); return false; }
    return has == TRUE;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // trim
        auto l = tok.find_first_not_of(" \t");
        auto r = tok.find_last_not_of(" \t");
        if (l == std::string::npos) continue;
        out.push_back(tok.substr(l, r - l + 1));
    }
    return out;
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(hay.begin(), hay.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b){ return std::tolower(a) == std::tolower(b); });
    return it != hay.end();
}

} // namespace

KdeConnectBridge::KdeConnectBridge(AppState& state, KdeConnectConfig cfg)
    : state_(state), cfg_(std::move(cfg)) {}

KdeConnectBridge::~KdeConnectBridge() { stop(); }

std::string KdeConnectBridge::active_device_name() const {
    std::lock_guard<std::mutex> lk(info_mtx_);
    return active_device_name_;
}

std::string KdeConnectBridge::active_device_id() const {
    std::lock_guard<std::mutex> lk(info_mtx_);
    return active_device_id_;
}

void KdeConnectBridge::set_app_blocklist(std::string csv) {
    cfg_.app_blocklist = std::move(csv);
}

void KdeConnectBridge::set_message_apps(std::string csv) {
    cfg_.message_apps = std::move(csv);
}

void KdeConnectBridge::set_ignore_list(std::string csv) {
    cfg_.ignore_list = std::move(csv);
}

bool KdeConnectBridge::ring_phone() {
    if (!running_.load() || !device_ok_.load()) return false;
    ring_request_.store(true);   // worker picks it up on its next dispatch
    return true;
}

bool KdeConnectBridge::start() {
    if (running_.load() || !cfg_.enabled) return false;

    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err) || !conn) {
        if (dbus_error_is_set(&err)) {
            std::fprintf(stderr, "[kdeconnect] session bus get failed: %s\n", err.message);
            dbus_error_free(&err);
        }
        return false;
    }
    // Don't tear down the whole process if the connection drops.
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    conn_ = conn;

    running_.store(true);
    thread_ = std::thread(&KdeConnectBridge::worker, this);
    return true;
}

void KdeConnectBridge::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (conn_) {
        DBusConnection* c = static_cast<DBusConnection*>(conn_);
        dbus_connection_close(c);
        dbus_connection_unref(c);
        conn_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(seen_mtx_);
        seen_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.phone_battery_pct = -1;
        state_.health.phone_charging    = false;
    }
}

void KdeConnectBridge::worker() {
    using clock = std::chrono::steady_clock;
    DBusConnection* conn = static_cast<DBusConnection*>(conn_);

    // Discovery state.
    std::string current_dev_id;
    std::string current_match_rule;
    auto next_discovery = clock::now();
    const auto discovery_interval = std::chrono::seconds(5);

    auto drop_match = [&](const std::string& rule){
        if (rule.empty()) return;
        DBusError e; dbus_error_init(&e);
        dbus_bus_remove_match(conn, rule.c_str(), &e);
        if (dbus_error_is_set(&e)) dbus_error_free(&e);
    };

    auto pick_device = [&]() -> std::string {
        // Honor cfg device_id first; otherwise first paired + reachable.
        if (!cfg_.device_id.empty()) return cfg_.device_id;
        auto ids = call_get_str_array(conn, kDaemonObject, kDaemonInterface,
                                      "devices", true, true, true);
        if (ids.empty()) return {};
        return ids.front();
    };

    auto rebind_to = [&](const std::string& dev_id) {
        drop_match(current_match_rule);
        current_match_rule.clear();
        current_dev_id = dev_id;

        if (dev_id.empty()) {
            device_ok_.store(false);
            std::lock_guard<std::mutex> lk(info_mtx_);
            active_device_id_.clear();
            active_device_name_.clear();
            return;
        }

        // Resolve a friendly device name (best-effort).
        const std::string dev_path =
            std::string("/modules/kdeconnect/devices/") + dev_id;
        std::string nm = call_get_str_prop(conn, dev_path.c_str(),
                                           kDeviceInterface, "name");
        {
            std::lock_guard<std::mutex> lk(info_mtx_);
            active_device_id_   = dev_id;
            active_device_name_ = nm.empty() ? dev_id : nm;
        }
        device_ok_.store(true);

        // Subscribe to notificationPosted signals for this device's
        // notifications plugin object.
        const std::string notif_path = dev_path + "/notifications";
        std::ostringstream rule;
        rule << "type='signal',sender='" << kKdeService << "',"
             << "interface='" << kNotificationsIface << "',"
             << "member='notificationPosted',"
             << "path='" << notif_path << "'";
        current_match_rule = rule.str();
        DBusError e; dbus_error_init(&e);
        dbus_bus_add_match(conn, current_match_rule.c_str(), &e);
        if (dbus_error_is_set(&e)) {
            std::fprintf(stderr, "[kdeconnect] add_match failed: %s\n", e.message);
            dbus_error_free(&e);
            current_match_rule.clear();
        }
    };

    auto handle_notification = [&](const std::string& notif_id) {
        if (notif_id.empty() || current_dev_id.empty()) return;
        {
            std::lock_guard<std::mutex> lk(seen_mtx_);
            if (!seen_.insert(notif_id).second) return;   // already pushed
        }
        const std::string obj =
            "/modules/kdeconnect/devices/" + current_dev_id + "/notifications/" + notif_id;
        // Pull the human-facing fields. ticker is the canonical "what to
        // display" — it's the same string KDE Connect shows on its
        // notification toast on a desktop.
        std::string app_name = call_get_str_prop(conn, obj.c_str(),
                                                 kNotificationIface, "appName");
        std::string ticker   = call_get_str_prop(conn, obj.c_str(),
                                                 kNotificationIface, "ticker");
        std::string title    = call_get_str_prop(conn, obj.c_str(),
                                                 kNotificationIface, "title");
        std::string text     = call_get_str_prop(conn, obj.c_str(),
                                                 kNotificationIface, "text");

        // Blocklist (case-insensitive substring match on appName).
        for (const auto& b : split_csv(cfg_.app_blocklist))
            if (icontains(app_name, b)) return;

        // Ignore list — mute noisy servers / group chats by name (matched
        // against the notification's title + text + ticker).
        for (const auto& ig : split_csv(cfg_.ignore_list))
            if (icontains(title, ig) || icontains(text, ig) || icontains(ticker, ig)) return;

        // Chat / DM apps get a larger toast with the sender as the title and
        // the message wrapped below.
        bool is_msg = false;
        for (const auto& m : split_csv(cfg_.message_apps))
            if (icontains(app_name, m)) { is_msg = true; break; }

        Notification n;
        n.type = NotifType::App;
        if (is_msg) {
            n.big   = true;
            n.icon  = "message";
            // Title = who/where (sender or "Server #channel"); body = the text.
            n.title = !title.empty() ? title
                    : (!app_name.empty() ? app_name : std::string("Message"));
            n.body  = !text.empty() ? text : ticker;
            if (n.body.empty()) n.body = "(no text)";
        } else {
            // Compose: prefer "<title> — <text>"; ticker is the catch-all.
            n.title = !app_name.empty() ? app_name : std::string("Phone");
            if (!title.empty() && !text.empty())   n.body = title + " — " + text;
            else if (!ticker.empty())              n.body = ticker;
            else if (!title.empty())               n.body = title;
            else if (!text.empty())                n.body = text;
            else                                   n.body = "(empty)";
        }
        // Give chat messages a bit longer on screen so they can be read.
        n.auto_dismiss_s = is_msg ? std::max(cfg_.auto_dismiss_s, 14.f)
                                  : cfg_.auto_dismiss_s;

        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.notifs.push(std::move(n));
    };

    int  last_logged_pct      = -2;   // -2 = never logged
    bool last_logged_charging = false;
    auto poll_battery = [&]() {
        if (current_dev_id.empty()) {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.health.phone_battery_pct = -1;
            state_.health.phone_charging    = false;
            return;
        }
        // KDE Connect publishes the battery plugin on a sub-object of the
        // device, not on the device root — gdbus introspect of
        // /modules/kdeconnect/devices/<id>/battery shows the charge +
        // isCharging properties live there.
        const std::string batt_path =
            std::string("/modules/kdeconnect/devices/") + current_dev_id + "/battery";
        const int  pct      = call_get_int_prop(conn, batt_path.c_str(),
                                                kBatteryIface, "charge", -1);
        const bool charging = call_get_bool_prop(conn, batt_path.c_str(),
                                                 kBatteryIface, "isCharging", false);
        const int clamped = (pct >= 0 && pct <= 100) ? pct : -1;
        // Log only on first read or when the values change, so the journal
        // doesn't fill with per-poll noise once the bridge is healthy.
        if (clamped != last_logged_pct || charging != last_logged_charging) {
            std::fprintf(stderr, "[kdeconnect] phone battery %d%%%s\n",
                         clamped, charging ? " (charging)" : "");
            last_logged_pct      = clamped;
            last_logged_charging = charging;
        }
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.phone_battery_pct = clamped;
        state_.health.phone_charging    = charging;
    };

    while (running_.load()) {
        const auto now = clock::now();

        // Periodic discovery: detect kdeconnectd appearing/disappearing
        // and re-pick the device if our binding has gone stale. Battery
        // is polled in the same cycle — KDE Connect doesn't push a
        // PropertiesChanged signal we can subscribe to from libdbus
        // without a generated proxy, and a 5 s refresh is plenty for a
        // chrome indicator.
        if (now >= next_discovery) {
            next_discovery = now + discovery_interval;
            const bool present = service_present(conn);
            daemon_ok_.store(present);
            if (!present) {
                if (!current_dev_id.empty()) rebind_to({});
            } else {
                const std::string want = pick_device();
                if (want != current_dev_id) rebind_to(want);
            }
            poll_battery();
        }

        // Ring-my-phone request (from the menu) — fire the findmyphone plugin
        // on the worker thread so the DBus connection stays single-owner.
        if (ring_request_.exchange(false) && !current_dev_id.empty()) {
            const std::string obj =
                "/modules/kdeconnect/devices/" + current_dev_id + "/findmyphone";
            DBusMessage* msg = dbus_message_new_method_call(
                kKdeService, obj.c_str(), kFindMyPhoneIface, "ring");
            if (msg) {
                dbus_connection_send(conn, msg, nullptr);
                dbus_connection_flush(conn);
                dbus_message_unref(msg);
                std::fprintf(stderr, "[kdeconnect] ring requested\n");
            }
        }

        // Dispatch incoming signals. read_write blocks briefly so we
        // don't busy-spin when nothing is happening.
        if (!dbus_connection_read_write(conn, 200)) {
            // Connection died — wait for daemon to come back.
            daemon_ok_.store(false);
            device_ok_.store(false);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        while (true) {
            DBusMessage* m = dbus_connection_pop_message(conn);
            if (!m) break;
            if (dbus_message_is_signal(m, kNotificationsIface, "notificationPosted")) {
                const char* notif_id = nullptr;
                DBusError e; dbus_error_init(&e);
                if (dbus_message_get_args(m, &e,
                                          DBUS_TYPE_STRING, &notif_id,
                                          DBUS_TYPE_INVALID) && notif_id) {
                    handle_notification(notif_id);
                }
                if (dbus_error_is_set(&e)) dbus_error_free(&e);
            }
            dbus_message_unref(m);
        }
    }

    drop_match(current_match_rule);
}

} // namespace integrations

#include "kdeconnect_bridge.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <dbus/dbus.h>
#include <nlohmann/json.hpp>

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
constexpr const char* kShareIface           = "org.kde.kdeconnect.device.share";
constexpr const char* kPingIface            = "org.kde.kdeconnect.device.ping";
constexpr const char* kMprisIface           = "org.kde.kdeconnect.device.mprisremote";
constexpr const char* kRunCmdIface          = "org.kde.kdeconnect.device.remotecommands";
constexpr const char* kConnIface            = "org.kde.kdeconnect.device.connectivity_report";
constexpr const char* kSmsIface             = "org.kde.kdeconnect.device.sms";
constexpr const char* kTelephonyIface       = "org.kde.kdeconnect.device.telephony";
constexpr const char* kNotifPluginIface     = "org.kde.kdeconnect.device.notifications";
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

// Read a string-array property (variant carrying an array of strings), e.g.
// mprisremote.playerList. Empty on any error / wrong type.
std::vector<std::string> call_get_str_array_prop(DBusConnection* conn,
                                                const char* path,
                                                const char* iface_for_prop,
                                                const char* prop) {
    std::vector<std::string> out;
    DBusMessage* msg = dbus_message_new_method_call(
        kKdeService, path, kPropertiesIface, "Get");
    if (!msg) return out;
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_for_prop);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); if (reply) dbus_message_unref(reply); return out; }
    if (!reply) return out;
    DBusMessageIter rit, vit, ait;
    if (!dbus_message_iter_init(reply, &rit) ||
        dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply); return out;
    }
    dbus_message_iter_recurse(&rit, &vit);
    if (dbus_message_iter_get_arg_type(&vit) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply); return out;
    }
    dbus_message_iter_recurse(&vit, &ait);
    while (dbus_message_iter_get_arg_type(&ait) == DBUS_TYPE_STRING) {
        const char* s = nullptr;
        dbus_message_iter_get_basic(&ait, &s);
        if (s) out.emplace_back(s);
        dbus_message_iter_next(&ait);
    }
    dbus_message_unref(reply);
    return out;
}

// Fire-and-forget method call with no arguments (e.g. notification.dismiss,
// telephony.muteRinger).
void call_method_void(DBusConnection* conn, const char* path,
                      const char* iface, const char* method) {
    DBusMessage* msg = dbus_message_new_method_call(kKdeService, path, iface, method);
    if (!msg) return;
    dbus_connection_send(conn, msg, nullptr);
    dbus_message_unref(msg);
}

// Fire-and-forget method call with one string arg (reply, triggerCommand,
// sendAction, …).
void call_method_str(DBusConnection* conn, const char* path,
                     const char* iface, const char* method, const std::string& arg) {
    DBusMessage* msg = dbus_message_new_method_call(kKdeService, path, iface, method);
    if (!msg) return;
    const char* a = arg.c_str();
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &a);
    dbus_connection_send(conn, msg, nullptr);
    dbus_message_unref(msg);
}

// Fire-and-forget method call with one int32 arg (setVolume).
void call_method_int(DBusConnection* conn, const char* path,
                     const char* iface, const char* method, int arg) {
    DBusMessage* msg = dbus_message_new_method_call(kKdeService, path, iface, method);
    if (!msg) return;
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_int32_t v = arg;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &v);
    dbus_connection_send(conn, msg, nullptr);
    dbus_message_unref(msg);
}

// Properties.Set a string property to `value` (used to switch the active mpris
// player). Fire-and-forget.
void set_str_prop(DBusConnection* conn, const char* path,
                  const char* iface_for_prop, const char* prop, const std::string& value) {
    DBusMessage* msg = dbus_message_new_method_call(
        kKdeService, path, kPropertiesIface, "Set");
    if (!msg) return;
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_for_prop);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    DBusMessageIter var;
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
    const char* v = value.c_str();
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&it, &var);
    dbus_connection_send(conn, msg, nullptr);
    dbus_message_unref(msg);
}

// Send an SMS via the sms plugin. Marshals the current signature
//   sendSms(av addresses, s textMessage, av attachmentUrls, x subID)
// with a single address variant(string), empty attachments, subID -1.
void call_send_sms(DBusConnection* conn, const char* path,
                   const std::string& address, const std::string& message) {
    DBusMessage* msg = dbus_message_new_method_call(kKdeService, path, kSmsIface, "sendSms");
    if (!msg) return;
    DBusMessageIter it; dbus_message_iter_init_append(msg, &it);
    // av addresses = [ variant(string) ]
    DBusMessageIter arr;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "v", &arr);
    {
        DBusMessageIter var;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_VARIANT, "s", &var);
        const char* a = address.c_str();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &a);
        dbus_message_iter_close_container(&arr, &var);
    }
    dbus_message_iter_close_container(&it, &arr);
    // s textMessage
    const char* m = message.c_str();
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &m);
    // av attachmentUrls = []
    DBusMessageIter att;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "v", &att);
    dbus_message_iter_close_container(&it, &att);
    // x subID = -1
    dbus_int64_t sub = -1;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT64, &sub);
    dbus_connection_send(conn, msg, nullptr);
    dbus_message_unref(msg);
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
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.app_blocklist = std::move(csv);
}

void KdeConnectBridge::set_message_apps(std::string csv) {
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.message_apps = std::move(csv);
}

void KdeConnectBridge::set_ignore_list(std::string csv) {
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.ignore_list = std::move(csv);
}

bool KdeConnectBridge::ring_phone() {
    if (!running_.load()) return false;
    // Optimistic: set the request even if the device looks unreachable right now.
    // The worker forces a fresh discovery and keeps trying for a few seconds, so a
    // phone that just reconnected (e.g. woke from doze) still rings.
    ring_request_.store(true);
    return true;
}

bool KdeConnectBridge::share_file(const std::string& path) {
    if (!running_.load() || path.empty()) return false;
    std::lock_guard<std::mutex> lk(share_mtx_);
    share_queue_.push_back("file://" + path);   // queue holds ready-to-send URLs
    return true;
}

bool KdeConnectBridge::share_url(const std::string& url) {
    if (!running_.load() || url.empty()) return false;
    std::lock_guard<std::mutex> lk(share_mtx_);
    share_queue_.push_back(url);
    return true;
}

bool KdeConnectBridge::send_ping(const std::string& message) {
    if (!running_.load() || message.empty()) return false;
    std::lock_guard<std::mutex> lk(share_mtx_);
    ping_queue_.push_back(message);
    return true;
}

// ── Snapshot getters ──────────────────────────────────────────────────────────
std::vector<KdeConnectBridge::PhoneNotif>
KdeConnectBridge::phone_notifications() const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return phone_notifs_;
}

KdeConnectBridge::MediaStatus KdeConnectBridge::media_status() const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return media_;
}

KdeConnectBridge::Connectivity KdeConnectBridge::connectivity() const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return connectivity_;
}

std::vector<KdeConnectBridge::RunCommand> KdeConnectBridge::run_commands() const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return commands_;
}

std::vector<KdeConnectBridge::DeviceInfo> KdeConnectBridge::devices() const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return devices_;
}

bool KdeConnectBridge::request_pairing(const std::string& device_id) {
    if (!running_.load() || device_id.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    pair_q_.push_back({device_id, true});
    return true;
}

bool KdeConnectBridge::unpair(const std::string& device_id) {
    if (!running_.load() || device_id.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    pair_q_.push_back({device_id, false});
    return true;
}

std::vector<std::pair<std::string, std::vector<std::string>>>
KdeConnectBridge::notif_roster() const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    out.reserve(roster_.size());
    for (const auto& [app, senders] : roster_)
        out.emplace_back(app, std::vector<std::string>(senders.begin(), senders.end()));
    return out;
}

bool KdeConnectBridge::is_ignored(const std::string& needle) const {
    if (needle.empty()) return false;
    std::string ign;
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        ign = cfg_.ignore_list;
    }
    for (const auto& e : split_csv(ign))
        if (e == needle || icontains(needle, e)) return true;
    return false;
}

bool KdeConnectBridge::toggle_ignore(const std::string& needle) {
    if (needle.empty()) return false;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    auto items = split_csv(cfg_.ignore_list);
    auto it = std::find(items.begin(), items.end(), needle);
    bool now_ignored;
    if (it != items.end()) { items.erase(it); now_ignored = false; }
    else                   { items.push_back(needle); now_ignored = true; }
    std::string csv;
    for (size_t i = 0; i < items.size(); ++i) { if (i) csv += ','; csv += items[i]; }
    cfg_.ignore_list = std::move(csv);
    return now_ignored;
}

// ── TX action queue pushers ───────────────────────────────────────────────────
bool KdeConnectBridge::reply_notification(const std::string& notif_id,
                                          const std::string& message) {
    if (!running_.load() || notif_id.empty() || message.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    reply_q_.push_back({notif_id, message});
    return true;
}

bool KdeConnectBridge::dismiss_notification(const std::string& notif_id) {
    if (!running_.load()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    dismiss_q_.push_back(notif_id);   // "" → dismiss all
    return true;
}

bool KdeConnectBridge::run_command(const std::string& key) {
    if (!running_.load() || key.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    runcmd_q_.push_back(key);
    return true;
}

bool KdeConnectBridge::media_action(const std::string& action) {
    if (!running_.load() || action.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    media_q_.push_back({"action", action});
    return true;
}

bool KdeConnectBridge::media_set_volume(int volume) {
    if (!running_.load()) return false;
    volume = std::max(0, std::min(100, volume));
    std::lock_guard<std::mutex> lk(tx_mtx_);
    media_q_.push_back({"volume", std::to_string(volume)});
    return true;
}

bool KdeConnectBridge::media_set_player(const std::string& player) {
    if (!running_.load() || player.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    media_q_.push_back({"player", player});
    return true;
}

bool KdeConnectBridge::send_sms(const std::string& address, const std::string& message) {
    if (!running_.load() || address.empty() || message.empty()) return false;
    std::lock_guard<std::mutex> lk(tx_mtx_);
    sms_q_.push_back({address, message});
    return true;
}

bool KdeConnectBridge::mute_ringer() {
    if (!running_.load()) return false;
    mute_ringer_req_.store(true);
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
    std::string current_match_rule;   // notificationPosted match
    std::string current_batt_rule;    // battery.refreshed match (push battery)
    auto next_discovery = clock::now();
    const auto discovery_interval = std::chrono::seconds(5);
    // Media / connectivity / battery polled on a faster cadence so the menu and
    // HUD feel live; battery also arrives via the refreshed signal (push).
    auto next_fast = clock::now();
    const auto fast_interval = std::chrono::seconds(2);
    std::string seeded_dev;   // device whose active notifications we've ingested

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
        drop_match(current_match_rule); current_match_rule.clear();
        drop_match(current_batt_rule);  current_batt_rule.clear();
        current_dev_id = dev_id;
        // Reset device-scoped snapshots so a disconnect clears the menus.
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            phone_notifs_.clear();
            media_ = MediaStatus{};
            connectivity_ = Connectivity{};
            commands_.clear();
            commands_fetched_ = false;
            roster_.clear();
        }
        last_batt_alert_pct_ = 200;

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

        // Push battery: KDE Connect's battery plugin emits refreshed(b,i) on
        // every change, so we don't have to wait for the next poll.
        const std::string batt_path = dev_path + "/battery";
        std::ostringstream brule;
        brule << "type='signal',sender='" << kKdeService << "',"
              << "interface='" << kBatteryIface << "',"
              << "member='refreshed',"
              << "path='" << batt_path << "'";
        current_batt_rule = brule.str();
        DBusError be; dbus_error_init(&be);
        dbus_bus_add_match(conn, current_batt_rule.c_str(), &be);
        if (dbus_error_is_set(&be)) { dbus_error_free(&be); current_batt_rule.clear(); }
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

        // Snapshot the CSV filters — the menu thread edits these live.
        std::string f_block, f_msg, f_ign;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            f_block = cfg_.app_blocklist;
            f_msg   = cfg_.message_apps;
            f_ign   = cfg_.ignore_list;
        }

        // Blocklist (case-insensitive substring match on appName).
        for (const auto& b : split_csv(f_block))
            if (icontains(app_name, b)) return;

        // Chat / DM apps get a larger toast with the sender as the title and
        // the message wrapped below. (Computed early — also gates "repliable".)
        bool is_msg = false;
        for (const auto& m : split_csv(f_msg))
            if (icontains(app_name, m)) { is_msg = true; break; }

        // Record into the roster (app → senders) for the grouped ignore picker.
        // Done BEFORE the ignore check so muted senders still appear (in red) and
        // can be un-muted. Lightly capped to bound memory.
        {
            const std::string app    = app_name.empty() ? std::string("Phone") : app_name;
            const std::string sender = !title.empty() ? title : app;
            std::lock_guard<std::mutex> lk(snap_mtx_);
            if (roster_.size() < 64 || roster_.count(app)) {
                auto& set = roster_[app];
                if (set.size() < 64) set.insert(sender);
            }
        }

        // Ignore list — mute noisy servers / group chats by name (matched
        // against the notification's title + text + ticker).
        for (const auto& ig : split_csv(f_ign))
            if (icontains(title, ig) || icontains(text, ig) || icontains(ticker, ig)) return;

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

        // Record into the reply/dismiss list (newest first, capped). Messaging
        // apps are treated as repliable.
        {
            PhoneNotif pn;
            pn.id = notif_id; pn.app = app_name; pn.title = title;
            pn.text = !text.empty() ? text : ticker; pn.repliable = is_msg;
            std::lock_guard<std::mutex> lk(snap_mtx_);
            phone_notifs_.insert(phone_notifs_.begin(), std::move(pn));
            if (phone_notifs_.size() > 30) phone_notifs_.resize(30);
        }

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
        // Low-battery alert: raise once when crossing to/below the threshold
        // while discharging. last_batt_alert_pct_ tracks the level we alerted at
        // so re-entering the band (after a charge) re-arms, but a steady drain
        // doesn't spam (it only fires on the crossing).
        if (cfg_.low_battery_pct > 0 && clamped >= 0) {
            const bool below = !charging && clamped <= cfg_.low_battery_pct;
            if (below && last_batt_alert_pct_ > cfg_.low_battery_pct) {
                Notification n;
                n.type = NotifType::App; n.icon = "bell";
                n.title = "Phone battery low";
                n.body  = "Phone at " + std::to_string(clamped) + "% \xE2\x80\x94 not charging";
                n.auto_dismiss_s = 12.f;
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.notifs.push(std::move(n));
            }
            // Re-arm once we climb back above the threshold (or plug in).
            if (charging || clamped > cfg_.low_battery_pct) last_batt_alert_pct_ = 200;
            else                                            last_batt_alert_pct_ = clamped;
        }

        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.phone_battery_pct = clamped;
        state_.health.phone_charging    = charging;
    };

    // Now-playing snapshot from the phone's media session (mprisremote plugin).
    auto poll_media = [&]() {
        if (current_dev_id.empty()) return;
        const std::string mp =
            "/modules/kdeconnect/devices/" + current_dev_id + "/mprisremote";
        MediaStatus m;
        m.players = call_get_str_array_prop(conn, mp.c_str(), kMprisIface, "playerList");
        m.player  = call_get_str_prop(conn, mp.c_str(), kMprisIface, "player");
        if (m.player.empty() && !m.players.empty()) m.player = m.players.front();
        m.has_player = !m.player.empty();
        if (m.has_player) {
            m.playing = call_get_bool_prop(conn, mp.c_str(), kMprisIface, "isPlaying", false);
            m.title   = call_get_str_prop(conn, mp.c_str(), kMprisIface, "title");
            m.artist  = call_get_str_prop(conn, mp.c_str(), kMprisIface, "artist");
            m.album   = call_get_str_prop(conn, mp.c_str(), kMprisIface, "album");
            m.volume  = call_get_int_prop(conn, mp.c_str(), kMprisIface, "volume", -1);
            if (m.title.empty()) {   // some versions only fill nowPlaying
                std::string np = call_get_str_prop(conn, mp.c_str(), kMprisIface, "nowPlaying");
                if (!np.empty()) m.title = np;
            }
        }
        std::lock_guard<std::mutex> lk(snap_mtx_);
        media_ = std::move(m);
    };

    // Cellular connectivity (connectivity_report plugin).
    auto poll_connectivity = [&]() {
        if (current_dev_id.empty()) return;
        const std::string cp =
            "/modules/kdeconnect/devices/" + current_dev_id + "/connectivity_report";
        Connectivity c;
        c.network_type = call_get_str_prop(conn, cp.c_str(), kConnIface, "cellularNetworkType");
        c.strength     = call_get_int_prop(conn, cp.c_str(), kConnIface,
                                           "cellularNetworkStrength", -1);
        c.ok = !c.network_type.empty() || c.strength >= 0;
        std::lock_guard<std::mutex> lk(snap_mtx_);
        connectivity_ = std::move(c);
    };

    // All reachable devices (paired or not) for the pairing picker. Independent
    // of the bound device, so it works before any device is "active".
    auto poll_devices = [&]() {
        auto ids = call_get_str_array(conn, kDaemonObject, kDaemonInterface,
                                      "devices", true, true, false);  // reachable, any pair state
        std::vector<DeviceInfo> out;
        for (const auto& id : ids) {
            const std::string dp = "/modules/kdeconnect/devices/" + id;
            DeviceInfo d; d.id = id;
            d.name      = call_get_str_prop(conn, dp.c_str(), kDeviceInterface, "name");
            d.paired    = call_get_bool_prop(conn, dp.c_str(), kDeviceInterface, "isPaired", false);
            d.reachable = call_get_bool_prop(conn, dp.c_str(), kDeviceInterface, "isReachable", true);
            if (d.name.empty()) d.name = id;
            out.push_back(std::move(d));
        }
        std::lock_guard<std::mutex> lk(snap_mtx_);
        devices_ = std::move(out);
    };

    // Saved phone commands (remotecommands plugin) — fetched once per binding.
    auto fetch_commands = [&]() {
        bool done; { std::lock_guard<std::mutex> lk(snap_mtx_); done = commands_fetched_; }
        if (current_dev_id.empty() || done) return;
        const std::string rp =
            "/modules/kdeconnect/devices/" + current_dev_id + "/remotecommands";
        const std::string js = call_get_str_prop(conn, rp.c_str(), kRunCmdIface, "commands");
        std::vector<RunCommand> cmds;
        if (!js.empty()) {
            try {
                auto j = nlohmann::json::parse(js);
                for (auto it = j.begin(); it != j.end(); ++it) {
                    RunCommand rc; rc.key = it.key();
                    rc.name = it.value().is_object()
                                ? it.value().value("name", rc.key) : rc.key;
                    cmds.push_back(std::move(rc));
                }
            } catch (...) {}
        }
        std::lock_guard<std::mutex> lk(snap_mtx_);
        commands_ = std::move(cmds);
        commands_fetched_ = true;
    };

    // Seed the roster + reply/dismiss list from the phone's already-active
    // notifications when we first bind, so the menus aren't empty until new
    // notifications arrive. Doesn't raise toasts (these predate the session).
    auto seed_active = [&]() {
        if (current_dev_id.empty()) return;
        const std::string np =
            "/modules/kdeconnect/devices/" + current_dev_id + "/notifications";
        auto ids = call_get_str_array(conn, np.c_str(), kNotifPluginIface,
                                      "activeNotifications");
        // Snapshot the CSV filters once for the batch — menu edits them live.
        std::string f_block, f_msg;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            f_block = cfg_.app_blocklist;
            f_msg   = cfg_.message_apps;
        }
        for (const auto& id : ids) {
            { std::lock_guard<std::mutex> lk(seen_mtx_); seen_.insert(id); }  // don't re-toast
            const std::string obj = np + "/" + id;
            std::string app   = call_get_str_prop(conn, obj.c_str(), kNotificationIface, "appName");
            std::string title = call_get_str_prop(conn, obj.c_str(), kNotificationIface, "title");
            std::string text  = call_get_str_prop(conn, obj.c_str(), kNotificationIface, "text");
            bool block = false;
            for (const auto& b : split_csv(f_block))
                if (icontains(app, b)) { block = true; break; }
            if (block) continue;
            bool is_msg = false;
            for (const auto& m : split_csv(f_msg))
                if (icontains(app, m)) { is_msg = true; break; }
            const std::string A = app.empty() ? std::string("Phone") : app;
            const std::string S = !title.empty() ? title : A;
            std::lock_guard<std::mutex> lk(snap_mtx_);
            if (roster_.size() < 64 || roster_.count(A)) {
                auto& set = roster_[A]; if (set.size() < 64) set.insert(S);
            }
            bool exists = false;
            for (const auto& p : phone_notifs_) if (p.id == id) { exists = true; break; }
            if (!exists) {
                PhoneNotif pn; pn.id = id; pn.app = app; pn.title = title;
                pn.text = text; pn.repliable = is_msg;
                phone_notifs_.push_back(std::move(pn));
                if (phone_notifs_.size() > 30) phone_notifs_.resize(30);
            }
        }
    };

    // Drain the menu→worker TX queues (reply / dismiss / runcommand / media /
    // sms / mute-ringer). Single-owner DBus access stays on this thread.
    auto drain_tx = [&]() {
        std::vector<ReplyReq>    replies;
        std::vector<std::string> dismisses, runcmds;
        std::vector<MediaReq>    medias;
        std::vector<SmsReq>      smses;
        std::vector<std::pair<std::string, bool>> pairs;
        {
            std::lock_guard<std::mutex> lk(tx_mtx_);
            replies.swap(reply_q_);   dismisses.swap(dismiss_q_);
            runcmds.swap(runcmd_q_);  medias.swap(media_q_);  smses.swap(sms_q_);
            pairs.swap(pair_q_);
        }
        // Pairing targets an explicit device id, so handle it before the
        // "is a device bound?" guard (you pair a not-yet-paired device).
        for (const auto& [id, do_pair] : pairs) {
            const std::string dp = "/modules/kdeconnect/devices/" + id;
            call_method_void(conn, dp.c_str(), kDeviceInterface,
                             do_pair ? "requestPairing" : "unpair");
            std::fprintf(stderr, "[kdeconnect] %s %s\n",
                         do_pair ? "pair-request" : "unpair", id.c_str());
        }
        if (!pairs.empty()) { dbus_connection_flush(conn); poll_devices(); }
        const bool want_mute = mute_ringer_req_.exchange(false);
        const bool any = !replies.empty() || !dismisses.empty() || !runcmds.empty() ||
                         !medias.empty() || !smses.empty() || want_mute;
        if (!any) return;
        if (current_dev_id.empty()) return;     // nothing reachable — drop silently
        const std::string base = "/modules/kdeconnect/devices/" + current_dev_id;
        for (const auto& r : replies)
            call_method_str(conn, (base + "/notifications/" + r.id).c_str(),
                            kNotificationIface, "reply", r.msg);
        for (const auto& d : dismisses) {
            if (!d.empty()) {
                call_method_void(conn, (base + "/notifications/" + d).c_str(),
                                 kNotificationIface, "dismiss");
            } else {
                auto ids = call_get_str_array(conn, (base + "/notifications").c_str(),
                                              kNotifPluginIface, "activeNotifications");
                for (const auto& id : ids)
                    call_method_void(conn, (base + "/notifications/" + id).c_str(),
                                     kNotificationIface, "dismiss");
            }
            std::lock_guard<std::mutex> lk(snap_mtx_);
            if (d.empty()) phone_notifs_.clear();
            else phone_notifs_.erase(
                std::remove_if(phone_notifs_.begin(), phone_notifs_.end(),
                               [&](const PhoneNotif& p){ return p.id == d; }),
                phone_notifs_.end());
        }
        for (const auto& k : runcmds)
            call_method_str(conn, (base + "/remotecommands").c_str(),
                            kRunCmdIface, "triggerCommand", k);
        for (const auto& md : medias) {
            const std::string mp = base + "/mprisremote";
            if (md.kind == "action")
                call_method_str(conn, mp.c_str(), kMprisIface, "sendAction", md.arg);
            else if (md.kind == "volume")
                call_method_int(conn, mp.c_str(), kMprisIface, "setVolume",
                                std::atoi(md.arg.c_str()));
            else if (md.kind == "player")
                set_str_prop(conn, mp.c_str(), kMprisIface, "player", md.arg);
        }
        for (const auto& s : smses)
            call_send_sms(conn, (base + "/sms").c_str(), s.addr, s.msg);
        if (want_mute)
            call_method_void(conn, (base + "/telephony").c_str(),
                             kTelephonyIface, "muteRinger");
        dbus_connection_flush(conn);
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
        }

        // Faster cadence: battery / media / connectivity / command list. Battery
        // also arrives via the refreshed signal below, so this is a backstop.
        if (now >= next_fast) {
            next_fast = now + fast_interval;
            poll_battery();
            poll_media();
            poll_connectivity();
            poll_devices();
            fetch_commands();
            if (!current_dev_id.empty() && seeded_dev != current_dev_id) {
                seed_active();
                seeded_dev = current_dev_id;
            } else if (current_dev_id.empty()) {
                seeded_dev.clear();
            }
        }

        // Drain queued TX actions (reply/dismiss/runcommand/media/sms/mute).
        drain_tx();

        // Ring-my-phone request (from the menu) — fire the findmyphone plugin on
        // the worker thread so the DBus connection stays single-owner. If no
        // device is bound yet, force discovery and keep the request pending for a
        // few seconds so a phone that just reconnected still rings.
        if (ring_request_.load()) {
            if (current_dev_id.empty()) {
                next_discovery = now;                 // re-scan ASAP next loop
                if (++ring_attempts_ > 15) {          // ~3 s of 200 ms loops — give up
                    ring_request_.store(false);
                    ring_attempts_ = 0;
                    std::fprintf(stderr, "[kdeconnect] ring: no reachable device\n");
                }
            } else {
                const std::string obj =
                    "/modules/kdeconnect/devices/" + current_dev_id + "/findmyphone";
                DBusMessage* msg = dbus_message_new_method_call(
                    kKdeService, obj.c_str(), kFindMyPhoneIface, "ring");
                if (msg) {
                    dbus_connection_send(conn, msg, nullptr);
                    dbus_connection_flush(conn);
                    dbus_message_unref(msg);
                    std::fprintf(stderr, "[kdeconnect] ring sent\n");
                }
                ring_request_.store(false);
                ring_attempts_ = 0;
            }
        }

        // Post queued ping notifications (ping plugin → tappable notification on
        // the phone, nothing auto-opens).
        {
            std::vector<std::string> pings;
            { std::lock_guard<std::mutex> lk(share_mtx_); pings.swap(ping_queue_); }
            if (!pings.empty() && !current_dev_id.empty()) {
                const std::string obj =
                    "/modules/kdeconnect/devices/" + current_dev_id + "/ping";
                for (const auto& m : pings) {
                    DBusMessage* msg = dbus_message_new_method_call(
                        kKdeService, obj.c_str(), kPingIface, "sendPing");
                    if (!msg) continue;
                    msg_append_str(msg, m.c_str());
                    dbus_connection_send(conn, msg, nullptr);
                    dbus_message_unref(msg);
                }
                dbus_connection_flush(conn);
                std::fprintf(stderr, "[kdeconnect] sent %zu ping(s)\n", pings.size());
            } else if (!pings.empty()) {
                std::fprintf(stderr, "[kdeconnect] ping: no reachable device, dropped %zu\n",
                             pings.size());
            }
        }

        // Share queued files via the Share plugin (shareUrl with a file:// URL).
        {
            std::vector<std::string> pending;
            { std::lock_guard<std::mutex> lk(share_mtx_); pending.swap(share_queue_); }
            if (!pending.empty() && !current_dev_id.empty()) {
                const std::string obj =
                    "/modules/kdeconnect/devices/" + current_dev_id + "/share";
                for (const auto& url : pending) {   // each entry is a ready URL
                    DBusMessage* msg = dbus_message_new_method_call(
                        kKdeService, obj.c_str(), kShareIface, "shareUrl");
                    if (!msg) continue;
                    msg_append_str(msg, url.c_str());
                    dbus_connection_send(conn, msg, nullptr);
                    dbus_message_unref(msg);
                }
                dbus_connection_flush(conn);
                std::fprintf(stderr, "[kdeconnect] shared %zu file(s)\n", pending.size());
            } else if (!pending.empty()) {
                std::fprintf(stderr,
                    "[kdeconnect] share: no reachable device, dropped %zu file(s)\n",
                    pending.size());
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
            } else if (dbus_message_is_signal(m, kBatteryIface, "refreshed")) {
                // Push battery: refreshed(bool isCharging, int charge).
                dbus_bool_t charging = FALSE; dbus_int32_t charge = -1;
                DBusError e; dbus_error_init(&e);
                if (dbus_message_get_args(m, &e,
                                          DBUS_TYPE_BOOLEAN, &charging,
                                          DBUS_TYPE_INT32,   &charge,
                                          DBUS_TYPE_INVALID)) {
                    poll_battery();   // re-read so the low-battery alert logic runs
                }
                if (dbus_error_is_set(&e)) dbus_error_free(&e);
            }
            dbus_message_unref(m);
        }
    }

    drop_match(current_match_rule);
    drop_match(current_batt_rule);
}

} // namespace integrations

#pragma once
#include "../app_state.h"
#include <atomic>
#include <string>
#include <thread>

// Polls SSID (ioctl SIOCGIWESSID), signal strength (/proc/net/wireless),
// and IP address (getifaddrs) for a given wireless interface every 5 s.
// Results are written to AppState::wifi and AppState::health.wifi_ok.

class WifiMonitor {
public:
    WifiMonitor()  = default;
    ~WifiMonitor() { stop(); }

    void start(AppState* state, const std::string& iface = "wlan0");
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();

    AppState*         state_  = nullptr;
    std::string       iface_;
    std::thread       thread_;
    std::atomic<bool> running_ { false };
};

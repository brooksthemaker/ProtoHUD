#pragma once
#include "../app_state.h"
#include <atomic>
#include <string>
#include <thread>

// Pings a configured host every 2 s using popen("ping -c1 -W1 ...").
// Results are written to AppState::ping (latency_ms, reachable, history).

class PingMonitor {
public:
    PingMonitor()  = default;
    ~PingMonitor() { stop(); }

    // host: IPv4 address or hostname to ping (e.g. "192.168.1.1")
    void start(AppState* state, const std::string& host);
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();

    AppState*         state_  = nullptr;
    std::string       host_;
    std::thread       thread_;
    std::atomic<bool> running_ { false };
};

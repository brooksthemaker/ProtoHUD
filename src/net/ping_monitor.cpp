#include "ping_monitor.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

void PingMonitor::start(AppState* state, const std::string& host) {
    state_   = state;
    host_    = host;
    running_ = true;
    // Store the configured host in state so the panel can display it.
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        state_->ping.host = host;
    }
    thread_ = std::thread(&PingMonitor::thread_fn, this);
}

void PingMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// Run one ping and return latency in ms, or -1 if unreachable/error.
static float ping_once(const std::string& host) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "ping -c1 -W1 %s 2>/dev/null | grep 'time='", host.c_str());
    FILE* fp = popen(cmd, "r");
    if (!fp) return -1.f;
    char buf[256] = {};
    char* line = fgets(buf, sizeof(buf), fp);
    pclose(fp);
    if (!line) return -1.f;
    // Parse "time=X.X ms" (also handles "time=X ms")
    const char* p = strstr(buf, "time=");
    if (!p) return -1.f;
    float ms = 0.f;
    if (sscanf(p, "time=%f", &ms) == 1) return ms;
    return -1.f;
}

void PingMonitor::thread_fn() {
    while (running_) {
        float ms = host_.empty() ? -1.f : ping_once(host_);
        bool  ok = ms >= 0.f;

        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            auto& p = state_->ping;
            p.reachable  = ok;
            p.latency_ms = ok ? ms : 0.f;
            int h = (p.history_head + 1) % kPingHistLen;
            p.history[h]  = ok ? ms : 0.f;
            p.history_head = h;
        }

        // Sleep 2 s in 100 ms chunks so stop() is responsive.
        for (int i = 0; i < 20 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

#pragma once
#include "../app_state.h"
#include <atomic>
#include <thread>

// Polls /proc/stat (CPU), /proc/meminfo (RAM), and /proc/uptime every second.
// Results are written to AppState::sys_metrics under the state mutex.

class SystemMonitor {
public:
    SystemMonitor()  = default;
    ~SystemMonitor() { stop(); }

    void start(AppState* state);
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();

    AppState*          state_    = nullptr;
    std::thread        thread_;
    std::atomic<bool>  running_  { false };
    uint64_t           prev_total_ = 0;
    uint64_t           prev_idle_  = 0;
};

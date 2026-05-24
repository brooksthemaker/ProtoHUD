#pragma once
#include "../app_state.h"
#include <atomic>
#include <thread>

// Polls /proc/stat (aggregate + per-core CPU), /proc/meminfo (RAM),
// /proc/uptime, the thermal sysfs (CPU temp) every second, and the VideoCore
// GPU clocks/temperature via vcgencmd every two seconds. Results are written to
// AppState::sys_metrics and AppState::gpu under the state mutex.

class SystemMonitor {
public:
    SystemMonitor()  = default;
    ~SystemMonitor() { stop(); }

    void start(AppState* state);
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();
    void poll_gpu();   // vcgencmd clocks + temp → AppState::gpu

    AppState*          state_    = nullptr;
    std::thread        thread_;
    std::atomic<bool>  running_  { false };
    uint64_t           prev_total_ = 0;
    uint64_t           prev_idle_  = 0;
    // Per-core jiffy baselines (index 0 = cpu0, …)
    uint64_t           prev_core_total_[kMaxCpuCores] = {};
    uint64_t           prev_core_idle_ [kMaxCpuCores] = {};
};

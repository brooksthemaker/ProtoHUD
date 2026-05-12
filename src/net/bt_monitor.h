#pragma once
#include "../app_state.h"
#include <atomic>
#include <thread>

// Polls paired/connected Bluetooth devices via bluetoothctl every 10 s.
// Results are written to AppState::bt_devices under the state mutex.
// Call refresh() to trigger an immediate rescan (e.g. from a menu action).

class BtMonitor {
public:
    BtMonitor()  = default;
    ~BtMonitor() { stop(); }

    void start(AppState* state);
    void stop();
    void refresh() { want_refresh_ = true; }
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();
    static std::vector<BtDevice> scan_devices();

    AppState*          state_       = nullptr;
    std::thread        thread_;
    std::atomic<bool>  running_     { false };
    std::atomic<bool>  want_refresh_{ false };
};

#include "bt_monitor.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

void BtMonitor::start(AppState* state) {
    state_   = state;
    running_ = true;
    thread_  = std::thread(&BtMonitor::thread_fn, this);
}

void BtMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// Parse "Device XX:XX:XX:XX:XX:XX Name" lines from bluetoothctl output.
static std::vector<BtDevice> parse_devices(FILE* fp, bool connected) {
    std::vector<BtDevice> out;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // Format: "Device AA:BB:CC:DD:EE:FF Some Name\n"
        char mac[32] = {};
        if (sscanf(line, " Device %31s", mac) != 1) continue;
        // Name follows the MAC — find it after the MAC token
        const char* mac_end = strstr(line, mac);
        if (!mac_end) continue;
        const char* name_start = mac_end + strlen(mac);
        while (*name_start == ' ') ++name_start;
        // Strip trailing newline
        std::string name(name_start);
        if (!name.empty() && name.back() == '\n') name.pop_back();
        BtDevice d;
        d.mac       = mac;
        d.name      = name.empty() ? mac : name;
        d.connected = connected;
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<BtDevice> BtMonitor::scan_devices() {
    std::vector<BtDevice> all;

    // Connected devices
    FILE* fp = popen("bluetoothctl devices Connected 2>/dev/null", "r");
    if (fp) {
        auto connected = parse_devices(fp, true);
        pclose(fp);
        all.insert(all.end(), connected.begin(), connected.end());
    }

    // Paired-but-not-connected devices (de-duplicate against already-connected MACs)
    fp = popen("bluetoothctl devices Paired 2>/dev/null", "r");
    if (fp) {
        auto paired = parse_devices(fp, false);
        pclose(fp);
        for (auto& p : paired) {
            bool dup = false;
            for (const auto& a : all) if (a.mac == p.mac) { dup = true; break; }
            if (!dup) all.push_back(std::move(p));
        }
    }

    return all;
}

void BtMonitor::thread_fn() {
    // Poll immediately on startup, then every 10 s.
    want_refresh_ = true;
    int ticks = 0;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ticks++;

        bool do_scan = want_refresh_.exchange(false) || (ticks >= 50); // 50×200ms = 10s
        if (!do_scan) continue;
        ticks = 0;

        auto devices = scan_devices();
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->bt_devices = std::move(devices);
        }
    }
}

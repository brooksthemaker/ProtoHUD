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

// Parse "Device XX:XX:XX:XX:XX:XX Name" lines from bluetoothctl output. Flags
// (connected/paired) are applied by the caller during the merge.
static std::vector<BtDevice> parse_devices(FILE* fp) {
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
        d.mac  = mac;
        d.name = name.empty() ? mac : name;
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<BtDevice> BtMonitor::scan_devices() {
    std::vector<BtDevice> all;
    // Merge a bluetoothctl list into `all`, OR-ing the connected/paired flags
    // onto an existing entry (same MAC) or appending a fresh one.
    auto merge = [&](std::vector<BtDevice> list, bool set_conn, bool set_paired) {
        for (auto& d : list) {
            BtDevice* ex = nullptr;
            for (auto& a : all) if (a.mac == d.mac) { ex = &a; break; }
            if (ex) {
                if (set_conn)   ex->connected = true;
                if (set_paired) ex->paired    = true;
            } else {
                d.connected = set_conn;
                d.paired    = set_paired;
                all.push_back(std::move(d));
            }
        }
    };
    FILE* fp = popen("bluetoothctl devices Connected 2>/dev/null", "r");
    if (fp) { auto v = parse_devices(fp); pclose(fp); merge(std::move(v), true,  true);  }
    fp = popen("bluetoothctl devices Paired 2>/dev/null", "r");
    if (fp) { auto v = parse_devices(fp); pclose(fp); merge(std::move(v), false, true);  }
    // All known devices — includes ones just discovered by a scan; these stay
    // unpaired so the menu can offer to pair them (and the System panel hides
    // discovered-only entries).
    fp = popen("bluetoothctl devices 2>/dev/null", "r");
    if (fp) { auto v = parse_devices(fp); pclose(fp); merge(std::move(v), false, false); }
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

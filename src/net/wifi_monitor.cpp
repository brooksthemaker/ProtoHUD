#include "wifi_monitor.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/wireless.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

void WifiMonitor::start(AppState* state, const std::string& iface) {
    state_   = state;
    iface_   = iface;
    running_ = true;
    thread_  = std::thread(&WifiMonitor::thread_fn, this);
}

void WifiMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// Fetch SSID via ioctl SIOCGIWESSID. Returns empty string if not associated.
static std::string get_ssid(const char* iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    char essid[IW_ESSID_MAX_SIZE + 1] = {};
    struct iwreq req {};
    strncpy(req.ifr_name, iface, IFNAMSIZ - 1);
    req.u.essid.pointer = essid;
    req.u.essid.length  = IW_ESSID_MAX_SIZE;
    bool ok = ioctl(fd, SIOCGIWESSID, &req) >= 0 && req.u.essid.length > 0;
    close(fd);
    return ok ? std::string(essid, req.u.essid.length) : std::string{};
}

// Read signal level (dBm) from /proc/net/wireless for the given interface.
static int get_signal_dbm(const char* iface) {
    FILE* f = fopen("/proc/net/wireless", "r");
    if (!f) return -100;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        int status;
        float link, level, noise;
        if (sscanf(line, " %31[^:]: %d %f %f %f", name, &status, &link, &level, &noise) == 5) {
            if (strcmp(name, iface) == 0) { fclose(f); return static_cast<int>(level); }
        }
    }
    fclose(f);
    return -100;
}

// Get IPv4 address for the interface as a dotted-decimal string.
static std::string get_ip(const char* iface) {
    struct ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0) return {};
    std::string result;
    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, iface) != 0) continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            result = buf;
        break;
    }
    freeifaddrs(ifa_list);
    return result;
}

void WifiMonitor::thread_fn() {
    while (running_) {
        std::string ssid = get_ssid(iface_.c_str());
        bool connected   = !ssid.empty();
        int  sig         = connected ? get_signal_dbm(iface_.c_str()) : -100;
        std::string ip   = connected ? get_ip(iface_.c_str()) : std::string{};

        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->wifi.connected  = connected;
            state_->wifi.ssid       = ssid;
            state_->wifi.ip         = ip;
            state_->wifi.signal_dbm = sig;
            state_->health.wifi_ok  = connected;
        }

        for (int i = 0; i < 50 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

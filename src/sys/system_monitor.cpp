#include "system_monitor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/sysinfo.h>
#include <thread>

void SystemMonitor::start(AppState* state) {
    state_   = state;
    running_ = true;
    thread_  = std::thread(&SystemMonitor::thread_fn, this);
}

void SystemMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// Read first "cpu" line of /proc/stat → (total_jiffies, idle_jiffies)
static bool read_cpu_jiffies(uint64_t& total, uint64_t& idle) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;
    char label[8];
    uint64_t user, nice, system, id, iowait, irq, softirq, steal;
    int n = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                   label, &user, &nice, &system, &id, &iowait, &irq, &softirq, &steal);
    fclose(f);
    if (n < 9) return false;
    idle  = id + iowait;
    total = user + nice + system + id + iowait + irq + softirq + steal;
    return true;
}

// Parse /proc/meminfo for MemTotal and MemAvailable (kB).
static bool read_meminfo(float& total_mb, float& used_mb) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    uint64_t total_kb = 0, avail_kb = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        uint64_t val;
        if (sscanf(line, "MemTotal: %llu kB", &val) == 1)     total_kb = val;
        if (sscanf(line, "MemAvailable: %llu kB", &val) == 1) avail_kb = val;
        if (total_kb && avail_kb) break;
    }
    fclose(f);
    if (!total_kb) return false;
    total_mb = static_cast<float>(total_kb) / 1024.f;
    used_mb  = static_cast<float>(total_kb - avail_kb) / 1024.f;
    return true;
}

void SystemMonitor::thread_fn() {
    // Prime the CPU delta baseline before the first report.
    read_cpu_jiffies(prev_total_, prev_idle_);

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;

        // ── Uptime ────────────────────────────────────────────────────────────
        struct sysinfo si {};
        uint64_t uptime_s = (sysinfo(&si) == 0) ? static_cast<uint64_t>(si.uptime) : 0;

        // ── CPU % ─────────────────────────────────────────────────────────────
        uint64_t cur_total = 0, cur_idle = 0;
        float cpu_pct = 0.f;
        if (read_cpu_jiffies(cur_total, cur_idle)) {
            uint64_t dtotal = cur_total - prev_total_;
            uint64_t didle  = cur_idle  - prev_idle_;
            if (dtotal > 0)
                cpu_pct = 100.f * (1.f - static_cast<float>(didle) /
                                         static_cast<float>(dtotal));
            cpu_pct    = std::clamp(cpu_pct, 0.f, 100.f);
            prev_total_ = cur_total;
            prev_idle_  = cur_idle;
        }

        // ── RAM ───────────────────────────────────────────────────────────────
        float total_mb = 0.f, used_mb = 0.f;
        read_meminfo(total_mb, used_mb);

        // ── Write to AppState ─────────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            auto& m = state_->sys_metrics;
            m.uptime_s     = uptime_s;
            m.cpu_pct      = cpu_pct;
            m.ram_used_mb  = used_mb;
            m.ram_total_mb = total_mb;

            int h = (m.history_head + 1) % kSysHistLen;
            m.cpu_history[h] = cpu_pct;
            m.ram_history[h] = (total_mb > 0.f) ? (used_mb / total_mb * 100.f) : 0.f;
            m.history_head   = h;
        }
    }
}

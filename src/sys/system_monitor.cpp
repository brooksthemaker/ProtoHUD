#include "system_monitor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

// Parse /proc/stat: aggregate "cpu" line + per-core "cpuN" lines into
// (total_jiffies, idle_jiffies). Returns the number of per-core entries filled.
static int read_cpu_jiffies(uint64_t& agg_total, uint64_t& agg_idle,
                            uint64_t core_total[], uint64_t core_idle[],
                            int max_cores) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int cores = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;  // cpu lines come first
        uint64_t user, nice, system, id, iowait, irq, softirq, steal;
        // Aggregate line: "cpu  …"; per-core: "cpuN …"
        if (line[3] == ' ') {
            if (sscanf(line + 3, "%llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &id, &iowait, &irq, &softirq, &steal) == 8) {
                agg_idle  = id + iowait;
                agg_total = user + nice + system + id + iowait + irq + softirq + steal;
            }
        } else {
            int idx = -1;
            if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                       &idx, &user, &nice, &system, &id, &iowait, &irq, &softirq, &steal) == 9
                && idx >= 0 && idx < max_cores) {
                core_idle [idx] = id + iowait;
                core_total[idx] = user + nice + system + id + iowait + irq + softirq + steal;
                if (idx + 1 > cores) cores = idx + 1;
            }
        }
    }
    fclose(f);
    return cores;
}

// CPU package temperature in °C from the thermal sysfs (0 if unavailable).
static float read_cpu_temp_c() {
    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return 0.f;
    long milli = 0;
    int n = fscanf(f, "%ld", &milli);
    fclose(f);
    return (n == 1) ? static_cast<float>(milli) / 1000.f : 0.f;
}

// Current frequency (MHz) of logical core `idx` (0 if unavailable).
static float read_core_mhz(int idx) {
    char path[96];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", idx);
    FILE* f = fopen(path, "r");
    if (!f) return 0.f;
    long khz = 0;
    int n = fscanf(f, "%ld", &khz);
    fclose(f);
    return (n == 1) ? static_cast<float>(khz) / 1000.f : 0.f;
}

// ── GPU helpers (vcgencmd — VideoCore on Raspberry Pi) ────────────────────────

// Run `vcgencmd <args>` and return the substring after the first '=' (or "").
static std::string vcgen(const char* args) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "vcgencmd %s 2>/dev/null", args);
    FILE* fp = popen(cmd, "r");
    if (!fp) return {};
    char line[96] = {};
    char* got = fgets(line, sizeof(line), fp);
    pclose(fp);
    if (!got) return {};
    const char* eq = strchr(line, '=');
    return eq ? std::string(eq + 1) : std::string();
}

static float vcgen_clock_mhz(const char* domain) {
    char arg[48];
    snprintf(arg, sizeof(arg), "measure_clock %s", domain);
    std::string v = vcgen(arg);
    if (v.empty()) return 0.f;
    return static_cast<float>(std::strtod(v.c_str(), nullptr) / 1e6);  // Hz → MHz
}

static float vcgen_temp_c() {
    std::string v = vcgen("measure_temp");   // "47.2'C"
    if (v.empty()) return 0.f;
    return static_cast<float>(std::strtod(v.c_str(), nullptr));
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
    // Prime the CPU delta baselines before the first report.
    read_cpu_jiffies(prev_total_, prev_idle_,
                     prev_core_total_, prev_core_idle_, kMaxCpuCores);

    // GPU metrics come from vcgencmd, which fork+execs a subprocess per query
    // (6 of them per poll) — keep that on a slow 10 s cadence while the cheap
    // /proc-based stats stay at 1 s. Start near the threshold so the first GPU
    // sample still lands a couple of seconds after boot.
    constexpr int kGpuPollPeriodS = 10;
    int gpu_tick = kGpuPollPeriodS - 2;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;

        // ── Uptime ────────────────────────────────────────────────────────────
        struct sysinfo si {};
        uint64_t uptime_s = (sysinfo(&si) == 0) ? static_cast<uint64_t>(si.uptime) : 0;

        // ── CPU % (aggregate + per-core) ──────────────────────────────────────
        uint64_t cur_total = 0, cur_idle = 0;
        uint64_t cur_core_total[kMaxCpuCores] = {};
        uint64_t cur_core_idle [kMaxCpuCores] = {};
        float cpu_pct = 0.f;
        float core_pct[kMaxCpuCores] = {};
        float core_mhz[kMaxCpuCores] = {};
        int   core_count = 0;

        int cores = read_cpu_jiffies(cur_total, cur_idle,
                                     cur_core_total, cur_core_idle, kMaxCpuCores);
        if (cores >= 0) {
            uint64_t dtotal = cur_total - prev_total_;
            uint64_t didle  = cur_idle  - prev_idle_;
            if (dtotal > 0)
                cpu_pct = 100.f * (1.f - static_cast<float>(didle) /
                                         static_cast<float>(dtotal));
            cpu_pct     = std::clamp(cpu_pct, 0.f, 100.f);
            prev_total_ = cur_total;
            prev_idle_  = cur_idle;

            core_count = std::min(cores, kMaxCpuCores);
            for (int i = 0; i < core_count; ++i) {
                uint64_t dt = cur_core_total[i] - prev_core_total_[i];
                uint64_t di = cur_core_idle [i] - prev_core_idle_[i];
                float p = (dt > 0) ? 100.f * (1.f - static_cast<float>(di) /
                                                    static_cast<float>(dt)) : 0.f;
                core_pct[i] = std::clamp(p, 0.f, 100.f);
                core_mhz[i] = read_core_mhz(i);
                prev_core_total_[i] = cur_core_total[i];
                prev_core_idle_[i]  = cur_core_idle[i];
            }
        }

        float cpu_temp = read_cpu_temp_c();

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
            m.cpu_core_count = core_count;
            m.cpu_temp_c     = cpu_temp;
            for (int i = 0; i < core_count; ++i) {
                m.cpu_core_pct[i] = core_pct[i];
                m.cpu_core_mhz[i] = core_mhz[i];
            }

            int h = (m.history_head + 1) % kSysHistLen;
            m.cpu_history[h] = cpu_pct;
            m.ram_history[h] = (total_mb > 0.f) ? (used_mb / total_mb * 100.f) : 0.f;
            m.history_head   = h;
        }

        // ── GPU (every 10 s — each vcgencmd spawns a subprocess) ──────────────
        if (++gpu_tick >= kGpuPollPeriodS) {
            gpu_tick = 0;
            poll_gpu();
        }
    }
}

void SystemMonitor::poll_gpu() {
    // Functional clock domains exposed by the VideoCore. Only those reporting a
    // non-zero frequency are kept (e.g. h264/hevc are absent on some SoCs).
    static const char* kDomains[] = { "core", "v3d", "isp", "h264", "hevc" };

    GpuMetrics g;
    g.temp_c = vcgen_temp_c();
    for (const char* d : kDomains) {
        if (g.clock_count >= kMaxGpuClocks) break;
        float mhz = vcgen_clock_mhz(d);
        if (mhz <= 0.f) continue;
        GpuClock& c = g.clocks[g.clock_count++];
        snprintf(c.name, sizeof(c.name), "%s", d);
        c.mhz = mhz;
    }
    g.available = (g.clock_count > 0) || (g.temp_c > 0.f);

    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        state_->gpu = g;
    }
}

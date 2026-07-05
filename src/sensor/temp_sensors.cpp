#include "temp_sensors.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "../app_state.h"

namespace sensor {

namespace fs = std::filesystem;

namespace {
constexpr const char* kW1Root = "/sys/bus/w1/devices";
}

TempSensors::TempSensors(TempSensorsConfig cfg, AppState& state)
    : cfg_(std::move(cfg)), state_(state) {}

TempSensors::~TempSensors() { stop(); }

std::vector<std::string> TempSensors::discover_ds18b20() {
    std::vector<std::string> ids;
    std::error_code ec;
    if (!fs::is_directory(kW1Root, ec)) return ids;
    for (const auto& e : fs::directory_iterator(kW1Root, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("28-", 0) == 0) ids.push_back(name);   // DS18B20 family code
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

double TempSensors::read_ds18b20(const std::string& id) const {
    // Resolve the device folder: an explicit id, else the first probe on the bus.
    std::string dev = id;
    if (dev.empty()) {
        const auto found = discover_ds18b20();
        if (found.empty()) return std::nan("");
        dev = found.front();
    }
    const fs::path base = fs::path(kW1Root) / dev;

    // Preferred: the plain milli-°C "temperature" file (newer kernels).
    {
        std::ifstream f(base / "temperature");
        long milli = 0;
        if (f && (f >> milli)) return static_cast<double>(milli) / 1000.0;
    }
    // Legacy: w1_slave — two lines, "... YES/NO" then "...  t=23456".
    {
        std::ifstream f(base / "w1_slave");
        std::string line;
        bool crc_ok = false;
        while (std::getline(f, line)) {
            if (line.find("YES") != std::string::npos) crc_ok = true;
            const auto p = line.find("t=");
            if (p != std::string::npos) {
                if (!crc_ok) return std::nan("");
                return std::stod(line.substr(p + 2)) / 1000.0;
            }
        }
    }
    return std::nan("");
}

void TempSensors::set_coproc_reader(CoprocRead fn) {
    std::lock_guard<std::mutex> lk(reader_mtx_);
    coproc_read_ = std::move(fn);
}

double TempSensors::read_one(const TempSensorCfg& s) const {
    if (s.type == "ds18b20") return read_ds18b20(s.id);
    if (s.type == "coproc") {
        // Probes on the RP2350 coprocessor: reads come from the reader main
        // wires in once the link exists (copy the fn under the lock so a
        // reload can swap it while we poll).
        CoprocRead fn;
        {
            std::lock_guard<std::mutex> lk(reader_mtx_);
            fn = coproc_read_;
        }
        double c = 0.0;
        if (fn && fn(s.id, c)) return c;
        return std::nan("");
    }
    // max31865 (PT100 via IIO) / i2c chips slot in here later.
    return std::nan("");
}

bool TempSensors::start() {
    if (!cfg_.enabled || cfg_.sensors.empty()) return false;
    running_.store(true);
    thread_ = std::thread(&TempSensors::loop, this);
    return true;
}

void TempSensors::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void TempSensors::loop() {
    const auto period = std::chrono::milliseconds(std::max(100, cfg_.poll_ms));
    while (running_.load()) {
        std::vector<TempReading> out;
        out.reserve(cfg_.sensors.size());
        for (const auto& s : cfg_.sensors) {
            const double c = read_one(s);
            TempReading r;
            r.label = s.label;
            r.ok    = !std::isnan(c);
            r.c     = r.ok ? c : 0.0;
            r.warn  = r.ok && c >= s.warn_c;
            r.crit  = r.ok && c >= s.crit_c;
            out.push_back(std::move(r));
        }
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.temps = std::move(out);
        }
        // Sleep in small slices so stop() is responsive.
        for (auto slept = std::chrono::milliseconds(0);
             slept < period && running_.load();
             slept += std::chrono::milliseconds(50))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace sensor

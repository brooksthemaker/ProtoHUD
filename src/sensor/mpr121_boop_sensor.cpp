#include "mpr121_boop_sensor.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sensor {

namespace {
// ── MPR121 register map (subset we use) ──────────────────────────────────────
constexpr uint8_t REG_TOUCH_STATUS_L = 0x00;
constexpr uint8_t REG_MHD_R          = 0x2B;
constexpr uint8_t REG_NHD_R          = 0x2C;
constexpr uint8_t REG_NCL_R          = 0x2D;
constexpr uint8_t REG_FDL_R          = 0x2E;
constexpr uint8_t REG_MHD_F          = 0x2F;
constexpr uint8_t REG_NHD_F          = 0x30;
constexpr uint8_t REG_NCL_F          = 0x31;
constexpr uint8_t REG_FDL_F          = 0x32;
constexpr uint8_t REG_TOUCH_TH_E0    = 0x41;   // +2*N for electrode N
constexpr uint8_t REG_RELEASE_TH_E0  = 0x42;
constexpr uint8_t REG_DEBOUNCE       = 0x5B;
constexpr uint8_t REG_CONFIG1        = 0x5C;
constexpr uint8_t REG_CONFIG2        = 0x5D;
constexpr uint8_t REG_ECR            = 0x5E;
constexpr uint8_t REG_SOFT_RESET     = 0x80;

constexpr uint8_t ECR_RUN_12E        = 0x8F;   // CL=10b (baseline tracking on) + 12 electrodes
constexpr uint8_t ECR_STOP           = 0x00;
constexpr uint8_t SOFT_RESET_MAGIC   = 0x63;
} // namespace

Mpr121BoopSensor::Mpr121BoopSensor(Config cfg) : cfg_(std::move(cfg)) {
    auto now = std::chrono::steady_clock::now();
    for (auto& t : last_boop_t_) t = now;
}

Mpr121BoopSensor::~Mpr121BoopSensor() { stop(); }

// ── I²C primitives (caller holds bus_mtx_) ───────────────────────────────────

bool Mpr121BoopSensor::write_reg_locked(uint8_t reg, uint8_t val) {
    if (i2c_fd_ < 0) return false;
    if (ioctl(i2c_fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) return false;
    uint8_t buf[2] = { reg, val };
    return ::write(i2c_fd_, buf, 2) == 2;
}

bool Mpr121BoopSensor::read_regs_locked(uint8_t reg, uint8_t* buf, size_t len) {
    if (i2c_fd_ < 0) return false;
    if (ioctl(i2c_fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) return false;
    if (::write(i2c_fd_, &reg, 1) != 1) return false;
    return ::read(i2c_fd_, buf, len) == static_cast<ssize_t>(len);
}

bool Mpr121BoopSensor::open_bus() {
    i2c_fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (i2c_fd_ < 0) {
        std::fprintf(stderr, "[boop] cannot open %s: %s\n",
                     cfg_.i2c_bus.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

void Mpr121BoopSensor::apply_zone_threshold_locked(uint8_t zi) {
    if (zi >= ZoneCount) return;
    const int8_t e = cfg_.electrode[zi];
    if (e < 0 || e > 11) return;
    write_reg_locked(REG_TOUCH_TH_E0   + 2 * e, cfg_.touch_threshold[zi]);
    write_reg_locked(REG_RELEASE_TH_E0 + 2 * e, cfg_.release_threshold[zi]);
}

bool Mpr121BoopSensor::init_chip_locked() {
    // Soft reset and wait for the chip to come back up.
    if (!write_reg_locked(REG_SOFT_RESET, SOFT_RESET_MAGIC)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Must be in stop mode (ECR = 0) to write configuration.
    if (!write_reg_locked(REG_ECR, ECR_STOP)) return false;

    // Rising-edge baseline filter — standard MPR121 cookbook values that work
    // well for skin touch through a few mm of non-conductive material.
    write_reg_locked(REG_MHD_R, 0x01);
    write_reg_locked(REG_NHD_R, 0x01);
    write_reg_locked(REG_NCL_R, 0x0E);
    write_reg_locked(REG_FDL_R, 0x00);

    // Falling-edge baseline filter (slower — guards against drift).
    write_reg_locked(REG_MHD_F, 0x01);
    write_reg_locked(REG_NHD_F, 0x05);
    write_reg_locked(REG_NCL_F, 0x01);
    write_reg_locked(REG_FDL_F, 0x00);

    // Per-electrode touch / release thresholds (only zones we care about; the
    // rest stay at chip defaults / unused).
    for (uint8_t zi = 0; zi < ZoneCount; ++zi)
        apply_zone_threshold_locked(zi);

    // Debounce: 2 consecutive readings to confirm touch/release.
    write_reg_locked(REG_DEBOUNCE, (2 << 4) | 2);

    // AFE config — CDC = 16 µA, CDT = 0.5 µs, ESI = 16 ms (default-ish).
    write_reg_locked(REG_CONFIG1, 0x10);
    write_reg_locked(REG_CONFIG2, 0x20);

    // Enter run mode with all 12 electrodes enabled + auto baseline tracking.
    return write_reg_locked(REG_ECR, ECR_RUN_12E);
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool Mpr121BoopSensor::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    {
        std::lock_guard<std::mutex> lk(bus_mtx_);
        if (!open_bus())        return false;
        if (!init_chip_locked()) {
            std::fprintf(stderr, "[boop] MPR121 init failed at 0x%02x on %s\n",
                         cfg_.i2c_addr, cfg_.i2c_bus.c_str());
            ::close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }
    }
    running_.store(true);
    thread_ = std::thread(&Mpr121BoopSensor::poll_loop, this);
    return true;
}

void Mpr121BoopSensor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(bus_mtx_);
    if (i2c_fd_ >= 0) {
        // Put the chip in stop mode on shutdown so it doesn't keep updating
        // baselines uselessly.
        write_reg_locked(REG_ECR, ECR_STOP);
        ::close(i2c_fd_);
        i2c_fd_ = -1;
    }
}

void Mpr121BoopSensor::set_zone_enabled(Zone z, bool enabled) {
    const auto zi = static_cast<uint8_t>(z);
    if (zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(bus_mtx_);
    cfg_.zone_enabled[zi] = enabled;
}

void Mpr121BoopSensor::set_zone_threshold(Zone z, uint8_t touch) {
    const auto zi = static_cast<uint8_t>(z);
    if (zi >= ZoneCount) return;
    // Keep release_threshold at roughly half of touch for sensible hysteresis.
    std::lock_guard<std::mutex> lk(bus_mtx_);
    cfg_.touch_threshold[zi]   = touch;
    cfg_.release_threshold[zi] = static_cast<uint8_t>(std::max(2, touch / 2));
    apply_zone_threshold_locked(zi);
}

// ── Poll loop ────────────────────────────────────────────────────────────────

void Mpr121BoopSensor::poll_loop() {
    const auto period = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(1.0 / std::max(1.0, cfg_.poll_hz)));

    while (running_.load()) {
        const auto next_t = std::chrono::steady_clock::now() + period;

        uint16_t touched = 0;
        {
            std::lock_guard<std::mutex> lk(bus_mtx_);
            uint8_t st[2] = { 0, 0 };
            if (read_regs_locked(REG_TOUCH_STATUS_L, st, 2))
                touched = static_cast<uint16_t>(st[0]) |
                          (static_cast<uint16_t>(st[1] & 0x1F) << 8);
        }

        const auto now = std::chrono::steady_clock::now();
        for (uint8_t zi = 0; zi < ZoneCount; ++zi) {
            // Snapshot zone config under the lock — cheap, avoids torn reads
            // when the menu updates thresholds mid-loop.
            bool   enabled;
            int8_t electrode;
            {
                std::lock_guard<std::mutex> lk(bus_mtx_);
                enabled   = cfg_.zone_enabled[zi];
                electrode = cfg_.electrode[zi];
            }
            if (!enabled || electrode < 0 || electrode > 11) {
                last_touched_[zi] = false;
                continue;
            }

            const bool is_touched = (touched >> electrode) & 1u;
            const bool rising     = is_touched && !last_touched_[zi];
            last_touched_[zi]     = is_touched;

            if (!rising) continue;

            // Refractory: ignore retriggers inside the cooldown window.
            const double dt_s = std::chrono::duration<double>(now - last_boop_t_[zi]).count();
            if (dt_s < cfg_.refractory_s) continue;
            last_boop_t_[zi] = now;

            if (on_boop_) on_boop_(static_cast<Zone>(zi));
        }

        std::this_thread::sleep_until(next_t);
    }
}

} // namespace sensor

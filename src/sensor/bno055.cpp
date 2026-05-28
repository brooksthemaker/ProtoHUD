#include "bno055.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
// ── BNO055 register map (page 0 only — we never touch page 1) ────────────────
constexpr uint8_t REG_PAGE_ID     = 0x07;
constexpr uint8_t REG_ACC_DATA_X  = 0x08;   // 6 bytes accel (LSB/MSB)
constexpr uint8_t REG_MAG_DATA_X  = 0x0E;   // 6 bytes mag
constexpr uint8_t REG_GYR_DATA_X  = 0x14;   // 6 bytes gyro
constexpr uint8_t REG_EUL_HEADING = 0x1A;   // 6 bytes euler (h/r/p)
constexpr uint8_t REG_QUA_DATA    = 0x20;   // 8 bytes quaternion (w/x/y/z)
constexpr uint8_t REG_CALIB_STAT  = 0x35;
constexpr uint8_t REG_UNIT_SEL    = 0x3B;
constexpr uint8_t REG_OPR_MODE    = 0x3D;
constexpr uint8_t REG_PWR_MODE    = 0x3E;
constexpr uint8_t REG_SYS_TRIGGER = 0x3F;
constexpr uint8_t REG_CHIP_ID     = 0x00;

constexpr uint8_t CHIP_ID_BNO055  = 0xA0;
constexpr uint8_t OPR_MODE_CONFIG = 0x00;
constexpr uint8_t OPR_MODE_NDOF   = 0x0C;   // full 9-DOF fusion with mag
constexpr uint8_t PWR_MODE_NORMAL = 0x00;
constexpr uint8_t UNIT_SEL_DEG    = 0x00;   // m/s², deg, deg/s, Celsius

// Scale factors (datasheet §3.6.4).
constexpr float kEulerLsbPerDeg   = 16.0f;
constexpr float kAccelLsbPerG     = 100.0f; // m/s² → /9.80665 in g
constexpr float kAccelMsToG       = 1.0f / 9.80665f;
constexpr float kGyroLsbPerDps    = 16.0f;
constexpr float kMagLsbPerUt      = 16.0f;
constexpr float kQuatLsb          = 1.0f / 16384.0f;
} // namespace

Bno055::Bno055(const Config& cfg) : cfg_(cfg) {
    declination_deg_.store(cfg.declination_deg);
    heading_offset_ .store(cfg.heading_offset);
    heading_axes_   .store(cfg.heading_axes);
}

Bno055::~Bno055() { stop(); }

bool Bno055::write_reg(uint8_t reg, uint8_t val) {
    if (i2c_fd_ < 0) return false;
    if (ioctl(i2c_fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) return false;
    uint8_t buf[2] = { reg, val };
    return ::write(i2c_fd_, buf, 2) == 2;
}

bool Bno055::read_regs(uint8_t reg, uint8_t* buf, size_t len) {
    if (i2c_fd_ < 0) return false;
    if (ioctl(i2c_fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) return false;
    if (::write(i2c_fd_, &reg, 1) != 1) return false;
    return ::read(i2c_fd_, buf, len) == static_cast<ssize_t>(len);
}

bool Bno055::open_bus() {
    i2c_fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (i2c_fd_ < 0) {
        std::cerr << "[bno055] cannot open " << cfg_.i2c_bus
                  << ": " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool Bno055::init_chip_locked() {
    uint8_t id = 0;
    if (!read_regs(REG_CHIP_ID, &id, 1) || id != CHIP_ID_BNO055) {
        std::cerr << "[bno055] CHIP_ID mismatch at 0x" << std::hex
                  << cfg_.i2c_addr << " (got 0x" << static_cast<int>(id)
                  << ", want 0xA0)\n" << std::dec;
        return false;
    }
    // Drop to config mode so we can write the config registers.
    write_reg(REG_OPR_MODE, OPR_MODE_CONFIG);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Soft reset and wait. Datasheet says ≥650 ms before re-issuing I²C.
    write_reg(REG_SYS_TRIGGER, 0x20);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    // Re-check CHIP_ID — confirms the reset completed.
    if (!read_regs(REG_CHIP_ID, &id, 1) || id != CHIP_ID_BNO055) {
        std::cerr << "[bno055] device gone after reset\n";
        return false;
    }

    write_reg(REG_PWR_MODE,  PWR_MODE_NORMAL);
    write_reg(REG_PAGE_ID,   0x00);
    write_reg(REG_SYS_TRIGGER, 0x00);             // clear any external crystal bit
    write_reg(REG_UNIT_SEL,  UNIT_SEL_DEG);
    write_reg(REG_OPR_MODE,  OPR_MODE_NDOF);      // start fusion
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return true;
}

bool Bno055::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    if (!open_bus()) return false;
    if (!init_chip_locked()) {
        ::close(i2c_fd_);
        i2c_fd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&Bno055::poll_loop, this);
    return true;
}

void Bno055::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (i2c_fd_ >= 0) {
        write_reg(REG_OPR_MODE, OPR_MODE_CONFIG);
        ::close(i2c_fd_);
        i2c_fd_ = -1;
    }
}

void Bno055::poll_loop() {
    const auto period = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(1.0 / std::max(1.0f, cfg_.poll_hz)));

    auto le16 = [](const uint8_t* p) -> int16_t {
        return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                    (static_cast<uint16_t>(p[1]) << 8));
    };

    while (running_.load()) {
        const auto next_t = std::chrono::steady_clock::now() + period;

        // One bulk read covers everything we care about: 0x08..0x35 inclusive.
        // 46 bytes — well within the i2c-dev write/read buffer limits.
        uint8_t blk[REG_CALIB_STAT - REG_ACC_DATA_X + 1] = {0};
        if (!read_regs(REG_ACC_DATA_X, blk, sizeof(blk))) {
            std::this_thread::sleep_until(next_t);
            continue;
        }
        // Slice the block into typed values.
        const uint8_t* acc = blk + (REG_ACC_DATA_X  - REG_ACC_DATA_X);
        const uint8_t* mag = blk + (REG_MAG_DATA_X  - REG_ACC_DATA_X);
        const uint8_t* gyr = blk + (REG_GYR_DATA_X  - REG_ACC_DATA_X);
        const uint8_t* eul = blk + (REG_EUL_HEADING - REG_ACC_DATA_X);
        const uint8_t* qua = blk + (REG_QUA_DATA    - REG_ACC_DATA_X);
        const uint8_t  cal = blk[REG_CALIB_STAT     - REG_ACC_DATA_X];

        Sample s{};
        // Accel is reported in m/s² × 100 — convert to g for the readout.
        for (int i = 0; i < 3; ++i)
            s.accel_g[i]  = (le16(acc + 2 * i) / kAccelLsbPerG) * kAccelMsToG;
        for (int i = 0; i < 3; ++i)
            s.gyro_dps[i] = le16(gyr + 2 * i) / kGyroLsbPerDps;
        for (int i = 0; i < 3; ++i)
            s.mag_ut[i]   = le16(mag + 2 * i) / kMagLsbPerUt;
        for (int i = 0; i < 3; ++i)
            s.euler_deg[i] = le16(eul + 2 * i) / kEulerLsbPerDeg;
        s.quaternion[0] = le16(qua + 0) * kQuatLsb;
        s.quaternion[1] = le16(qua + 2) * kQuatLsb;
        s.quaternion[2] = le16(qua + 4) * kQuatLsb;
        s.quaternion[3] = le16(qua + 6) * kQuatLsb;
        s.calib_sys   = (cal >> 6) & 0x03;
        s.calib_gyro  = (cal >> 4) & 0x03;
        s.calib_accel = (cal >> 2) & 0x03;
        s.calib_mag   =  cal       & 0x03;

        // Push calibration into atomics so the menu/IMU debug window can read
        // them without taking a sample callback.
        calib_sys_  .store(s.calib_sys);
        calib_gyro_ .store(s.calib_gyro);
        calib_accel_.store(s.calib_accel);
        calib_mag_  .store(s.calib_mag);

        // Heading axes remap — same enum the Mpu9250 uses, just applied to
        // the pre-fused euler triplet here. 0 = use BNO's heading directly.
        const int axes = heading_axes_.load();
        float heading = s.euler_deg[0];   // BNO055 NDOF: heading is degrees, 0..360
        switch (axes) {
        case 1: heading =       360.0f - s.euler_deg[0]; break;   // mirror Z
        case 2: heading = std::fmod(s.euler_deg[1] + 360.0f, 360.0f); break; // roll-as-yaw
        case 3: heading = std::fmod(s.euler_deg[2] + 360.0f, 360.0f); break; // pitch-as-yaw
        case 4: heading = std::fmod(360.0f - s.euler_deg[1] + 360.f, 360.0f); break;
        case 5: heading = std::fmod(360.0f - s.euler_deg[2] + 360.f, 360.0f); break;
        case 0:
        default: break;
        }
        heading += declination_deg_.load() + heading_offset_.load();
        // Wrap to [0, 360).
        heading = std::fmod(heading, 360.0f);
        if (heading < 0.0f) heading += 360.0f;

        if (cb_)      cb_(heading);
        if (samp_cb_) samp_cb_(s);

        std::this_thread::sleep_until(next_t);
    }
}

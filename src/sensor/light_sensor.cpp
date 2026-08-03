#include "light_sensor.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sensor {

namespace {
// BH1750 opcodes (datasheet §6).
constexpr uint8_t BH1750_POWER_ON           = 0x01;
constexpr uint8_t BH1750_RESET              = 0x07;
constexpr uint8_t BH1750_CONT_HIGH_RES_MODE = 0x10;  // 1-lux resolution, 120 ms typ

// OPT3001 registers (datasheet §7.6). 16-bit registers, MSB first on the wire.
constexpr uint8_t OPT3001_REG_RESULT    = 0x00;
constexpr uint8_t OPT3001_REG_CONFIG    = 0x01;
constexpr uint8_t OPT3001_REG_DEVICE_ID = 0x7F;      // reads 0x3001
// Config word: automatic full-scale range (RN=1100), 100 ms conversions
// (CT=0) so the default 8 Hz poll always sees a fresh sample, continuous
// mode (M=11), INT latch left at its reset default (unused here).
constexpr uint16_t OPT3001_CONFIG_CONT_100MS = 0xC610;
constexpr uint16_t OPT3001_DEVICE_ID         = 0x3001;
} // namespace

LightSensor::LightSensor(const Config& cfg) : cfg_(cfg) {}

LightSensor::~LightSensor() { stop(); }

bool LightSensor::start() {
    if (running_.load()) return true;
    if (!cfg_.enabled) return false;

    fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::fprintf(stderr, "[light] open %s failed: %s\n",
                     cfg_.i2c_bus.c_str(), std::strerror(errno));
        return false;
    }
    if (::ioctl(fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) {
        std::fprintf(stderr, "[light] addressing 0x%02x failed: %s\n",
                     cfg_.i2c_addr, std::strerror(errno));
        ::close(fd_); fd_ = -1; return false;
    }

    bool ok = false;
    switch (cfg_.type) {
    case Type::Bh1750:  ok = init_bh1750();  break;
    case Type::Opt3001: ok = init_opt3001(); break;
    }
    if (!ok) {
        std::fprintf(stderr, "[light] init failed\n");
        ::close(fd_); fd_ = -1; return false;
    }

    running_.store(true);
    thr_ = std::thread(&LightSensor::run, this);
    return true;
}

void LightSensor::stop() {
    running_.store(false);
    if (thr_.joinable()) thr_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool LightSensor::init_bh1750() {
    // The datasheet's startup sequence is Power On → Reset → measurement
    // mode. Reset only clears the data register, not the mode, so we set
    // Continuous H-Res afterwards. write() with a single byte is enough —
    // BH1750 commands aren't register-addressed.
    const uint8_t pwr = BH1750_POWER_ON;
    const uint8_t rst = BH1750_RESET;
    const uint8_t mode = BH1750_CONT_HIGH_RES_MODE;
    if (::write(fd_, &pwr,  1) != 1) return false;
    if (::write(fd_, &rst,  1) != 1) return false;
    if (::write(fd_, &mode, 1) != 1) return false;
    // First conversion needs ~120 ms before the data register is valid.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return true;
}

bool LightSensor::read_bh1750(float& lux) {
    uint8_t buf[2] = {0, 0};
    if (::read(fd_, buf, 2) != 2) return false;
    const uint16_t raw = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    // Datasheet conversion: lux = raw / 1.2 in H-Res Mode (no sensitivity tweak).
    lux = static_cast<float>(raw) / 1.2f;
    return true;
}

bool LightSensor::init_opt3001() {
    // Sanity-check the device ID first — 0x44 is shared with nothing else in
    // this ecosystem, but a wrong-chip-at-the-right-address failure would
    // otherwise surface as nonsense lux values instead of a clean error.
    uint8_t reg = OPT3001_REG_DEVICE_ID;
    uint8_t buf[2] = {0, 0};
    if (::write(fd_, &reg, 1) != 1 || ::read(fd_, buf, 2) != 2) return false;
    const uint16_t id = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    if (id != OPT3001_DEVICE_ID) {
        std::fprintf(stderr, "[light] OPT3001 device id 0x%04x != 0x%04x\n",
                     id, OPT3001_DEVICE_ID);
        return false;
    }

    const uint8_t cfg[3] = { OPT3001_REG_CONFIG,
                             static_cast<uint8_t>(OPT3001_CONFIG_CONT_100MS >> 8),
                             static_cast<uint8_t>(OPT3001_CONFIG_CONT_100MS & 0xFF) };
    if (::write(fd_, cfg, 3) != 3) return false;

    // Park the register pointer on RESULT once — it doesn't auto-increment,
    // so every poll after this is a bare 2-byte read (same shape as BH1750).
    reg = OPT3001_REG_RESULT;
    if (::write(fd_, &reg, 1) != 1) return false;

    // First conversion completes ~100 ms after entering continuous mode.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return true;
}

bool LightSensor::read_opt3001(float& lux) {
    uint8_t buf[2] = {0, 0};
    if (::read(fd_, buf, 2) != 2) return false;
    // Result format (datasheet §7.6.1): E[15:12] exponent, R[11:0] mantissa;
    // lux = 0.01 × 2^E × R. Auto-range spans 0.01 to ~83k lux.
    const uint16_t raw = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    const uint16_t exp = raw >> 12;
    const uint16_t man = raw & 0x0FFF;
    lux = 0.01f * static_cast<float>(1u << exp) * static_cast<float>(man);
    return true;
}

void LightSensor::run() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::milliseconds(
        cfg_.poll_hz > 0.1f ? static_cast<int>(1000.f / cfg_.poll_hz) : 125);
    auto next = clock::now() + period;
    while (running_.load()) {
        float lux = 0.f;
        bool ok = false;
        switch (cfg_.type) {
        case Type::Bh1750:  ok = read_bh1750(lux);  break;
        case Type::Opt3001: ok = read_opt3001(lux); break;
        }
        if (ok) {
            latest_lux_.store(lux);
            if (cb_) cb_(lux);
        }
        std::this_thread::sleep_until(next);
        next += period;
    }
}

} // namespace sensor

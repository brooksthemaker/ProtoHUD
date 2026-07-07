#include "bme280.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sensor {
namespace {

// Bosch integer compensation (datasheet §4.2.3), verbatim except for the
// calibration words arriving as parameters. t_fine couples T→P/H.
struct Bme280Cal {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1, H3; int16_t H2, H4, H5; int8_t H6;
};

int32_t compensate_t(const Bme280Cal& c, int32_t adc_T, int32_t& t_fine) {
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)c.T1 << 1))) * ((int32_t)c.T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)c.T1)) *
                      ((adc_T >> 4) - ((int32_t)c.T1))) >> 12) * ((int32_t)c.T3)) >> 14;
    t_fine = var1 + var2;
    return (t_fine * 5 + 128) >> 8;                       // 0.01 °C
}

uint32_t compensate_p(const Bme280Cal& c, int32_t adc_P, int32_t t_fine) {
    int64_t var1 = ((int64_t)t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)c.P6;
    var2 = var2 + ((var1 * (int64_t)c.P5) << 17);
    var2 = var2 + (((int64_t)c.P4) << 35);
    var1 = ((var1 * var1 * (int64_t)c.P3) >> 8) + ((var1 * (int64_t)c.P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)c.P1) >> 33);
    if (var1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)c.P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)c.P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)c.P7) << 4);
    return (uint32_t)p;                                   // Pa, Q24.8
}

uint32_t compensate_h(const Bme280Cal& c, int32_t adc_H, int32_t t_fine) {
    int32_t v = t_fine - ((int32_t)76800);
    v = (((((adc_H << 14) - (((int32_t)c.H4) << 20) - (((int32_t)c.H5) * v)) +
           ((int32_t)16384)) >> 15) *
         (((((((v * ((int32_t)c.H6)) >> 10) *
              (((v * ((int32_t)c.H3)) >> 11) + ((int32_t)32768))) >> 10) +
            ((int32_t)2097152)) * ((int32_t)c.H2) + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)c.H1)) >> 4);
    v = std::clamp(v, 0, 419430400);
    return (uint32_t)(v >> 12);                           // %RH, Q22.10
}

}  // namespace

bool Bme280::wr(uint8_t r, uint8_t v) {
    uint8_t b[2] = { r, v };
    return ::write(fd_, b, 2) == 2;
}
int Bme280::rd(uint8_t r) {
    if (::write(fd_, &r, 1) != 1) return -1;
    uint8_t v = 0;
    if (::read(fd_, &v, 1) != 1) return -1;
    return v;
}
int Bme280::rd_block(uint8_t r, uint8_t* buf, int n) {
    if (::write(fd_, &r, 1) != 1) return -1;
    return static_cast<int>(::read(fd_, buf, n));
}

bool Bme280::init_chip() {
    const int id = rd(0xD0);
    if (id != 0x60) {                        // 0x58 would be a BMP280 (no humidity)
        fprintf(stderr, "[bme280] unexpected chip id 0x%02X on %s@0x%02X\n",
                id, cfg_.i2c_bus.c_str(), cfg_.i2c_addr);
        if (id < 0) return false;
    }
    uint8_t c[26];
    if (rd_block(0x88, c, 26) != 26) return false;        // T1..P9 + H1
    auto u16 = [&](int i){ return (uint16_t)(c[i] | (c[i+1] << 8)); };
    T1_ = u16(0);  T2_ = (int16_t)u16(2);  T3_ = (int16_t)u16(4);
    P1_ = u16(6);  P2_ = (int16_t)u16(8);  P3_ = (int16_t)u16(10);
    P4_ = (int16_t)u16(12); P5_ = (int16_t)u16(14); P6_ = (int16_t)u16(16);
    P7_ = (int16_t)u16(18); P8_ = (int16_t)u16(20); P9_ = (int16_t)u16(22);
    H1_ = c[25];
    uint8_t h[7];
    if (rd_block(0xE1, h, 7) != 7) return false;          // H2..H6
    H2_ = (int16_t)(h[0] | (h[1] << 8));
    H3_ = h[2];
    H4_ = (int16_t)((h[3] << 4) | (h[4] & 0x0F));         // 12-bit split registers
    H5_ = (int16_t)((h[5] << 4) | (h[4] >> 4));
    H6_ = (int8_t)h[6];
    // Oversampling ×1 everywhere, normal mode, 1000 ms standby, no filter.
    return wr(0xF2, 0x01) && wr(0xF5, 0xA0) && wr(0xF4, 0x27);
}

bool Bme280::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (fd_ < 0) { perror("[bme280] open i2c"); return false; }
    if (ioctl(fd_, I2C_SLAVE, cfg_.i2c_addr) < 0 || !init_chip()) {
        fprintf(stderr, "[bme280] init failed on %s@0x%02X\n",
                cfg_.i2c_bus.c_str(), cfg_.i2c_addr);
        ::close(fd_); fd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&Bme280::poll_loop, this);
    return true;
}

void Bme280::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void Bme280::poll_loop() {
    const Bme280Cal cal{ T1_, T2_, T3_, P1_, P2_, P3_, P4_, P5_,
                         P6_, P7_, P8_, P9_, H1_, H3_, H2_, H4_, H5_, H6_ };
    while (running_.load()) {
        uint8_t d[8];
        if (rd_block(0xF7, d, 8) == 8) {
            const int32_t adc_P = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
            const int32_t adc_T = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
            const int32_t adc_H = (d[6] << 8) | d[7];
            int32_t t_fine = 0;
            Reading r;
            r.temp_c       = compensate_t(cal, adc_T, t_fine) / 100.0f;
            r.pressure_hpa = compensate_p(cal, adc_P, t_fine) / 256.0f / 100.0f;
            r.humidity_pct = compensate_h(cal, adc_H, t_fine) / 1024.0f;
            if (cb_) cb_(r);
        }
        // Sleep in slices so stop() stays responsive at multi-second polls.
        double left = std::max(0.5, cfg_.poll_s);
        while (left > 0 && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            left -= 0.1;
        }
    }
}

}  // namespace sensor

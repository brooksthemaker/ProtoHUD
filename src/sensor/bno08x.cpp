// ── bno08x.cpp ────────────────────────────────────────────────────────────────
// BNO086 driver implementation. See bno08x.h. Uses the vendored CEVA SH-2 lib
// (vendor/sh2) over I2C, with INT/RST via the raw linux/gpio.h line-request ABI
// (the same ABI src/face/gpio_v2.h uses — no libgpiod dependency).

#include "bno08x.h"

extern "C" {
#include "sh2.h"
#include "sh2_SensorValue.h"
#include "sh2_err.h"
#include "euler.h"
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr float kRad2Deg = 57.2957795131f;

uint32_t steady_us() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// SH-2 HAL wrapper: sh2_Hal_t MUST be the first member so a self pointer can be
// cast back to recover the owning driver. SH-2 is process-global, so one static
// instance is enough (and matches the single-BNO08x constraint).
struct Bno08xHal {
    sh2_Hal_t hal;
    Bno08x*   owner;
};
Bno08xHal g_hal{};

int  hal_open_tramp (sh2_Hal_t* self)                                       { return reinterpret_cast<Bno08xHal*>(self)->owner->hal_open() ? 0 : -1; }
void hal_close_tramp(sh2_Hal_t* self)                                       { reinterpret_cast<Bno08xHal*>(self)->owner->hal_close(); }
int  hal_read_tramp (sh2_Hal_t* self, uint8_t* b, unsigned l, uint32_t* t)  { return reinterpret_cast<Bno08xHal*>(self)->owner->hal_read(b, l, t); }
int  hal_write_tramp(sh2_Hal_t* self, uint8_t* b, unsigned l)               { return reinterpret_cast<Bno08xHal*>(self)->owner->hal_write(b, l); }
uint32_t hal_time_tramp(sh2_Hal_t* self)                                    { (void)self; return steady_us(); }

void sensor_cb_tramp(void* cookie, sh2_SensorEvent_t* ev) { static_cast<Bno08x*>(cookie)->on_sensor_event(ev); }
void async_cb_tramp (void* cookie, sh2_AsyncEvent_t*  ev) { static_cast<Bno08x*>(cookie)->on_async_event(ev); }

// Claim a single GPIO line via the gpio_v2 line-request ABI. Returns the line
// request fd (>=0) or -1. flags = GPIO_V2_LINE_FLAG_{INPUT,OUTPUT}[|BIAS_*].
int request_line(const std::string& chip, int offset, uint64_t flags, const char* consumer) {
    if (offset < 0) return -1;
    int chip_fd = ::open(chip.c_str(), O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) return -1;
    gpio_v2_line_request req{};
    req.offsets[0]   = static_cast<uint32_t>(offset);
    req.num_lines    = 1;
    req.config.flags = flags;
    if (consumer) std::strncpy(req.consumer, consumer, sizeof(req.consumer) - 1);
    int r  = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    int fd = (r < 0) ? -1 : req.fd;
    ::close(chip_fd);
    return fd;
}

}  // namespace

Bno08x::Bno08x(const Config& cfg) : cfg_(cfg) {}
Bno08x::~Bno08x() { stop(); }

// ── GPIO (INT / RST) ──────────────────────────────────────────────────────────

bool Bno08x::gpio_open() {
    if (cfg_.rst_line >= 0) {
        rst_fd_ = request_line(cfg_.gpiochip, cfg_.rst_line,
                               GPIO_V2_LINE_FLAG_OUTPUT, "protohud-bno086-rst");
        if (rst_fd_ < 0) { fprintf(stderr, "[bno086] RST line request failed\n"); return false; }
    }
    if (cfg_.int_line >= 0) {
        int_fd_ = request_line(cfg_.gpiochip, cfg_.int_line,
                               GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP,
                               "protohud-bno086-int");
        if (int_fd_ < 0) { fprintf(stderr, "[bno086] INT line request failed\n"); return false; }
    }
    return true;
}

bool Bno08x::int_asserted() {
    if (int_fd_ < 0) return true;   // no INT wired → poll the bus every service tick
    gpio_v2_line_values v{};
    v.mask = 1;
    if (ioctl(int_fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return true;
    return (v.bits & 1ULL) == 0;    // INT is active-low: 0 = data ready
}

void Bno08x::rst_pulse() {
    if (rst_fd_ < 0) {              // no RST wired → just wait for the hub to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return;
    }
    gpio_v2_line_values v{};
    v.mask = 1;
    v.bits = 0; ioctl(rst_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);   // assert reset (low)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    v.bits = 1; ioctl(rst_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);   // release
    std::this_thread::sleep_for(std::chrono::milliseconds(200));     // boot time
}

// ── SH-2 HAL (I2C + reset) ────────────────────────────────────────────────────

bool Bno08x::hal_open() {
    i2c_fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (i2c_fd_ < 0) { perror("[bno086] open i2c"); return false; }
    if (ioctl(i2c_fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) { perror("[bno086] I2C_SLAVE"); return false; }
    if (!gpio_open()) return false;
    rst_pulse();   // put the hub in a known state before SHTP starts
    return true;
}

void Bno08x::hal_close() {
    if (i2c_fd_ >= 0) { ::close(i2c_fd_); i2c_fd_ = -1; }
    if (int_fd_ >= 0) { ::close(int_fd_); int_fd_ = -1; }
    if (rst_fd_ >= 0) { ::close(rst_fd_); rst_fd_ = -1; }
}

// SHTP-over-I2C read: peek the 4-byte header for the transfer length, then read
// the whole packet (the BNO08x returns header+cargo from the start on each read).
int Bno08x::hal_read(uint8_t* buf, unsigned len, uint32_t* t_us) {
    if (i2c_fd_ < 0) return 0;
    if (!int_asserted()) return 0;                       // nothing to read yet
    uint8_t header[4];
    if (::read(i2c_fd_, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)))
        return 0;
    unsigned pkt = (static_cast<unsigned>(header[0]) |
                    (static_cast<unsigned>(header[1]) << 8)) & 0x7FFFu;  // strip continuation bit
    if (pkt == 0) return 0;                              // header-only, no cargo
    if (pkt > len) pkt = len;                            // clamp to the SHTP buffer
    if (::read(i2c_fd_, buf, pkt) != static_cast<ssize_t>(pkt)) return 0;
    if (t_us) *t_us = steady_us();
    return static_cast<int>(pkt);
}

int Bno08x::hal_write(uint8_t* buf, unsigned len) {
    if (i2c_fd_ < 0) return 0;
    if (::write(i2c_fd_, buf, len) != static_cast<ssize_t>(len)) return 0;
    return static_cast<int>(len);
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

bool Bno08x::enable_reports() {
    sh2_SensorConfig_t sc{};
    sc.reportInterval_us = static_cast<uint32_t>(cfg_.report_interval_us);
    // ARVR-stabilized rotation vector: magnetometer-referenced (absolute heading,
    // no yaw drift) AND AR/VR-jitter-stabilized (good for head tracking).
    return sh2_setSensorConfig(SH2_ARVR_STABILIZED_RV, &sc) == SH2_OK;
}

bool Bno08x::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;

    declination_deg_.store(cfg_.declination_deg);
    head_tracking_.store(cfg_.head_tracking);

    g_hal.owner         = this;
    g_hal.hal.open      = hal_open_tramp;
    g_hal.hal.close     = hal_close_tramp;
    g_hal.hal.read      = hal_read_tramp;
    g_hal.hal.write     = hal_write_tramp;
    g_hal.hal.getTimeUs = hal_time_tramp;

    int rc = sh2_open(&g_hal.hal, async_cb_tramp, this);   // calls hal_open() (reset + I2C)
    if (rc != SH2_OK) {
        fprintf(stderr, "[bno086] sh2_open failed (rc=%d) on %s@0x%02X\n",
                rc, cfg_.i2c_bus.c_str(), cfg_.i2c_addr);
        hal_close();
        return false;
    }
    sh2_setSensorCallback(sensor_cb_tramp, this);
    if (!enable_reports())
        fprintf(stderr, "[bno086] warning: could not enable rotation-vector report\n");

    running_.store(true);
    ok_.store(true);
    thread_ = std::thread(&Bno08x::service_loop, this);
    return true;
}

void Bno08x::stop() {
    if (!running_.exchange(false)) { if (i2c_fd_ >= 0) { sh2_close(); } return; }
    if (thread_.joinable()) thread_.join();
    sh2_close();   // → hal_close()
    ok_.store(false);
}

bool Bno08x::restart() {
    stop();
    return start();
}

void Bno08x::service_loop() {
    while (running_.load()) {
        sh2_service();                       // reads via the HAL, dispatches callbacks
        if (want_reinit_.exchange(false))
            enable_reports();                // re-enable reports after a device reset
        int tare = tare_request_.exchange(0);
        if (tare) {
            const uint8_t axes = (tare == 2) ? (SH2_TARE_X | SH2_TARE_Y | SH2_TARE_Z)
                                             : SH2_TARE_Z;
            sh2_setTareNow(axes, SH2_TARE_BASIS_ROTATION_VECTOR);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ── event handling ────────────────────────────────────────────────────────────

void Bno08x::on_async_event(void* sh2_async_event) {
    auto* ev = static_cast<sh2_AsyncEvent_t*>(sh2_async_event);
    if (ev->eventId == SH2_RESET)
        want_reinit_.store(true);            // hub reset → re-request our reports
}

void Bno08x::on_sensor_event(void* sh2_sensor_event) {
    sh2_SensorValue_t val{};
    if (sh2_decodeSensorEvent(&val, static_cast<sh2_SensorEvent_t*>(sh2_sensor_event)) != SH2_OK)
        return;
    if (val.sensorId != SH2_ARVR_STABILIZED_RV)
        return;

    const float qw = val.un.arvrStabilizedRV.real;
    const float qx = val.un.arvrStabilizedRV.i;
    const float qy = val.un.arvrStabilizedRV.j;
    const float qz = val.un.arvrStabilizedRV.k;

    float roll, pitch, yaw;                  // radians (euler.c uses atan2/asin)
    q_to_ypr(qw, qx, qy, qz, &roll, &pitch, &yaw);
    const float roll_deg  = roll  * kRad2Deg;
    const float pitch_deg = pitch * kRad2Deg;
    const float yaw_deg   = yaw   * kRad2Deg;

    float heading = (cfg_.heading_invert ? -yaw_deg : yaw_deg)
                    + cfg_.heading_offset + declination_deg_.load();
    heading = std::fmod(heading, 360.f);
    if (heading < 0.f) heading += 360.f;

    Sample s;
    s.quaternion[0] = qw; s.quaternion[1] = qx; s.quaternion[2] = qy; s.quaternion[3] = qz;
    s.euler_deg[0]  = roll_deg; s.euler_deg[1] = pitch_deg; s.euler_deg[2] = yaw_deg;
    s.heading_deg   = heading;
    s.accuracy_deg  = val.un.arvrStabilizedRV.accuracy * kRad2Deg;
    s.status        = val.status;

    if (cb_)                                    cb_(heading);
    if (orient_cb_ && head_tracking_.load())    orient_cb_(roll_deg, pitch_deg, yaw_deg);
    if (samp_cb_)                               samp_cb_(s);
}

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
    const bool rv_ok = sh2_setSensorConfig(SH2_ARVR_STABILIZED_RV, &sc) == SH2_OK;

    // Calibrated accel / gyro / mag at the slower aux rate, for the IMU
    // readout and interference diagnosis (the mag report's status byte is the
    // magnetometer calibration quality the fused heading depends on). These
    // are best-effort — losing them costs debug rows, not the compass.
    sc.reportInterval_us = static_cast<uint32_t>(cfg_.aux_interval_us);
    if (sh2_setSensorConfig(SH2_ACCELEROMETER, &sc)             != SH2_OK ||
        sh2_setSensorConfig(SH2_GYROSCOPE_CALIBRATED, &sc)      != SH2_OK ||
        sh2_setSensorConfig(SH2_MAGNETIC_FIELD_CALIBRATED, &sc) != SH2_OK)
        fprintf(stderr, "[bno086] warning: could not enable accel/gyro/mag reports\n");

    // Auto-calibration: keep the chip's dynamic calibration running on all
    // three sensors and let it persist the refined calibration (DCD) to its
    // own flash periodically. The mag then walks itself up to s3 during
    // normal wear — no figure-8 ritual — and the result survives power
    // cycles. A final snapshot is also taken on clean shutdown.
    if (cfg_.auto_calibrate) {
        if (sh2_setCalConfig(SH2_CAL_ACCEL | SH2_CAL_GYRO | SH2_CAL_MAG) != SH2_OK)
            fprintf(stderr, "[bno086] warning: could not enable dynamic calibration\n");
        sh2_setDcdAutoSave(true);
    }

    return rv_ok;
}

bool Bno08x::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;

    declination_deg_.store(cfg_.declination_deg);
    roll_trim_.store(cfg_.roll_trim);
    pitch_trim_.store(cfg_.pitch_trim);
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
        if (tare == 3) {
            // Level (mounting calibration). TARE_NOW X|Y is silently ignored
            // by the firmware for rotation-vector bases (roll/pitch are
            // gravity-referenced) — the first cut of this did nothing. The
            // supported mechanism is the reorientation quaternion: pick R so
            // the CURRENT pose reads as yaw-only (out = R * raw), i.e.
            // R = yawonly(now) * conj(now).
            //
            // FIELD FIX: hardware showed roll/pitch flipping SIGN on each
            // press (+30 -> -30 -> +30 ...) instead of zeroing. That is the
            // signature of the firmware applying the commanded quaternion
            // TWICE (R*R*now = tilt mirrored, and the next press computes a
            // mirrored correction that flips it back — also why the water
            // effect swapped pooling sides on every tare). So command
            // sqrt(R) — half the rotation about the same axis — which lands
            // exactly level under double application, is idempotent on
            // repeat presses, and on a unit that applies it once still
            // converges by halving the error each press. Sim-verified with
            // the vendored euler.c (scratch harness level_half.c): 30 roll /
            // -20 pitch / 40 yaw -> 0 / 0 / 40 with the old code's double
            // application reproducing the observed sign flip.
            if (have_q_) {
                float yw, pt, rl;
                q_to_ypr(static_cast<float>(last_q_[0]), static_cast<float>(last_q_[1]),
                         static_cast<float>(last_q_[2]), static_cast<float>(last_q_[3]),
                         &yw, &pt, &rl);
                const double th = -static_cast<double>(yw);   // euler.c yaw = -theta_z
                const double c = std::cos(th * 0.5), s = std::sin(th * 0.5);
                // conj(now):
                const double w2 =  last_q_[0], x2 = -last_q_[1],
                             y2 = -last_q_[2], z2 = -last_q_[3];
                sh2_Quaternion_t R;                            // (c,0,0,s) * conj(now)
                R.w = c * w2 - s * z2;
                R.x = c * x2 - s * y2;
                R.y = c * y2 + s * x2;
                R.z = c * z2 + s * w2;
                // sqrt(R): normalize(R + identity) halves the rotation angle.
                // (R.w ~ -1 would be a 180 deg mount correction with no unique
                // half — fall back to R as-is rather than divide by ~0.)
                const double hw = R.w + 1.0;
                const double hn = std::sqrt(hw * hw + R.x * R.x + R.y * R.y + R.z * R.z);
                if (hn > 1e-6) {
                    R.w = hw / hn; R.x /= hn; R.y /= hn; R.z /= hn;
                }
                sh2_setReorientation(&R);
                sh2_persistTare();
            }
        } else if (tare) {
            const uint8_t axes = (tare == 2) ? (SH2_TARE_X | SH2_TARE_Y | SH2_TARE_Z)
                                             : SH2_TARE_Z;
            sh2_setTareNow(axes, SH2_TARE_BASIS_ROTATION_VECTOR);
        }
        // 3 ms poll ≈ 330 Hz — comfortably above the ~175 reports/s the chip
        // produces (100 Hz orientation + 3×25 Hz aux) while cutting the empty
        // SHTP header reads a 1 ms poll burned ~90% of its I2C traffic on.
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    // Snapshot the session's dynamic-calibration refinements to the chip's
    // flash on the way out (the periodic autosave catches most of it; this
    // catches the tail). Still on the service thread, link still open —
    // sh2 blocking ops self-pump SHTP with a timeout, so this can't hang.
    if (cfg_.auto_calibrate) sh2_saveDcdNow();
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

    // Aux reports (calibrated accel / gyro / mag) — forward and return.
    if (aux_cb_) {
        AuxSample a;
        a.status = val.status;
        switch (val.sensorId) {
        case SH2_ACCELEROMETER:
            a.kind = AuxSample::Kind::Accel;
            a.v[0] = val.un.accelerometer.x;
            a.v[1] = val.un.accelerometer.y;
            a.v[2] = val.un.accelerometer.z;
            aux_cb_(a);
            return;
        case SH2_GYROSCOPE_CALIBRATED:
            a.kind = AuxSample::Kind::Gyro;
            a.v[0] = val.un.gyroscope.x;
            a.v[1] = val.un.gyroscope.y;
            a.v[2] = val.un.gyroscope.z;
            aux_cb_(a);
            return;
        case SH2_MAGNETIC_FIELD_CALIBRATED:
            a.kind = AuxSample::Kind::Mag;
            a.v[0] = val.un.magneticField.x;
            a.v[1] = val.un.magneticField.y;
            a.v[2] = val.un.magneticField.z;
            aux_cb_(a);
            return;
        default: break;
        }
    }
    if (val.sensorId != SH2_ARVR_STABILIZED_RV)
        return;

    const float qw = val.un.arvrStabilizedRV.real;
    const float qx = val.un.arvrStabilizedRV.i;
    const float qy = val.un.arvrStabilizedRV.j;
    const float qz = val.un.arvrStabilizedRV.k;

    // Reject degenerate quaternions at the source: around a tare / persist /
    // hub reset the in-flight report can decode to a non-unit quaternion, and
    // q_to_ypr's asin() then returns NaN — which would flow into everything
    // downstream (attitude indicator, water slosh, face inertia, timewarp).
    const float qn = qw * qw + qx * qx + qy * qy + qz * qz;
    if (!std::isfinite(qn) || qn < 0.25f || qn > 4.0f) return;

    // Latest pose for level()'s reorientation math. Callbacks fire on the
    // service thread (sh2_service dispatches synchronously), the same thread
    // that consumes this in service_loop — no locking needed.
    last_q_[0] = qw; last_q_[1] = qx; last_q_[2] = qy; last_q_[3] = qz;
    have_q_ = true;

    float yaw, pitch, roll;                  // radians (euler.c uses atan2/asin)
    // q_to_ypr's out-params are ordered yaw, pitch, roll. Passing (&roll, ...,
    // &yaw) here had them swapped, so the compass heading tracked the sensor's
    // ROLL — tilting the head spun the compass while turning moved "R".
    q_to_ypr(qw, qx, qy, qz, &yaw, &pitch, &roll);
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll))
        return;                              // |asin arg| > 1 on a noisy sample
    // Manual trim: small additive corrections for residual lean (menu
    // sliders / config). Yaw trim is heading_offset below, as before.
    const float roll_deg  = roll  * kRad2Deg + roll_trim_.load();
    const float pitch_deg = pitch * kRad2Deg + pitch_trim_.load();
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

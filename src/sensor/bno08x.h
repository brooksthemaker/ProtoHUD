#pragma once
// ── bno08x.h ──────────────────────────────────────────────────────────────────
// Driver for the BNO086 (CEVA BNO08x family) 9-DOF sensor over I2C, built on the
// vendored CEVA SH-2 library (vendor/sh2). Unlike the BNO055's simple register
// map, the BNO08x speaks SH-2 / SHTP; this driver wraps that with an I2C +
// INT/RST HAL (raw linux/gpio.h ioctl, no libgpiod dependency) and surfaces:
//
//   • heading_callback     — tilt-compensated compass heading, from the
//     magnetometer-referenced ARVR-stabilized rotation vector (no yaw drift).
//     Feeds AppState::imu_bno08x, same as Bno055 feeds imu_bno.
//   • orientation_callback — roll/pitch/yaw (deg) for head tracking; feeds the
//     imu_pose path (timewarp + pin-in-space) when head_tracking is enabled.
//   • sample_callback      — quaternion + accuracy for the debug window.
//
// recenter() issues an SH-2 tare so "forward" matches the display.
//
// NOTE: the SH-2 library keeps process-global state, so only ONE Bno08x
// instance may be active at a time (one head IMU — fine here).
//
// Threading: a background thread services SHTP (~1 kHz); callbacks fire there.

#include <atomic>
#include <functional>
#include <utility>
#include <string>
#include <thread>
#include <cstdint>

class Bno08x {
public:
    struct Config {
        bool        enabled            = false;
        std::string i2c_bus            = "/dev/i2c-1";
        int         i2c_addr           = 0x4A;         // 0x4B if SA0/ADR tied high
        std::string gpiochip           = "/dev/gpiochip0";
        int         int_line           = -1;           // INT offset (data-ready, active-low); -1 = poll the bus
        int         rst_line           = -1;           // RST offset (active-low); -1 = no hardware reset
        int         report_interval_us = 5000;         // 5 ms = 200 Hz orientation
        int         aux_interval_us    = 40000;        // 25 Hz calibrated accel/gyro/mag
        bool        auto_calibrate     = true;         // dynamic cal on accel/gyro/mag +
                                                       // periodic DCD save to chip flash
        float       declination_deg    = 0.0f;         // local magnetic declination (+E/-W)
        float       heading_offset     = 0.0f;         // mechanical mount offset (deg)
        bool        heading_invert     = false;        // flip yaw→heading direction if mirrored
        bool        head_tracking      = false;        // also feed imu_pose for head tracking
        // Manual trim added to the euler outputs (degrees) — fine correction
        // for residual lean after Set Level, or instead of it for small
        // mounting angles. Additive on roll/pitch only (yaw already has
        // heading_offset), so keep it small (< ~15°) — beyond that use Set
        // Level, which reorients properly in the quaternion domain.
        float       roll_trim          = 0.0f;
        float       pitch_trim         = 0.0f;
    };

    struct Sample {
        float   quaternion[4] = {1, 0, 0, 0};  // w, x, y, z
        float   euler_deg[3]  = {0, 0, 0};     // roll, pitch, yaw
        float   heading_deg   = 0.f;           // 0..360, offset + declination applied
        float   accuracy_deg  = 0.f;           // rotation-vector accuracy estimate
        uint8_t status        = 0;             // SH-2 status byte (accuracy bits 1..0)
    };

    // Calibrated accel / gyro / mag, streamed alongside the rotation vector at
    // the (slower) aux rate. One event per report; `kind` says which block of
    // `v` is fresh. `status` is the SH-2 accuracy for THAT sensor (0 unreliable
    // .. 3 high) — for Mag it's the magnetometer calibration quality, the
    // number that tells you whether the fused heading can be trusted.
    struct AuxSample {
        enum class Kind : uint8_t { Accel, Gyro, Mag } kind = Kind::Accel;
        float   v[3]   = {0, 0, 0};   // Accel: m/s²   Gyro: rad/s   Mag: µT
        uint8_t status = 0;
    };

    explicit Bno08x(const Config& cfg);
    ~Bno08x();

    using HeadingCallback     = std::function<void(float heading_deg)>;
    using OrientationCallback = std::function<void(float roll, float pitch, float yaw)>;
    using SampleCallback      = std::function<void(const Sample&)>;
    using AuxCallback         = std::function<void(const AuxSample&)>;
    void set_aux_callback(AuxCallback cb)                 { aux_cb_    = std::move(cb); }
    void set_heading_callback(HeadingCallback cb)         { cb_        = std::move(cb); }
    void set_orientation_callback(OrientationCallback cb) { orient_cb_ = std::move(cb); }
    void set_sample_callback(SampleCallback cb)           { samp_cb_   = std::move(cb); }

    bool start();
    void stop();
    bool restart();
    bool connected() const { return running_.load() && ok_.load(); }

    // Tare: make the current orientation the new "forward" (yaw only, or all
    // axes). Applied on the service thread. Wired to a menu action / recenter.
    void recenter(bool all_axes = false) { tare_request_.store(all_axes ? 2 : 1); }
    // Mounting calibration: tare roll/pitch ONLY (X|Y) so the sensor's mount
    // orientation reads as level, leaving the yaw/north reference untouched,
    // and persist it on the chip so it survives power cycles. Fixes every
    // absolute-roll consumer at the source (Motion Reactive lean, Face
    // Inertia rest offset, the readout's RPY).
    void level() { tare_request_.store(3); }

    void set_declination_deg(float v) { declination_deg_.store(v); }
    void set_head_tracking(bool v)    { head_tracking_.store(v);   }
    // Live trim (menu sliders) — applied to roll/pitch on the next sample.
    void set_trim(float roll_deg, float pitch_deg) {
        roll_trim_.store(roll_deg);
        pitch_trim_.store(pitch_deg);
    }
    // Software tare: fold the CURRENT (post-trim) roll/pitch into the trim
    // offsets so this exact pose reads 0/0 from the next sample on — pure
    // software, independent of whatever the chip's hardware tare (level())
    // did or didn't achieve. Returns {roll_trim, pitch_trim} so the caller
    // can persist them / reflect them on the trim sliders. Undo by setting
    // the trims back to zero.
    std::pair<float, float> zero_here() {
        const float r = roll_trim_.load()  - last_roll_deg_.load();
        const float p = pitch_trim_.load() - last_pitch_deg_.load();
        roll_trim_.store(r);
        pitch_trim_.store(p);
        return { r, p };
    }

    // Internal — public only so the C HAL/callback trampolines can reach them.
    bool hal_open();
    void hal_close();
    int  hal_read(uint8_t* buf, unsigned len, uint32_t* t_us);
    int  hal_write(uint8_t* buf, unsigned len);
    void on_sensor_event(void* sh2_sensor_event);   // sh2_SensorEvent_t*
    void on_async_event(void* sh2_async_event);     // sh2_AsyncEvent_t*

private:
    void service_loop();
    bool enable_reports();
    bool gpio_open();
    bool int_asserted();     // true = data ready (INT low, or no INT wired)
    void rst_pulse();

    Config             cfg_;
    std::thread        thread_;
    std::atomic<bool>  running_        { false };
    std::atomic<bool>  ok_             { false };
    std::atomic<bool>  want_reinit_    { false };   // set on device reset → re-enable reports
    std::atomic<int>   tare_request_   { 0 };       // 0=none, 1=yaw, 2=all axes, 3=level
    // Latest sample quaternion (w,x,y,z) for level()'s reorientation math.
    // Written and read on the service thread only.
    double             last_q_[4]      = {1, 0, 0, 0};
    bool               have_q_         = false;
    std::atomic<float> declination_deg_{ 0.f };
    std::atomic<float> roll_trim_      { 0.f };
    std::atomic<float> pitch_trim_     { 0.f };
    // Latest published (post-trim) euler, for zero_here()'s capture.
    std::atomic<float> last_roll_deg_  { 0.f };
    std::atomic<float> last_pitch_deg_ { 0.f };
    std::atomic<bool>  head_tracking_  { false };

    int i2c_fd_ = -1;
    int int_fd_ = -1;   // gpio line-request fd for INT (input)
    int rst_fd_ = -1;   // gpio line-request fd for RST (output)

    HeadingCallback     cb_;
    OrientationCallback orient_cb_;
    SampleCallback      samp_cb_;
    AuxCallback         aux_cb_;
};

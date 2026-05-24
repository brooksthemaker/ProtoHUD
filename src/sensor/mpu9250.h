#pragma once
// ── mpu9250.h ─────────────────────────────────────────────────────────────────
// Backup compass driver for the HiLetgo GY-9250 / MPU-9250 + AK8963 breakout.
// Communicates over Linux I2C (i2c-dev); no external library required.
//
// Usage:
//   Mpu9250 imu(cfg);
//   imu.set_heading_callback([](float deg){ /* update state */ });
//   imu.start();
//   ...
//   imu.stop();
//
// Calibration (hard-iron, one-time):
//   imu.begin_calibration();         // start slow rotation of sensor
//   /* rotate sensor ~360° on each axis for 30 s */
//   imu.end_calibration();           // computes and stores biases
//   float bx, by, bz;
//   imu.get_mag_bias(bx, by, bz);    // persist to config
//
// The heading callback fires at ~50 Hz on a background thread.
// When the VITURE XR glasses' IMU is connected it fires at ~1 kHz,
// effectively dominating compass_heading; this driver quietly takes over
// the moment the glasses disconnect (last-writer-wins, mutex-protected).

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class Mpu9250 {
public:
    struct Config {
        bool        enabled          = false;
        std::string i2c_bus          = "/dev/i2c-1"; // GPIO 2/3 on CM5 40-pin header
        int         mpu_addr         = 0x68;          // 0x68 (AD0 low) or 0x69 (AD0 high)
        float       declination_deg  = 0.0f;          // local magnetic declination (+E/-W)
        float       heading_offset   = 0.0f;          // mechanical mounting offset (degrees)
        // Mounting orientation: 0=default, 1=90°CCW, 2=180°, 3=270°CCW (in XY plane).
        // Use when the chip is physically rotated relative to the expected axis convention.
        int         mount_rotation   = 0;
        int         heading_axes     = 0;  // 0=XY(default) 1=ZY 2=XZ 3=ZX 4=YX 5=YZ
        // Hard-iron calibration offsets — persisted to config
        float       mag_bias_x       = 0.0f;
        float       mag_bias_y       = 0.0f;
        float       mag_bias_z       = 0.0f;
    };

    explicit Mpu9250(const Config& cfg);
    ~Mpu9250();

    using HeadingCallback = std::function<void(float heading_deg)>;

    // Register callback invoked on each new heading estimate (~50 Hz).
    void set_heading_callback(HeadingCallback cb) { cb_ = std::move(cb); }

    // Full 9-axis sample, delivered alongside each heading update for the debug
    // window. Magnetometer is sensitivity-adjusted and hard-iron corrected.
    struct Sample {
        float accel_g[3]  = {0.f, 0.f, 0.f};   // x, y, z (g)
        float gyro_dps[3] = {0.f, 0.f, 0.f};   // x, y, z (deg/s)
        float mag_ut[3]   = {0.f, 0.f, 0.f};   // x, y, z (µT)
        float temp_c      = 0.f;               // die temperature
        float heading_deg = 0.f;               // fused compass heading
    };
    using SampleCallback = std::function<void(const Sample&)>;

    // Register callback invoked with the full raw sample on each update (~50 Hz).
    void set_sample_callback(SampleCallback cb) { sample_cb_ = std::move(cb); }

    // Open I2C, initialise MPU-9250 and AK8963, start background thread.
    // Returns false if the device is not found or initialisation fails.
    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Enabled flag (gates start()).  Toggling the menu "Active" item updates
    // this so the change can be persisted to config on exit, and so start()
    // works at runtime even when the config loaded with the compass disabled.
    void set_enabled(bool e) { cfg_.enabled = e; }
    bool is_enabled() const  { return cfg_.enabled; }

    // ── Calibration ──────────────────────────────────────────────────────────
    // Rotate the sensor through 360° on each axis while calibrating.
    // end_calibration() computes bias from the accumulated min/max envelope.
    void begin_calibration();
    void end_calibration();   // also calls set_mag_bias() internally
    bool is_calibrating() const { return calibrating_.load(); }

    // Runtime mounting orientation (0–3, same units as Config::mount_rotation).
    // Thread-safe: read from sensor thread, written from menu/render thread.
    void set_mount_rotation(int r);
    int  get_mount_rotation() const;

    // Heading axis preset for 3-D mounting orientations (see mpu9250.cpp for values).
    // Use when mount_rotation doesn't help, i.e. chip is not face-up.
    void set_heading_axes(int a);
    int  get_heading_axes() const;

    // Direct bias access (set on load from config; get to persist on exit).
    void get_mag_bias(float& x, float& y, float& z) const;
    void set_mag_bias(float  x, float  y, float  z);

private:
    bool  open_bus();
    bool  init_mpu();
    bool  init_ak8963();
    void  sensor_thread_fn();
    float compute_heading(float mx, float my, float mz,
                          int16_t ax, int16_t ay, int16_t az) const;

    // Raw I2C primitives
    bool  write_reg(int fd, uint8_t dev_addr, uint8_t reg, uint8_t val);
    bool  read_regs(int fd, uint8_t dev_addr, uint8_t reg,
                    uint8_t* buf, int len);

    Config cfg_;
    HeadingCallback cb_;
    SampleCallback  sample_cb_;

    int  i2c_fd_  = -1;
    // AK8963 factory sensitivity adjustment (read from fuse ROM at init)
    float adj_x_  = 1.0f, adj_y_ = 1.0f, adj_z_ = 1.0f;

    std::thread        thread_;
    std::atomic<bool>  running_       { false };
    std::atomic<bool>  calibrating_   { false };
    std::atomic<int>   mount_rotation_{ 0 };
    std::atomic<int>   heading_axes_  { 0 };

    // Hard-iron calibration accumulators
    float cal_min_x_, cal_max_x_;
    float cal_min_y_, cal_max_y_;
    float cal_min_z_, cal_max_z_;
};

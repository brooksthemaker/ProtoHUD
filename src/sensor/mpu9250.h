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

    // Open I2C, initialise MPU-9250 and AK8963, start background thread.
    // Returns false if the device is not found or initialisation fails.
    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // ── Calibration ──────────────────────────────────────────────────────────
    // Rotate the sensor through 360° on each axis while calibrating.
    // end_calibration() computes bias from the accumulated min/max envelope.
    void begin_calibration();
    void end_calibration();   // also calls set_mag_bias() internally
    bool is_calibrating() const { return calibrating_.load(); }

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

    int  i2c_fd_  = -1;
    // AK8963 factory sensitivity adjustment (read from fuse ROM at init)
    float adj_x_  = 1.0f, adj_y_ = 1.0f, adj_z_ = 1.0f;

    std::thread        thread_;
    std::atomic<bool>  running_     { false };
    std::atomic<bool>  calibrating_ { false };

    // Hard-iron calibration accumulators
    float cal_min_x_, cal_max_x_;
    float cal_min_y_, cal_max_y_;
    float cal_min_z_, cal_max_z_;
};

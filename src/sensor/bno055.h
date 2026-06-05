#pragma once
// ── bno055.h ──────────────────────────────────────────────────────────────────
// Adafruit BNO055 (9-DOF absolute orientation) driver over Linux i2c-dev or a
// serial UART (Config::transport = "i2c" | "uart"). UART mode (PS1 → 3.3V)
// sidesteps the Pi's I²C clock-stretching trouble — Adafruit's recommended path.
// Unlike the MPU9250, the BNO055 does sensor fusion on-chip and reports
// heading/roll/pitch directly in degrees, plus a calibration status byte.
// We just init it in NDOF mode and poll the EUL_* registers — no Madgwick /
// Mahony / hard-iron-bias maths needed in userland.
//
// Threading: same pattern as Mpu9250 — heading_callback fires at ~50 Hz on a
// background thread; sample_callback delivers the full 9-axis + calibration
// snapshot alongside each heading update for the debug window.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class Bno055 {
public:
    struct Config {
        bool        enabled         = false;
        std::string i2c_bus         = "/dev/i2c-1";   // GPIO 2/3 on CM5 40-pin header
        int         i2c_addr        = 0x28;           // default; 0x29 if ADR pin tied high
        float       declination_deg = 0.0f;           // local magnetic declination (+E/-W)
        float       heading_offset  = 0.0f;           // mechanical mounting offset
        // Mounting orientation: same enum as Mpu9250::Config::heading_axes so
        // the menu can reuse the picker. 0 = default; we re-map via software
        // by reading the appropriate Euler-axis pair rather than touching the
        // chip's AXIS_MAP_CONFIG register (keeps NDOF fusion stable).
        int         heading_axes    = 0;
        // Poll rate. BNO055 internal fusion runs at 100 Hz; sampling at
        // 50 Hz keeps CPU low and matches Mpu9250's cadence.
        float       poll_hz         = 50.0f;
        // Where to persist the 22-byte on-chip calibration profile (empty →
        // disabled). Restored at start() when the file exists; saved on
        // request, or automatically the first time the chip reports fully
        // calibrated (sys == 3) when no file exists yet.
        std::string calib_path;
        bool        auto_save_calibration = true;
        // Transport: "i2c" (default) or "uart". The Pi's I²C clock-stretching is
        // unreliable with the BNO055; UART mode (solder PS1 → 3.3V so SCL/SDA
        // become RX/TX) sidesteps it. uart_device is the serial port; the
        // BNO055 UART runs at 115200.
        std::string transport   = "i2c";
        std::string uart_device = "/dev/ttyAMA0";
        int         uart_baud   = 115200;
    };

    explicit Bno055(const Config& cfg);
    ~Bno055();

    using HeadingCallback = std::function<void(float heading_deg)>;
    void set_heading_callback(HeadingCallback cb) { cb_ = std::move(cb); }

    // 9-axis sample + on-chip calibration status, delivered alongside each
    // heading update. Calibration values are 0..3 (0 = uncalibrated, 3 =
    // fully calibrated); when sys < 3 the heading should be treated as
    // drifting until the user rotates the head through enough orientations.
    struct Sample {
        float accel_g[3]   = {0, 0, 0};
        float gyro_dps[3]  = {0, 0, 0};
        float mag_ut[3]    = {0, 0, 0};
        float euler_deg[3] = {0, 0, 0};   // [0]=heading, [1]=roll, [2]=pitch
        float quaternion[4]= {1, 0, 0, 0}; // w,x,y,z
        uint8_t calib_sys = 0, calib_gyro = 0, calib_accel = 0, calib_mag = 0;
    };
    void set_sample_callback(std::function<void(const Sample&)> cb) { samp_cb_ = std::move(cb); }

    bool start();
    void stop();
    bool connected() const { return running_.load(); }

    // Live tuning from the menu — picked up next poll cycle.
    void set_declination_deg(float v) { declination_deg_.store(v); }
    void set_heading_offset (float v) { heading_offset_ .store(v); }
    void set_heading_axes   (int   v) { heading_axes_   .store(v); }
    bool is_calibrated()      const   { return calib_sys_.load() >= 2; }
    uint8_t calib_sys()       const   { return calib_sys_.load(); }

    // ── Calibration persistence ────────────────────────────────────────────
    // Async: the poll thread drops to CONFIG mode, reads the 22-byte offset
    // profile, returns to NDOF, writes it to cfg.calib_path, then fires the
    // saved-callback with success/failure. Best done once the chip reports
    // sys == 3. Restore is automatic at start() when the file exists.
    void request_calibration_save() { save_req_.store(true); }
    void set_calib_saved_callback(std::function<void(bool ok)> cb) {
        calib_saved_cb_ = std::move(cb);
    }
    bool has_saved_calibration() const;   // a calib file exists + is the right size
    uint8_t calib_gyro()      const   { return calib_gyro_.load(); }
    uint8_t calib_accel()     const   { return calib_accel_.load(); }
    uint8_t calib_mag()       const   { return calib_mag_.load(); }

private:
    bool open_bus();
    bool open_uart();
    void close_bus();
    bool is_uart() const { return cfg_.transport == "uart"; }
    bool init_chip_locked();
    bool write_reg(uint8_t reg, uint8_t val);
    bool read_regs(uint8_t reg, uint8_t* buf, size_t len);
    // BNO055 serial register protocol (used when transport == "uart").
    bool uart_write_reg(uint8_t reg, uint8_t val);
    bool uart_read_regs(uint8_t reg, uint8_t* buf, size_t len);
    bool uart_read_exact(uint8_t* buf, size_t n, int timeout_ms);
    void uart_flush_input();
    void poll_loop();

    // Calibration profile (22 bytes at 0x55..0x6A). read/write helpers assume
    // the chip is already in CONFIG mode (offsets are only writable there).
    bool read_calib_offsets(uint8_t out[22]);
    bool write_calib_offsets(const uint8_t in[22]);
    bool save_calibration();                       // CONFIG→read→NDOF→file (poll thread)
    bool load_calibration_file(uint8_t out[22]) const;

    Config cfg_;
    int    i2c_fd_  = -1;
    int    uart_fd_ = -1;

    HeadingCallback                       cb_;
    std::function<void(const Sample&)>    samp_cb_;

    std::atomic<bool>    running_         { false };
    std::atomic<float>   declination_deg_ { 0.f };
    std::atomic<float>   heading_offset_  { 0.f };
    std::atomic<int>     heading_axes_    { 0   };
    std::atomic<uint8_t> calib_sys_       { 0   };
    std::atomic<uint8_t> calib_gyro_      { 0   };
    std::atomic<uint8_t> calib_accel_     { 0   };
    std::atomic<uint8_t> calib_mag_       { 0   };

    std::atomic<bool>    save_req_        { false };   // menu → poll thread: save now
    std::atomic<bool>    auto_saved_      { false };   // one-shot auto-save guard
    std::function<void(bool)> calib_saved_cb_;

    std::thread thread_;
};

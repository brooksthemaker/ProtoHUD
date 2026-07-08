#include "mpu9250.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

// Linux I2C-dev (target only — compiled on Raspberry Pi CM5)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

// ── MPU-9250 register addresses ───────────────────────────────────────────────
static constexpr uint8_t MPU_SMPLRT_DIV    = 0x19;
static constexpr uint8_t MPU_CONFIG        = 0x1A;
static constexpr uint8_t MPU_GYRO_CONFIG   = 0x1B;
static constexpr uint8_t MPU_ACCEL_CONFIG  = 0x1C;
static constexpr uint8_t MPU_INT_PIN_CFG   = 0x37; // bit1 = BYPASS_EN
static constexpr uint8_t MPU_ACCEL_XOUT_H  = 0x3B;
static constexpr uint8_t MPU_TEMP_OUT_H    = 0x41; // 2 bytes
static constexpr uint8_t MPU_GYRO_XOUT_H   = 0x43; // 6 bytes
static constexpr uint8_t MPU_PWR_MGMT_1    = 0x6B;
static constexpr uint8_t MPU_WHO_AM_I      = 0x75; // expect 0x71 or 0x73

// ── AK8963 (magnetometer) register addresses ──────────────────────────────────
static constexpr uint8_t AK_ADDR  = 0x0C;
static constexpr uint8_t AK_WIA   = 0x00; // WHO_AM_I — expect 0x48
static constexpr uint8_t AK_ST1   = 0x02; // bit0 = DRDY
static constexpr uint8_t AK_HXL   = 0x03; // 6 bytes: HXL HXH HYL HYH HZL HZH
static constexpr uint8_t AK_ST2   = 0x09; // bit3 = HOFL (overflow); must read to unlatch
static constexpr uint8_t AK_CNTL1 = 0x0A; // mode register
static constexpr uint8_t AK_ASAX  = 0x10; // 3 sensitivity-adjust ROM bytes

static constexpr uint8_t AK_MODE_POWER_DOWN = 0x00;
static constexpr uint8_t AK_MODE_FUSE_ROM   = 0x0F;
static constexpr uint8_t AK_MODE_CONT2_16BIT = 0x16; // 100 Hz, 16-bit output

// ── Constructor / Destructor ──────────────────────────────────────────────────

Mpu9250::Mpu9250(const Config& cfg)
    : cfg_(cfg), mount_rotation_(cfg.mount_rotation), heading_axes_(cfg.heading_axes) {}

Mpu9250::~Mpu9250() { stop(); }

// ── I2C primitives ────────────────────────────────────────────────────────────

bool Mpu9250::write_reg(int fd, uint8_t dev_addr, uint8_t reg, uint8_t val) {
    if (ioctl(fd, I2C_SLAVE, dev_addr) < 0) return false;
    uint8_t buf[2] = { reg, val };
    return ::write(fd, buf, 2) == 2;
}

bool Mpu9250::read_regs(int fd, uint8_t dev_addr, uint8_t reg,
                        uint8_t* buf, int len) {
    if (ioctl(fd, I2C_SLAVE, dev_addr) < 0) return false;
    if (::write(fd, &reg, 1) != 1) return false;
    return ::read(fd, buf, len) == len;
}

// ── Initialisation ────────────────────────────────────────────────────────────

bool Mpu9250::open_bus() {
    i2c_fd_ = open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (i2c_fd_ < 0) {
        std::cerr << "[mpu9250] cannot open " << cfg_.i2c_bus
                  << ": " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool Mpu9250::init_mpu() {
    // Verify WHO_AM_I (0x71 for MPU-9250, 0x73 for MPU-9255, 0x68 for MPU-6050 clone)
    uint8_t who = 0;
    if (!read_regs(i2c_fd_, cfg_.mpu_addr, MPU_WHO_AM_I, &who, 1)) {
        std::cerr << "[mpu9250] no ACK from MPU at 0x"
                  << std::hex << (int)cfg_.mpu_addr << std::dec << "\n";
        return false;
    }
    if (who != 0x71 && who != 0x73 && who != 0x68 && who != 0x70) {
        std::cerr << "[mpu9250] unexpected WHO_AM_I=0x" << std::hex << (int)who
                  << " (expected 0x71/0x73)\n" << std::dec;
        // Continue anyway — some clones differ
    }
    std::cerr << "[mpu9250] WHO_AM_I=0x" << std::hex << (int)who << std::dec << "\n";

    // Reset
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_PWR_MGMT_1, 0x80);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Wake up, PLL clock from X gyro
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_PWR_MGMT_1, 0x01);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // DLPF bandwidth ~184 Hz (smooth gyro/accel for tilt data)
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_CONFIG,       0x01);
    // Sample-rate divider: 9 → 100 Hz (1000/(1+9))
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_SMPLRT_DIV,   0x09);
    // Gyro ±250°/s
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_GYRO_CONFIG,  0x00);
    // Accel ±2 g
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_ACCEL_CONFIG, 0x00);

    // Enable I2C bypass so we can talk directly to AK8963
    write_reg(i2c_fd_, cfg_.mpu_addr, MPU_INT_PIN_CFG,  0x02);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return true;
}

bool Mpu9250::init_ak8963() {
    // Verify AK8963 WHO_AM_I
    uint8_t who = 0;
    if (!read_regs(i2c_fd_, AK_ADDR, AK_WIA, &who, 1) || who != 0x48) {
        std::cerr << "[mpu9250] AK8963 not found (WHO_AM_I=0x"
                  << std::hex << (int)who << std::dec
                  << ") — check BYPASS_EN / wiring\n";
        return false;
    }

    // Power down before mode change
    write_reg(i2c_fd_, AK_ADDR, AK_CNTL1, AK_MODE_POWER_DOWN);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // Enter fuse ROM mode to read sensitivity adjustment
    write_reg(i2c_fd_, AK_ADDR, AK_CNTL1, AK_MODE_FUSE_ROM);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    uint8_t asa[3] = {};
    read_regs(i2c_fd_, AK_ADDR, AK_ASAX, asa, 3);
    // Adjustment formula from datasheet: Hadj = H × ((ASA - 128) / 256 + 1)
    adj_x_ = (asa[0] - 128.0f) / 256.0f + 1.0f;
    adj_y_ = (asa[1] - 128.0f) / 256.0f + 1.0f;
    adj_z_ = (asa[2] - 128.0f) / 256.0f + 1.0f;
    std::cerr << "[mpu9250] AK8963 adj x=" << adj_x_
              << " y=" << adj_y_ << " z=" << adj_z_ << "\n";

    // Power down, then set 16-bit continuous mode 2 (100 Hz)
    write_reg(i2c_fd_, AK_ADDR, AK_CNTL1, AK_MODE_POWER_DOWN);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    write_reg(i2c_fd_, AK_ADDR, AK_CNTL1, AK_MODE_CONT2_16BIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return true;
}

void Mpu9250::set_mount_rotation(int r) { mount_rotation_.store(r & 3); }
int  Mpu9250::get_mount_rotation() const { return mount_rotation_.load(); }

void Mpu9250::set_heading_axes(int a) { heading_axes_.store(a); }
int  Mpu9250::get_heading_axes() const { return heading_axes_.load(); }

// ── Heading computation ───────────────────────────────────────────────────────
// Preset 0 (default):  chip face-up,       X=fwd, Y=left, Z=up
//   → tilt-compensated atan2(-my_h, mx_h)
// Presets 1-5: chip in non-horizontal orientation — flat (no tilt comp)
//   The formula is atan2(-B, A) where A/B are chosen per preset:
//   1  ZY   chip face-forward / perpendicular to face (Z=fwd, Y=left)
//   2  XZ   chip on its left side  (X=fwd, Z=left)
//   3  ZX   chip face-forward, rotated 90° (Z=fwd, X=left)
//   4  YX   chip on its right side (Y=fwd, X=left)
//   5  YZ   chip face-up, alt 90°  (Y=fwd, Z=left)
// mount_rotation (0-3) first rotates the XY plane in 90° CCW steps; use
// this for fine-tuning within a given preset.

float Mpu9250::compute_heading(float mx, float my, float mz,
                                int16_t ax, int16_t ay, int16_t az) const {
    float ax_f = ax / 16384.0f;
    float ay_f = ay / 16384.0f;
    float az_f = az / 16384.0f;

    // In-plane mounting correction (XY rotation, 90° CCW per step)
    for (int i = 0, r = mount_rotation_.load() & 3; i < r; ++i) {
        float t;
        t = mx; mx = -my; my = t;
        t = ax_f; ax_f = -ay_f; ay_f = t;
    }

    // Select axis pair for the heading formula atan2(-B, A)
    const int axes = heading_axes_.load();
    float A, B;
    switch (axes) {
        case 1:  A =  mz; B =  my; break;   // ZY  — face-forward
        case 2:  A =  mx; B = -mz; break;   // XZ  — left-side
        case 3:  A = -mz; B =  mx; break;   // ZX  — face-forward 90°
        case 4:  A =  my; B = -mx; break;   // YX  — right-side
        case 5:  A =  my; B =  mz; break;   // YZ  — face-up alt
        default: A =  mx; B =  my; break;   // 0: XY standard
    }

    float heading;
    if (axes == 0) {
        // Full tilt-compensated heading for the standard face-up orientation
        float norm = sqrtf(ax_f*ax_f + ay_f*ay_f + az_f*az_f);
        if (norm >= 0.1f) {
            ax_f /= norm; ay_f /= norm; az_f /= norm;
            float pitch = asinf(-ax_f);
            float roll  = atan2f(ay_f, az_f);
            float cp = cosf(pitch), sp = sinf(pitch);
            float cr = cosf(roll),  sr = sinf(roll);
            float mx_h =  mx * cp + mz * sp;
            float my_h =  mx * sr * sp + my * cr - mz * sr * cp;
            heading = atan2f(-my_h, mx_h) * (180.0f / M_PI);
        } else {
            heading = atan2f(-B, A) * (180.0f / M_PI);
        }
    } else {
        // Flat heading for non-face-up orientations (no tilt compensation)
        heading = atan2f(-B, A) * (180.0f / M_PI);
    }

    heading += cfg_.declination_deg + cfg_.heading_offset;
    if (heading < 0.f)   heading += 360.f;
    if (heading >= 360.f) heading -= 360.f;
    return heading;
}

// ── Sensor thread ─────────────────────────────────────────────────────────────

void Mpu9250::sensor_thread_fn() {
    while (running_) {
        // ── Read magnetometer ─────────────────────────────────────────────────
        uint8_t st1 = 0;
        read_regs(i2c_fd_, AK_ADDR, AK_ST1, &st1, 1);

        if (st1 & 0x01) { // data ready
            uint8_t raw[7] = {};
            if (read_regs(i2c_fd_, AK_ADDR, AK_HXL, raw, 7)) {
                uint8_t st2 = raw[6];
                if (!(st2 & 0x08)) { // no overflow
                    int16_t rx = (int16_t)((raw[1] << 8) | raw[0]);
                    int16_t ry = (int16_t)((raw[3] << 8) | raw[2]);
                    int16_t rz = (int16_t)((raw[5] << 8) | raw[4]);

                    // Apply sensitivity adjustment and hard-iron bias
                    float mx = rx * adj_x_ - cfg_.mag_bias_x;
                    float my = ry * adj_y_ - cfg_.mag_bias_y;
                    float mz = rz * adj_z_ - cfg_.mag_bias_z;

                    // Calibration accumulation
                    if (calibrating_) {
                        if (mx < cal_min_x_) cal_min_x_ = mx;
                        if (mx > cal_max_x_) cal_max_x_ = mx;
                        if (my < cal_min_y_) cal_min_y_ = my;
                        if (my > cal_max_y_) cal_max_y_ = my;
                        if (mz < cal_min_z_) cal_min_z_ = mz;
                        if (mz > cal_max_z_) cal_max_z_ = mz;
                    }

                    // ── Read accel + temp + gyro in one burst ─────────────────
                    // 0x3B..0x48 is one contiguous register block (accel 6,
                    // temp 2, gyro 6): a single 14-byte transaction replaces
                    // the three separate reads (3 ioctl+write+read triples).
                    uint8_t mraw[14] = {};
                    read_regs(i2c_fd_, cfg_.mpu_addr, MPU_ACCEL_XOUT_H, mraw, 14);
                    int16_t ax = (int16_t)((mraw[0] << 8) | mraw[1]);
                    int16_t ay = (int16_t)((mraw[2] << 8) | mraw[3]);
                    int16_t az = (int16_t)((mraw[4] << 8) | mraw[5]);

                    float heading = compute_heading(mx, my, mz, ax, ay, az);

                    if (cb_) cb_(heading);

                    // ── Full sample for the debug window ──────────────────────
                    if (sample_cb_) {
                        Sample s;
                        // Accel ±2 g → 16384 LSB/g
                        s.accel_g[0] = ax / 16384.0f;
                        s.accel_g[1] = ay / 16384.0f;
                        s.accel_g[2] = az / 16384.0f;

                        // Die temperature: T(°C) = raw/333.87 + 21.0
                        int16_t t = (int16_t)((mraw[6] << 8) | mraw[7]);
                        s.temp_c = t / 333.87f + 21.0f;

                        // Gyro ±250 °/s → 131 LSB/(°/s)
                        int16_t gx = (int16_t)((mraw[8]  << 8) | mraw[9]);
                        int16_t gy = (int16_t)((mraw[10] << 8) | mraw[11]);
                        int16_t gz = (int16_t)((mraw[12] << 8) | mraw[13]);
                        s.gyro_dps[0] = gx / 131.0f;
                        s.gyro_dps[1] = gy / 131.0f;
                        s.gyro_dps[2] = gz / 131.0f;

                        // AK8963 16-bit mode → 0.15 µT/LSB (mx/my/mz are
                        // sensitivity-adjusted, hard-iron-corrected counts)
                        s.mag_ut[0] = mx * 0.15f;
                        s.mag_ut[1] = my * 0.15f;
                        s.mag_ut[2] = mz * 0.15f;

                        s.heading_deg = heading;
                        sample_cb_(s);
                    }
                }
            }
        }

        // ~50 Hz poll (AK8963 outputs at 100 Hz in continuous mode 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ::close(i2c_fd_);
    i2c_fd_ = -1;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool Mpu9250::start() {
    if (!cfg_.enabled) return false;
    if (running_)      return true;

    if (!open_bus())    return false;
    if (!init_mpu())  { ::close(i2c_fd_); i2c_fd_ = -1; return false; }
    if (!init_ak8963()){ ::close(i2c_fd_); i2c_fd_ = -1; return false; }

    std::cerr << "[mpu9250] started on " << cfg_.i2c_bus << "\n";
    running_ = true;
    thread_  = std::thread(&Mpu9250::sensor_thread_fn, this);
    return true;
}

void Mpu9250::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ── Calibration ───────────────────────────────────────────────────────────────

void Mpu9250::begin_calibration() {
    cal_min_x_ = cal_min_y_ = cal_min_z_ =  1e9f;
    cal_max_x_ = cal_max_y_ = cal_max_z_ = -1e9f;
    calibrating_ = true;
    std::cerr << "[mpu9250] calibration started — rotate sensor through all axes\n";
}

void Mpu9250::end_calibration() {
    calibrating_ = false;
    // Hard-iron offset = centre of the min/max envelope
    float bx = (cal_min_x_ + cal_max_x_) * 0.5f;
    float by = (cal_min_y_ + cal_max_y_) * 0.5f;
    float bz = (cal_min_z_ + cal_max_z_) * 0.5f;
    set_mag_bias(bx, by, bz);
    std::cerr << "[mpu9250] calibration done  bias=("
              << bx << ", " << by << ", " << bz << ")\n";
}

void Mpu9250::get_mag_bias(float& x, float& y, float& z) const {
    x = cfg_.mag_bias_x;
    y = cfg_.mag_bias_y;
    z = cfg_.mag_bias_z;
}

void Mpu9250::set_mag_bias(float x, float y, float z) {
    cfg_.mag_bias_x = x;
    cfg_.mag_bias_y = y;
    cfg_.mag_bias_z = z;
}

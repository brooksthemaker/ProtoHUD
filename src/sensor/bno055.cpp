#include "bno055.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
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
// Calibration profile: 22 contiguous bytes 0x55..0x6A (accel/mag/gyro offsets
// + accel/mag radius). Only writable in CONFIG mode; persists in the chip
// across mode switches but is lost on power-down — hence saving to disk.
constexpr uint8_t REG_ACC_OFFSET  = 0x55;
constexpr int     CALIB_BLOB_LEN  = 22;

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

speed_t baud_constant(int baud) {
    switch (baud) {
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        default:      return B115200;   // BNO055 UART default
    }
}
} // namespace

Bno055::Bno055(const Config& cfg) : cfg_(cfg) {
    declination_deg_.store(cfg.declination_deg);
    heading_offset_ .store(cfg.heading_offset);
    heading_axes_   .store(cfg.heading_axes);
}

Bno055::~Bno055() { stop(); }

bool Bno055::write_reg(uint8_t reg, uint8_t val) {
    if (is_uart()) return uart_write_reg(reg, val);
    if (i2c_fd_ < 0) return false;
    if (ioctl(i2c_fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) return false;
    uint8_t buf[2] = { reg, val };
    return ::write(i2c_fd_, buf, 2) == 2;
}

bool Bno055::read_regs(uint8_t reg, uint8_t* buf, size_t len) {
    if (is_uart()) return uart_read_regs(reg, buf, len);
    if (i2c_fd_ < 0) return false;
    // Combined write-then-read in ONE transaction with a repeated START
    // (I2C_RDWR) instead of a separate write+STOP+read. The BNO055 stretches
    // the I2C clock, and a repeated-start read is far more reliable through the
    // Pi/RP1 controller than two discrete transfers. (If reads are still flaky,
    // slow the bus: dtparam=i2c_arm_baudrate=10000 in config.txt.)
    struct i2c_msg msgs[2];
    msgs[0].addr  = static_cast<__u16>(cfg_.i2c_addr);
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;
    msgs[1].addr  = static_cast<__u16>(cfg_.i2c_addr);
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = static_cast<__u16>(len);
    msgs[1].buf   = buf;
    struct i2c_rdwr_ioctl_data xfer { msgs, 2 };
    return ioctl(i2c_fd_, I2C_RDWR, &xfer) == 2;
}

bool Bno055::open_bus() {
    if (is_uart()) return open_uart();
    i2c_fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (i2c_fd_ < 0) {
        std::cerr << "[bno055] cannot open " << cfg_.i2c_bus
                  << ": " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool Bno055::open_uart() {
    uart_fd_ = ::open(cfg_.uart_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart_fd_ < 0) {
        std::cerr << "[bno055] cannot open " << cfg_.uart_device
                  << ": " << std::strerror(errno) << "\n";
        return false;
    }
    termios tio{};
    if (tcgetattr(uart_fd_, &tio) != 0) {
        std::cerr << "[bno055] tcgetattr failed on " << cfg_.uart_device << "\n";
        ::close(uart_fd_); uart_fd_ = -1; return false;
    }
    cfmakeraw(&tio);
    const speed_t spd = baud_constant(cfg_.uart_baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(uart_fd_, TCSANOW, &tio);
    tcflush(uart_fd_, TCIOFLUSH);
    return true;
}

void Bno055::close_bus() {
    if (i2c_fd_  >= 0) { ::close(i2c_fd_);  i2c_fd_  = -1; }
    if (uart_fd_ >= 0) { ::close(uart_fd_); uart_fd_ = -1; }
}

// ── BNO055 UART register protocol ────────────────────────────────────────────
// Write: [0xAA 0x00 reg len data…] → ack [0xEE 0x01] on success.
// Read:  [0xAA 0x01 reg len]        → [0xBB len data…] on success, else [0xEE st].
// The link occasionally NAKs with 0xEE 0x07 (bus overrun); retry a few times.
void Bno055::uart_flush_input() {
    if (uart_fd_ >= 0) tcflush(uart_fd_, TCIFLUSH);
}

bool Bno055::uart_read_exact(uint8_t* buf, size_t n, int timeout_ms) {
    if (uart_fd_ < 0) return false;
    size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (got < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd p{uart_fd_, POLLIN, 0};
        const int pr = ::poll(&p, 1, left > 0 ? left : 1);
        if (pr < 0) { if (errno == EINTR) continue; return false; }
        if (pr == 0) return false;
        const ssize_t r = ::read(uart_fd_, buf + got, n - got);
        if (r > 0) got += static_cast<size_t>(r);
        else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    }
    return true;
}

bool Bno055::uart_write_reg(uint8_t reg, uint8_t val) {
    if (uart_fd_ < 0) return false;
    const uint8_t cmd[5] = { 0xAA, 0x00, reg, 1, val };
    for (int attempt = 0; attempt < 5; ++attempt) {
        uart_flush_input();
        if (::write(uart_fd_, cmd, sizeof cmd) != static_cast<ssize_t>(sizeof cmd))
            return false;
        uint8_t resp[2];
        if (uart_read_exact(resp, 2, 30) && resp[0] == 0xEE && resp[1] == 0x01)
            return true;                              // WRITE_SUCCESS
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    return false;
}

bool Bno055::uart_read_regs(uint8_t reg, uint8_t* buf, size_t len) {
    if (uart_fd_ < 0 || len == 0 || len > 128) return false;
    const uint8_t cmd[4] = { 0xAA, 0x01, reg, static_cast<uint8_t>(len) };
    for (int attempt = 0; attempt < 5; ++attempt) {
        uart_flush_input();
        if (::write(uart_fd_, cmd, sizeof cmd) != static_cast<ssize_t>(sizeof cmd))
            return false;
        uint8_t hdr[2];
        if (!uart_read_exact(hdr, 2, 30)) continue;            // no response → retry
        if (hdr[0] == 0xBB && hdr[1] == len) {
            if (uart_read_exact(buf, len, 30)) return true;    // payload
            continue;
        }
        // hdr[0] == 0xEE → error status hdr[1] (0x07 overrun / 0x02 read-fail) → retry
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    return false;
}

bool Bno055::init_chip_locked() {
    // Wait out the chip's power-on window before the first read. The datasheet
    // POR time is ~650 ms and the UART variant can need ≥1 s; reading CHIP_ID
    // any sooner returns nothing (or garbage) and used to abort init outright.
    // Poll CHIP_ID for up to ~2 s — a chip that's already up answers on the
    // first try (no added delay after a normal boot), one that was just powered
    // simply settles first.
    uint8_t id = 0;
    bool found = false;
    for (int i = 0; i < 20; ++i) {                 // ~2 s total
        if (read_regs(REG_CHIP_ID, &id, 1) && id == CHIP_ID_BNO055) { found = true; break; }
        if (is_uart()) uart_flush_input();         // drop any partial/boot bytes
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!found) {
        std::cerr << "[bno055] CHIP_ID mismatch on "
                  << (is_uart() ? cfg_.uart_device : cfg_.i2c_bus)
                  << " (got 0x" << std::hex << static_cast<int>(id)
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

    // Restore a saved calibration profile while still in CONFIG mode (offsets
    // are only writable here). The chip keeps refining the mag model in NDOF,
    // but this gives it the gyro/accel cal and a big head start.
    uint8_t blob[CALIB_BLOB_LEN];
    if (load_calibration_file(blob)) {
        if (write_calib_offsets(blob))
            std::cerr << "[bno055] restored calibration from " << cfg_.calib_path << "\n";
    }

    write_reg(REG_OPR_MODE,  OPR_MODE_NDOF);      // start fusion
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return true;
}

// ── Calibration profile read/write/persist ──────────────────────────────────
bool Bno055::read_calib_offsets(uint8_t out[22]) {
    return read_regs(REG_ACC_OFFSET, out, CALIB_BLOB_LEN);
}

bool Bno055::write_calib_offsets(const uint8_t in[22]) {
    // Written byte-by-byte (the chip latches each offset register on write);
    // caller guarantees CONFIG mode.
    for (int i = 0; i < CALIB_BLOB_LEN; ++i)
        if (!write_reg(static_cast<uint8_t>(REG_ACC_OFFSET + i), in[i])) return false;
    return true;
}

bool Bno055::load_calibration_file(uint8_t out[22]) const {
    if (cfg_.calib_path.empty()) return false;
    FILE* f = std::fopen(cfg_.calib_path.c_str(), "rb");
    if (!f) return false;
    size_t n = std::fread(out, 1, CALIB_BLOB_LEN, f);
    std::fclose(f);
    return n == CALIB_BLOB_LEN;
}

bool Bno055::has_saved_calibration() const {
    if (cfg_.calib_path.empty()) return false;
    struct stat st{};
    return ::stat(cfg_.calib_path.c_str(), &st) == 0 && st.st_size >= CALIB_BLOB_LEN;
}

bool Bno055::save_calibration() {
    if (cfg_.calib_path.empty()) return false;
    uint8_t blob[CALIB_BLOB_LEN] = {0};
    // Offsets are only stable/readable in CONFIG mode; bounce out of NDOF,
    // read, then resume fusion. The in-chip calibration is unaffected.
    write_reg(REG_OPR_MODE, OPR_MODE_CONFIG);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const bool ok = read_calib_offsets(blob);
    write_reg(REG_OPR_MODE, OPR_MODE_NDOF);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!ok) return false;
    FILE* f = std::fopen(cfg_.calib_path.c_str(), "wb");
    if (!f) return false;
    const size_t n = std::fwrite(blob, 1, CALIB_BLOB_LEN, f);
    std::fclose(f);
    if (n != CALIB_BLOB_LEN) return false;
    std::cerr << "[bno055] saved calibration to " << cfg_.calib_path << "\n";
    return true;
}

bool Bno055::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    if (!open_bus()) return false;
    // Retry the whole init a few times — covers a chip still mid-reset when the
    // CHIP_ID poll window expires (e.g. a fresh power cycle right at launch).
    bool inited = false;
    for (int attempt = 0; attempt < 3 && !inited; ++attempt) {
        if (init_chip_locked()) { inited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!inited) {
        close_bus();
        return false;
    }
    // If a calibration file already exists, treat the one-shot auto-save as
    // done so we don't overwrite a good profile with a fresh (possibly worse)
    // one. Explicit "Save" from the menu always overwrites.
    auto_saved_.store(has_saved_calibration());
    running_.store(true);
    thread_ = std::thread(&Bno055::poll_loop, this);
    return true;
}

void Bno055::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (i2c_fd_ >= 0 || uart_fd_ >= 0) {
        write_reg(REG_OPR_MODE, OPR_MODE_CONFIG);
        close_bus();
    }
}

// Full teardown + re-init: closes the bus, re-opens it, and runs the settle +
// CHIP_ID poll again. Lets the menu kick a sensor that wasn't powered/ready at
// boot (the BNO055 needs ≥1 s after power-on before it talks) without
// restarting ProtoHUD. Returns true if the chip came back up.
bool Bno055::restart() {
    stop();
    if (!cfg_.enabled) return false;
    return start();
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

        // Calibration persistence (done on this thread so it owns the bus).
        // Explicit request always saves; otherwise auto-save once when the chip
        // first reports fully calibrated and no file exists yet.
        if (save_req_.exchange(false)) {
            const bool ok = save_calibration();
            auto_saved_.store(true);
            if (calib_saved_cb_) calib_saved_cb_(ok);
        } else if (cfg_.auto_save_calibration && !auto_saved_.load() &&
                   calib_sys_.load() >= 3) {
            const bool ok = save_calibration();
            auto_saved_.store(true);
            if (calib_saved_cb_) calib_saved_cb_(ok);
        }

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

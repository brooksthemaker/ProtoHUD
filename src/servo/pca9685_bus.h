#pragma once
// ── pca9685_bus.h ─────────────────────────────────────────────────────────────
// Direct-I²C servo backend: the PCA9685 wired to the CM5's own bus, replacing
// the coprocessor hop (SERVOM over USB serial) when servo_bus.transport =
// "i2c" in config.json. The PCA generates the pulses in hardware either way;
// this class takes over what the RP2350's servo_service() used to do — a
// ~66 Hz easing loop slewing each channel toward its target at its commanded
// deg/s — plus the deg→µs mapping from the per-channel pulse window
// (SERVOCAL's job on the firmware path).
//
// Sink-compatible with ServoController: move(ch, deg, speed) with deg -1 =
// detach (pulse off, servo goes limp), calibrate(ch, min_us, max_us).
//
// The board may be absent (not wired yet, helmet half-assembled): moves are
// kept as pending state and the worker re-probes every 5 s, so the ears
// assume their commanded pose the moment the board appears — no restart.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace servo {

class Pca9685Bus {
public:
    struct Config {
        std::string i2c_bus  = "/dev/i2c-1";
        int         i2c_addr = 0x40;   // PCA9685 default (A5..A0 low)
    };

    explicit Pca9685Bus(Config cfg) : cfg_(std::move(cfg)) {}
    ~Pca9685Bus() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        thr_ = std::thread(&Pca9685Bus::run, this);
    }
    void stop() {
        running_.store(false);
        if (thr_.joinable()) thr_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
    // Board probed and initialised (false while unwired — moves stay pending).
    bool connected() const { return online_.load(); }

    // ServoController sink. deg -1 = detach; speed deg/s, 0 = snap.
    void move(int ch, int deg, int speed) {
        if (ch < 0 || ch >= kChannels) return;
        std::lock_guard<std::mutex> lk(mtx_);
        Ch& c = ch_[ch];
        if (deg < 0) {                      // detach: stop driving the pin
            if (c.attached) { c.attached = false; c.dirty = true; }
            return;
        }
        c.tgt   = static_cast<float>(deg > 180 ? 180 : deg);
        c.speed = static_cast<float>(speed < 0 ? 0 : speed);
        if (!c.attached) {                  // first move = attach: snap to the
            c.cur = c.tgt;                  // target, exactly like the firmware
            c.attached = true;
        }
        c.dirty = true;
    }
    // ServoController cal sink: the µs window 0..180° maps onto.
    void calibrate(int ch, int min_us, int max_us) {
        if (ch < 0 || ch >= kChannels) return;
        std::lock_guard<std::mutex> lk(mtx_);
        ch_[ch].min_us = min_us;
        ch_[ch].max_us = max_us;
        if (ch_[ch].attached) ch_[ch].dirty = true;   // re-map current pose
    }

private:
    static constexpr int kChannels = 16;
    // Registers (datasheet §7.3). Auto-increment lets one write carry all four
    // ON/OFF bytes of a channel.
    static constexpr uint8_t kRegMode1    = 0x00;
    static constexpr uint8_t kRegLed0OnL  = 0x06;
    static constexpr uint8_t kRegAllOffH  = 0xFD;
    static constexpr uint8_t kRegPrescale = 0xFE;

    struct Ch {
        bool  attached = false;
        float cur = 90.f, tgt = 90.f;
        float speed  = 0.f;          // deg/s, 0 = snap
        int   min_us = 500, max_us = 2500;
        bool  dirty  = false;
        int   last_counts = -1;      // last written OFF count (skip no-ops)
    };

    bool wr(const uint8_t* buf, size_t n) {
        return fd_ >= 0 && ::write(fd_, buf, n) == static_cast<ssize_t>(n);
    }
    bool wr_reg(uint8_t reg, uint8_t val) {
        const uint8_t b[2] = { reg, val };
        return wr(b, 2);
    }

    // Open + initialise: 50 Hz servo frame, auto-increment, all outputs off
    // (channels stay detached until their first move). Returns false quietly
    // when the board isn't there yet.
    bool ensure_device() {
        if (online_.load()) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_probe_ < std::chrono::seconds(5)) return false;
        last_probe_ = now;
        if (fd_ < 0) {
            fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR | O_CLOEXEC);
            if (fd_ < 0) {
                warn_once("open %s failed: %s", cfg_.i2c_bus.c_str(),
                          std::strerror(errno));
                return false;
            }
            if (::ioctl(fd_, I2C_SLAVE, cfg_.i2c_addr) < 0) {
                ::close(fd_); fd_ = -1;
                warn_once("addressing 0x%02x failed", cfg_.i2c_addr);
                return false;
            }
        }
        // Probe: MODE1 must read back.
        uint8_t reg = kRegMode1, mode1 = 0;
        if (::write(fd_, &reg, 1) != 1 || ::read(fd_, &mode1, 1) != 1) {
            warn_once("PCA9685 not found at 0x%02x on %s (retrying every 5 s)",
                      cfg_.i2c_addr, cfg_.i2c_bus.c_str());
            return false;
        }
        // SLEEP to set the prescaler (writable only while asleep), then wake
        // with auto-increment and restart PWM. 25 MHz / (4096 × 50 Hz) − 1.
        if (!wr_reg(kRegMode1, 0x30)) return false;      // SLEEP | AI
        if (!wr_reg(kRegPrescale, 121)) return false;    // ≈50.04 Hz frame
        if (!wr_reg(kRegMode1, 0x20)) return false;      // AI, wake
        std::this_thread::sleep_for(std::chrono::microseconds(600));
        if (!wr_reg(kRegMode1, 0xA0)) return false;      // RESTART | AI
        wr_reg(kRegAllOffH, 0x10);                       // every output off
        {   // force a rewrite of whatever poses are already commanded
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& c : ch_) { c.dirty = true; c.last_counts = -1; }
        }
        std::fprintf(stderr, "[servo] PCA9685 online at 0x%02x on %s\n",
                     cfg_.i2c_addr, cfg_.i2c_bus.c_str());
        warned_ = false;
        online_.store(true);
        return true;
    }

    void write_channel(int ch, const Ch& c) {
        const uint8_t base = static_cast<uint8_t>(kRegLed0OnL + 4 * ch);
        if (!c.attached) {                                // full-off bit
            const uint8_t b[5] = { base, 0, 0, 0, 0x10 };
            if (!wr(b, 5)) drop_device();
            return;
        }
        const int lo = std::min(c.min_us, c.max_us);
        const int hi = std::max(c.min_us, c.max_us);
        const double us = lo + (hi - lo) * (c.cur / 180.0);
        // 4096 counts per 20 ms frame.
        int counts = static_cast<int>(std::lround(us * 4096.0 / 20000.0));
        counts = std::clamp(counts, 0, 4095);
        const uint8_t b[5] = { base, 0, 0,
                               static_cast<uint8_t>(counts & 0xFF),
                               static_cast<uint8_t>((counts >> 8) & 0x0F) };
        if (!wr(b, 5)) drop_device();
    }

    void drop_device() {
        if (online_.exchange(false))
            std::fprintf(stderr, "[servo] PCA9685 write failed — bus lost, "
                                 "re-probing every 5 s\n");
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    void run() {
        using clock = std::chrono::steady_clock;
        constexpr auto period = std::chrono::milliseconds(15);   // ≈66 Hz
        auto next = clock::now() + period;
        while (running_.load()) {
            if (ensure_device()) {
                // Ease under the lock (cheap), then do I²C unlocked.
                Ch snap[kChannels];
                bool todo[kChannels] = {};
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    for (int i = 0; i < kChannels; ++i) {
                        Ch& c = ch_[i];
                        if (c.attached && c.cur != c.tgt) {
                            if (c.speed <= 0.f) c.cur = c.tgt;   // snap
                            else {
                                const float step = c.speed * 0.015f;
                                const float d    = c.tgt - c.cur;
                                c.cur = (std::fabs(d) <= step)
                                            ? c.tgt : c.cur + std::copysign(step, d);
                            }
                            c.dirty = true;
                        }
                        if (c.dirty) {
                            c.dirty = false;
                            snap[i] = c;
                            todo[i] = true;
                        }
                    }
                }
                for (int i = 0; i < kChannels && online_.load(); ++i) {
                    if (!todo[i]) continue;
                    // Skip writes that quantise to the pulse already output.
                    const int lo = std::min(snap[i].min_us, snap[i].max_us);
                    const int hi = std::max(snap[i].min_us, snap[i].max_us);
                    const int counts = snap[i].attached
                        ? std::clamp(static_cast<int>(std::lround(
                              (lo + (hi - lo) * (snap[i].cur / 180.0)) *
                              4096.0 / 20000.0)), 0, 4095)
                        : -2;
                    if (counts == ch_[i].last_counts) continue;
                    write_channel(i, snap[i]);
                    std::lock_guard<std::mutex> lk(mtx_);
                    ch_[i].last_counts = counts;
                }
            }
            std::this_thread::sleep_until(next);
            next += period;
        }
    }

    template <typename... A>
    void warn_once(const char* fmt, A... args) {
        if (warned_) return;
        warned_ = true;
        std::string f = std::string("[servo] ") + fmt + "\n";
        std::fprintf(stderr, f.c_str(), args...);
    }

    Config             cfg_;
    int                fd_ = -1;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  online_{false};
    bool               warned_ = false;
    std::chrono::steady_clock::time_point last_probe_{};
    std::mutex         mtx_;
    Ch                 ch_[kChannels];
    std::thread        thr_;
};

} // namespace servo

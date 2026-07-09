#include "apds9960.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sensor {

// Register map (APDS-9960 datasheet).
namespace reg {
constexpr uint8_t ENABLE  = 0x80;   // PON|PEN|GEN below
constexpr uint8_t ID      = 0x92;   // 0xAB genuine; some clones report 0xA8/0x9C
constexpr uint8_t PDATA   = 0x9C;   // proximity, 0..255
constexpr uint8_t GPENTH  = 0xA0;   // gesture engine entry threshold (prox)
constexpr uint8_t GEXTH   = 0xA1;   // gesture engine exit threshold
constexpr uint8_t GCONF1  = 0xA2;
constexpr uint8_t GCONF2  = 0xA3;   // GGAIN[6:5] GLDRIVE[4:3] GWTIME[2:0]
constexpr uint8_t GPULSE  = 0xA6;   // pulse count/length
constexpr uint8_t GCONF4  = 0xAB;   // GMODE bit0
constexpr uint8_t GFLVL   = 0xAE;   // datasets waiting in the FIFO
constexpr uint8_t GSTATUS = 0xAF;   // GVALID bit0
constexpr uint8_t GFIFO_U = 0xFC;   // U,D,L,R burst
constexpr uint8_t PON = 0x01, PEN = 0x04, GEN = 0x40;
}  // namespace reg

bool Apds9960::wr(uint8_t r, uint8_t v) {
    uint8_t b[2] = { r, v };
    return ::write(fd_, b, 2) == 2;
}
int Apds9960::rd(uint8_t r) {
    if (::write(fd_, &r, 1) != 1) return -1;
    uint8_t v = 0;
    if (::read(fd_, &v, 1) != 1) return -1;
    return v;
}
int Apds9960::rd_block(uint8_t r, uint8_t* buf, int n) {
    if (::write(fd_, &r, 1) != 1) return -1;
    return static_cast<int>(::read(fd_, buf, n));
}

bool Apds9960::init_chip() {
    const int id = rd(reg::ID);
    if (id < 0) return false;
    if (id != 0xAB && id != 0xA8 && id != 0x9C)
        fprintf(stderr, "[apds9960] unexpected ID 0x%02X (continuing)\n", id);
    wr(reg::ENABLE, 0x00);                  // everything off while configuring
    wr(reg::GPENTH, 40);                    // enter gesture engine when close…
    wr(reg::GEXTH,  30);                    // …leave when the hand withdraws
    wr(reg::GCONF1, 0x40);                  // FIFO threshold 4 datasets
    wr(reg::GCONF2, 0x41);                  // 4x gain, 100 mA, 2.8 ms wait
    wr(reg::GPULSE, 0xC9);                  // 32 µs, 10 pulses
    wr(reg::GCONF4, 0x00);
    return wr(reg::ENABLE, reg::PON | reg::PEN | reg::GEN);
}

Apds9960::Gesture Apds9960::rotate(Gesture g) const {
    static constexpr Gesture cw[4] = {          // one 90° clockwise step:
        Gesture::Right, Gesture::Left,          // up→right, down→left
        Gesture::Up,    Gesture::Down };        // left→up,  right→down
    int steps = ((cfg_.gesture_rotation / 90) % 4 + 4) % 4;
    while (steps--) g = cw[static_cast<int>(g)];
    return g;
}

bool Apds9960::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    fd_ = ::open(cfg_.i2c_bus.c_str(), O_RDWR);
    if (fd_ < 0) { perror("[apds9960] open i2c"); return false; }
    if (ioctl(fd_, I2C_SLAVE, cfg_.i2c_addr) < 0 || !init_chip()) {
        fprintf(stderr, "[apds9960] init failed on %s@0x%02X\n",
                cfg_.i2c_bus.c_str(), cfg_.i2c_addr);
        ::close(fd_); fd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&Apds9960::poll_loop, this);
    return true;
}

void Apds9960::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { wr(reg::ENABLE, 0x00); ::close(fd_); fd_ = -1; }
}

void Apds9960::poll_loop() {
    const auto period = std::chrono::microseconds(
        static_cast<int64_t>(1e6 / std::max(5.0f, cfg_.poll_hz)));
    bool   near = false;
    bool   in_gesture = false;
    double first_ud = 0, first_lr = 0, last_ud = 0, last_lr = 0;
    bool   have_first = false;

    while (running_.load()) {
        // Proximity edge with hysteresis (near at threshold, far at 60% of it).
        const int p = rd(reg::PDATA);
        if (p >= 0) {
            if (!near && p >= cfg_.near_threshold)      { near = true;  if (prox_cb_) prox_cb_(true);  }
            else if (near && p < cfg_.near_threshold * 3 / 5) { near = false; if (prox_cb_) prox_cb_(false); }
        }

        // Gesture FIFO: while GVALID, drain datasets and track the normalised
        // U-D / L-R balance of the first and last strong samples. When the
        // engine goes idle after a run, the first→last delta names the swipe.
        const int gst = rd(reg::GSTATUS);
        if (gst >= 0 && (gst & 0x01)) {
            int lvl = rd(reg::GFLVL);
            while (lvl > 0 && running_.load()) {
                uint8_t fifo[4 * 32];
                const int take = std::min(lvl, 32);
                if (rd_block(reg::GFIFO_U, fifo, take * 4) != take * 4) break;
                for (int i = 0; i < take; ++i) {
                    const double u = fifo[i*4+0], d = fifo[i*4+1],
                                 l = fifo[i*4+2], r = fifo[i*4+3];
                    if (u + d + l + r < 40.0) continue;      // too weak to trust
                    const double ud = (u - d) / (u + d + 1.0);
                    const double lr = (l - r) / (l + r + 1.0);
                    if (!have_first) { first_ud = ud; first_lr = lr; have_first = true; }
                    last_ud = ud; last_lr = lr;
                    in_gesture = true;
                }
                lvl = rd(reg::GFLVL);
            }
        } else if (in_gesture) {
            // Engine idle again → classify the completed swipe.
            const double dud = last_ud - first_ud;
            const double dlr = last_lr - first_lr;
            if (std::fabs(dud) > 0.13 || std::fabs(dlr) > 0.13) {
                Gesture g = (std::fabs(dud) > std::fabs(dlr))
                                ? (dud > 0 ? Gesture::Up   : Gesture::Down)
                                : (dlr > 0 ? Gesture::Left : Gesture::Right);
                if (gest_cb_) gest_cb_(rotate(g));
            }
            in_gesture = false;
            have_first = false;
        }

        std::this_thread::sleep_for(period);
    }
}

}  // namespace sensor

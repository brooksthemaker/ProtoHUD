#include "coproc_inputs.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

// Line-based USB-CDC reader for an optional button coprocessor. Protocol v1
// (newline-delimited ASCII, see docs/coprocessor-input.md):
//   coproc → Pi : "HELLO ...", "BTN <id> SHORT|LONG", "PING", (DOWN/UP advisory)
// The reader thread owns the transport, reconnects on drop, and treats every
// inbound byte as untrusted: lines are length-bounded and unknown/malformed
// lines are ignored. Resolved presses go through the shared GpioFunc dispatch.

namespace input {

namespace {
// Map an integer baud to the matching termios speed constant. Falls back to
// 115200 (the documented default) for anything unrecognised.
speed_t baud_constant(int baud) {
    switch (baud) {
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 921600:  return B921600;
        default:      return B115200;
    }
}
constexpr size_t kMaxLine = 256;   // drop pathologically long lines (noise/garbage)
}  // namespace

CoprocInputs::CoprocInputs(CoprocConfig cfg, std::function<void(GpioFunc)> dispatch)
    : cfg_(std::move(cfg)), dispatch_(std::move(dispatch)) {}

CoprocInputs::~CoprocInputs() { shutdown(); }

bool CoprocInputs::init() {
    if (!cfg_.enabled) return false;
    if (cfg_.transport == "i2c") {
        // I²C + data-ready IRQ transport is specified in docs/coprocessor-input.md
        // but not implemented in v1. Report cleanly so the menu shows "offline"
        // rather than silently doing nothing.
        std::cerr << "[coproc] i2c transport not implemented in v1; use usb_serial\n";
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&CoprocInputs::reader_loop, this);
    std::cout << "[coproc] button coprocessor source started (" << cfg_.device << ")\n";
    return true;
}

void CoprocInputs::shutdown() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    connected_.store(false);
}

void CoprocInputs::reader_loop() {
    using clock = std::chrono::steady_clock;
    std::string buf;
    auto last_rx = clock::now();

    while (running_.load()) {
        // (Re)open the serial device if needed.
        if (fd_ < 0) {
            fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd_ < 0) {
                connected_.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));  // retry backoff
                continue;
            }
            termios tio{};
            if (tcgetattr(fd_, &tio) == 0) {
                cfmakeraw(&tio);
                const speed_t spd = baud_constant(cfg_.baud);
                cfsetispeed(&tio, spd);
                cfsetospeed(&tio, spd);
                tio.c_cflag |= (CLOCAL | CREAD);
                tio.c_cc[VMIN]  = 0;
                tio.c_cc[VTIME] = 0;
                tcsetattr(fd_, TCSANOW, &tio);
            }
            buf.clear();
            last_rx = clock::now();
            pins_pushed_ = false;   // re-push the pin map on the fresh connection
        }

        pollfd pfd{fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 200);   // 200 ms — lets us check running_/heartbeat
        if (pr < 0) {
            if (errno == EINTR) continue;
            ::close(fd_); fd_ = -1; connected_.store(false);
            continue;
        }
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            ::close(fd_); fd_ = -1; connected_.store(false);   // device unplugged
            continue;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char chunk[256];
            const ssize_t n = ::read(fd_, chunk, sizeof chunk);
            if (n > 0) {
                last_rx = clock::now();
                for (ssize_t i = 0; i < n; ++i) {
                    const char c = chunk[i];
                    if (c == '\n' || c == '\r') {
                        if (!buf.empty()) { on_line(buf); buf.clear(); }
                    } else if (buf.size() < kMaxLine) {
                        buf.push_back(c);
                    } else {
                        buf.clear();   // overflow → resync on next newline
                    }
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(fd_); fd_ = -1; connected_.store(false);
                continue;
            }
        }

        // Heartbeat: no traffic within the window → treat as offline (but keep
        // the fd open; a quiet-but-present coproc recovers on the next byte).
        const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - last_rx).count();
        if (idle > cfg_.heartbeat_timeout_ms) connected_.store(false);
    }
}

void CoprocInputs::on_line(const std::string& line) {
    // Tokenise on spaces (cheap, no allocations beyond the substrings).
    auto next_tok = [](const std::string& s, size_t& pos) -> std::string {
        while (pos < s.size() && s[pos] == ' ') ++pos;
        const size_t start = pos;
        while (pos < s.size() && s[pos] != ' ') ++pos;
        return s.substr(start, pos - start);
    };
    size_t pos = 0;
    const std::string cmd = next_tok(line, pos);
    if (cmd.empty()) return;

    if (cmd == "BTN") {
        const std::string id_s = next_tok(line, pos);
        const std::string evt  = next_tok(line, pos);
        if (id_s.empty() || evt.empty()) return;
        int id = -1;
        try { id = std::stoi(id_s); } catch (...) { return; }   // malformed id → ignore
        if (id < 0) return;
        connected_.store(true);
        if      (evt == "SHORT") handle_button(id, /*is_long=*/false);
        else if (evt == "LONG")  handle_button(id, /*is_long=*/true);
        // DOWN/UP are advisory in v1 — ignored.
        return;
    }
    if (cmd == "HELLO") {
        connected_.store(true);
        std::cout << "[coproc] " << line << "\n";
        // Push the configured pin map once per connection. The firmware re-HELLOs
        // after applying it; pins_pushed_ stops that from looping.
        if (!pins_pushed_ && !cfg_.pins.empty()) push_pin_config();
        return;
    }
    if (cmd == "PING") {
        connected_.store(true);
        if (fd_ >= 0) { const char* pong = "PONG\n"; (void)::write(fd_, pong, 5); }
        return;
    }
    if (cmd == "I2C") {   // "I2C <hex> <hex> …" or "I2C none" — I2CSCAN reply
        connected_.store(true);
        while (pos < line.size() && line[pos] == ' ') ++pos;
        std::lock_guard<std::mutex> lk(i2c_mtx_);
        i2c_result_ = (pos < line.size()) ? line.substr(pos) : std::string("none");
        return;
    }
    if (cmd == "BOOP") {  // "BOOP <electrode> <1|0>" — touch edge (MPR121 or TTP223)
        const std::string e_s = next_tok(line, pos);
        const std::string s_s = next_tok(line, pos);
        if (e_s.empty() || s_s.empty()) return;
        int e = -1;
        try { e = std::stoi(e_s); } catch (...) { return; }
        if (e < 0 || e > 11) return;
        connected_.store(true);
        const bool touched = (s_s == "1");
        if (boop_fn_) boop_fn_(e, touched);
        // Touch pads can double as buttons: fire the mapped function on the
        // touch-down edge (release does nothing, like a SHORT press).
        if (touched) {
            const auto it = cfg_.touch_map.find(e);
            if (it != cfg_.touch_map.end() && it->second != GpioFunc::None && dispatch_)
                dispatch_(it->second);
        }
        return;
    }
    if (cmd == "ADC") {   // "ADC <ch> <raw> <mv>" — ADCREAD reply, one per channel
        const std::string c_s = next_tok(line, pos);
        (void)next_tok(line, pos);                     // raw counts (unused here)
        const std::string m_s = next_tok(line, pos);
        if (c_s.empty() || m_s.empty()) return;
        int ch = -1; long mv = -1;
        try { ch = std::stoi(c_s); mv = std::stol(m_s); } catch (...) { return; }
        if (ch < 0 || ch > 2 || mv < 0 || mv > 5000) return;
        connected_.store(true);
        std::lock_guard<std::mutex> lk(adc_mtx_);
        adc_mv_[ch] = static_cast<int>(mv);
        return;
    }
    if (cmd == "TEMP") {  // "TEMP <rom16hex> <milli°C>" — one DS18B20 reading
        const std::string id_s = next_tok(line, pos);
        const std::string m_s  = next_tok(line, pos);
        if (id_s.empty() || id_s.size() > 20 || m_s.empty()) return;
        long milli = 0;
        try { milli = std::stol(m_s); } catch (...) { return; }
        connected_.store(true);
        std::lock_guard<std::mutex> lk(temp_mtx_);
        temps_[id_s] = { milli / 1000.0, std::chrono::steady_clock::now() };
        return;
    }
    // Unknown command → ignore (forward-compatible).
}

void CoprocInputs::push_pin_config() {
    if (fd_ < 0) return;
    std::string msg = "PINCFG CLR\n";
    for (const CoprocPin& p : cfg_.pins) {
        if (p.gp < 0) continue;
        msg += "PINCFG BTN " + std::to_string(p.gp) + " " + p.pull + " " +
               (p.active_low ? "1" : "0") + "\n";
    }
    // LED lines reference the button id (index), so emit them after all BTNs.
    for (size_t i = 0; i < cfg_.pins.size(); ++i)
        if (cfg_.pins[i].led_gp >= 0)
            msg += "PINCFG LED " + std::to_string(i) + " " +
                   std::to_string(cfg_.pins[i].led_gp) + "\n";
    msg += "PINCFG APPLY\n";
    (void)::write(fd_, msg.data(), msg.size());
    pins_pushed_ = true;
    std::cout << "[coproc] pushed pin map (" << cfg_.pins.size() << " buttons)\n";
}

void CoprocInputs::request_i2c_scan(int sda, int scl) {
    if (fd_ < 0) return;
    std::string cmd = "I2CSCAN";
    if (sda >= 0 && scl >= 0)
        cmd += " " + std::to_string(sda) + " " + std::to_string(scl);
    cmd += "\n";
    { std::lock_guard<std::mutex> lk(i2c_mtx_); i2c_result_ = "scanning…"; }
    (void)::write(fd_, cmd.data(), cmd.size());
}

std::string CoprocInputs::i2c_scan_result() const {
    std::lock_guard<std::mutex> lk(i2c_mtx_);
    return i2c_result_;
}

bool CoprocInputs::coproc_temp(const std::string& rom_id, double& c_out) const {
    std::lock_guard<std::mutex> lk(temp_mtx_);
    const auto it = temps_.find(rom_id);
    if (it == temps_.end()) return false;
    // Stale after ~3 firmware periods (probe unplugged / bus fault).
    if (std::chrono::steady_clock::now() - it->second.at > std::chrono::seconds(8))
        return false;
    c_out = it->second.c;
    return true;
}

void CoprocInputs::send_fan_duty(int zone, int duty_pct) {
    if (fd_ < 0 || zone < 0) return;
    duty_pct = duty_pct < 0 ? 0 : (duty_pct > 100 ? 100 : duty_pct);
    const std::string msg = "FAN " + std::to_string(zone) + " " +
                            std::to_string(duty_pct) + "\n";
    (void)::write(fd_, msg.data(), msg.size());
}

void CoprocInputs::send_servo(int ch, int deg) {
    if (fd_ < 0 || ch < 0 || ch > 3) return;
    const std::string msg = "SERVO " + std::to_string(ch) + " " +
        (deg < 0 ? std::string("off")
                 : std::to_string(deg > 180 ? 180 : deg)) + "\n";
    (void)::write(fd_, msg.data(), msg.size());
}

void CoprocInputs::send_led_zone(int r, int g, int b, int count) {
    if (fd_ < 0) return;
    auto c8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    std::string msg = "LEDZ " + std::to_string(c8(r)) + " " +
                      std::to_string(c8(g)) + " " + std::to_string(c8(b));
    if (count > 0) msg += " " + std::to_string(count > 300 ? 300 : count);
    msg += "\n";
    (void)::write(fd_, msg.data(), msg.size());
}

void CoprocInputs::send_led_pattern(int mode, int r, int g, int b, int speed) {
    if (fd_ < 0) return;
    auto c8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    const std::string msg = "LEDP " + std::to_string(mode < 0 ? 0 : (mode > 4 ? 4 : mode)) +
        " " + std::to_string(c8(r)) + " " + std::to_string(c8(g)) + " " +
        std::to_string(c8(b)) + " " + std::to_string(speed < 1 ? 50 : c8(speed)) + "\n";
    (void)::write(fd_, msg.data(), msg.size());
}

void CoprocInputs::send_led_brightness(int b) {
    if (fd_ < 0) return;
    const std::string msg = "LEDB " +
        std::to_string(b < 0 ? 0 : (b > 255 ? 255 : b)) + "\n";
    (void)::write(fd_, msg.data(), msg.size());
}

void CoprocInputs::send_led_frame(const uint8_t* rgb, int count) {
    if (fd_ < 0 || !rgb || count <= 0) return;
    if (count > 300) count = 300;
    static const char* kHex = "0123456789ABCDEF";
    // ~80 pixels per line keeps each write well under the firmware's 600-char
    // rx buffer; LEDSHOW latches the assembled frame atomically.
    for (int start = 0; start < count; start += 80) {
        const int n = std::min(80, count - start);
        std::string msg = "LEDF " + std::to_string(start) + " ";
        msg.reserve(msg.size() + n * 6 + 1);
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 3; ++ch) {
                const uint8_t v = rgb[(start + i) * 3 + ch];
                msg += kHex[v >> 4];
                msg += kHex[v & 0xF];
            }
        msg += "\n";
        (void)::write(fd_, msg.data(), msg.size());
    }
    const char* show = "LEDSHOW\n";
    (void)::write(fd_, show, 8);
}

void CoprocInputs::request_adc() {
    if (fd_ < 0) return;
    { std::lock_guard<std::mutex> lk(adc_mtx_); adc_mv_[0] = adc_mv_[1] = adc_mv_[2] = -1; }
    const char* cmd = "ADCREAD\n";
    (void)::write(fd_, cmd, 8);
}

std::string CoprocInputs::adc_result() const {
    std::lock_guard<std::mutex> lk(adc_mtx_);
    if (adc_mv_[0] < 0 && adc_mv_[1] < 0 && adc_mv_[2] < 0) return "no reading";
    std::string out;
    for (int ch = 0; ch < 3; ++ch) {
        if (ch) out += "  ";
        out += "ch" + std::to_string(ch) + " ";
        out += adc_mv_[ch] < 0 ? std::string("--")
                               : std::to_string(adc_mv_[ch]) + "mV";
    }
    return out;
}

void CoprocInputs::handle_button(int id, bool is_long) {
    const auto& map = is_long ? cfg_.long_map : cfg_.short_map;
    const auto it = map.find(id);
    const GpioFunc f = (it != map.end()) ? it->second : GpioFunc::None;
    if (f != GpioFunc::None && dispatch_) dispatch_(f);
}

}  // namespace input

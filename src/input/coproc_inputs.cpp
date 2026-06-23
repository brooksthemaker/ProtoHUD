#include "coproc_inputs.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

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
            line_buf_.clear();
            frame_.clear();
            last_rx = clock::now();
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
                for (ssize_t i = 0; i < n; ++i)
                    process_byte(static_cast<uint8_t>(chunk[i]));
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

// Dual-mode demux: a 0xAA byte begins a v2 binary frame; anything else feeds the
// v1 ASCII line accumulator. HELLO/PING stay ASCII for board detection + the
// heartbeat, so both protocol generations share one stream.
void CoprocInputs::process_byte(uint8_t c) {
    if (!frame_.empty()) {                       // mid-frame
        frame_.push_back(c);
        if (frame_.size() == 2 && frame_[1] != cp::MAGIC1) {
            // 0xAA wasn't a frame start after all — drop it, re-handle this byte.
            const uint8_t b = c;
            frame_.clear();
            process_byte(b);
            return;
        }
        if (frame_.size() >= cp::WIRE_HEADER) {
            const uint16_t len = static_cast<uint16_t>(frame_[3]) |
                                 (static_cast<uint16_t>(frame_[4]) << 8);
            if (len > cp::MAX_PAYLOAD) { frame_.clear(); return; }   // garbage → resync
            const size_t total = cp::WIRE_HEADER + len + cp::CRC_LEN;
            if (frame_.size() == total) {
                const uint16_t want = cp::crc16(&frame_[2],
                                                static_cast<uint16_t>(3 + len));
                const uint16_t got  = static_cast<uint16_t>(frame_[cp::WIRE_HEADER + len]) |
                                      (static_cast<uint16_t>(frame_[cp::WIRE_HEADER + len + 1]) << 8);
                if (want == got)
                    on_frame(frame_[2], &frame_[cp::WIRE_HEADER], len);
                frame_.clear();
            }
        }
        return;
    }
    if (c == cp::MAGIC0) { frame_.push_back(c); return; }
    ascii_byte(c);
}

void CoprocInputs::ascii_byte(uint8_t c) {
    if (c == '\n' || c == '\r') {
        if (!line_buf_.empty()) { on_line(line_buf_); line_buf_.clear(); }
    } else if (line_buf_.size() < kMaxLine) {
        line_buf_.push_back(static_cast<char>(c));
    } else {
        line_buf_.clear();   // overflow → resync on next newline
    }
}

// Decode one validated v2 frame and route it. Payloads are memcpy'd out of the
// (unaligned) frame buffer into the packed structs before use.
void CoprocInputs::on_frame(uint8_t cmd, const uint8_t* payload, uint16_t len) {
    connected_.store(true);
    auto take = [&](void* dst, size_t sz) -> bool {
        if (len < sz) return false;
        std::memcpy(dst, payload, sz);
        return true;
    };
    switch (cmd) {
        case cp::RSP_IMU_BNO: { cp::BnoPayload p;   if (take(&p, sizeof p) && on_bno_)   on_bno_(p);   break; }
        case cp::RSP_IMU_MPU: { cp::MpuPayload p;   if (take(&p, sizeof p) && on_mpu_)   on_mpu_(p);   break; }
        case cp::RSP_BOOP:    { cp::BoopPayload p;  if (take(&p, sizeof p) && on_boop_)  on_boop_(p);  break; }
        case cp::RSP_LIGHT:   { cp::LightPayload p; if (take(&p, sizeof p) && on_light_) on_light_(p); break; }
        case cp::RSP_BTN:     { cp::BtnPayload p;   if (take(&p, sizeof p)) handle_button(p.id, p.kind == cp::BTN_LONG); break; }
        case cp::RSP_STATUS:  { cp::StatusPayload p; if (take(&p, sizeof p)) caps_.store(p.caps); break; }
        default: break;   // unknown cmd → ignore (forward-compatible)
    }
}

// "HELLO proto-coproc v2 caps=buttons,imu_bno,... n_btn=8 n_chain=2"
void CoprocInputs::parse_hello_caps(const std::string& line) {
    const auto pos = line.find("caps=");
    if (pos == std::string::npos) return;
    size_t s = pos + 5;
    size_t e = line.find(' ', s);
    const std::string list = line.substr(s, e == std::string::npos ? std::string::npos : e - s);
    uint16_t caps = 0;
    auto has = [&](const char* tok){ return list.find(tok) != std::string::npos; };
    if (has("buttons")) caps |= cp::CAP_BUTTONS;
    if (has("imu_bno")) caps |= cp::CAP_IMU_BNO;
    if (has("imu_mpu")) caps |= cp::CAP_IMU_MPU;
    if (has("boop"))    caps |= cp::CAP_BOOP;
    if (has("light"))   caps |= cp::CAP_LIGHT;
    if (has("panels"))  caps |= cp::CAP_PANELS;
    caps_.store(caps);
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
        parse_hello_caps(line);
        std::cout << "[coproc] " << line << "\n";
        return;
    }
    if (cmd == "PING") {
        connected_.store(true);
        if (fd_ >= 0) { const char* pong = "PONG\n"; (void)::write(fd_, pong, 5); }
        return;
    }
    // Unknown command → ignore (forward-compatible).
}

void CoprocInputs::handle_button(int id, bool is_long) {
    const auto& map = is_long ? cfg_.long_map : cfg_.short_map;
    const auto it = map.find(id);
    const GpioFunc f = (it != map.end()) ? it->second : GpioFunc::None;
    if (f != GpioFunc::None && dispatch_) dispatch_(f);
}

}  // namespace input

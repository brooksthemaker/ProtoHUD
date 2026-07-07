#include "cmd_fifo.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace input {

namespace {
constexpr size_t kMaxLine = 128;   // command ids are short; drop garbage lines
}

CmdFifo::CmdFifo(CmdFifoConfig cfg, std::function<void(GpioFunc)> dispatch)
    : cfg_(std::move(cfg)), dispatch_(std::move(dispatch)) {}

CmdFifo::~CmdFifo() { stop(); }

bool CmdFifo::start() {
    if (!cfg_.enabled || cfg_.path.empty()) return false;

    // Make sure the parent directory and the FIFO itself exist.
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(cfg_.path).parent_path(), ec);
    struct stat st{};
    if (::stat(cfg_.path.c_str(), &st) != 0) {
        if (::mkfifo(cfg_.path.c_str(), 0666) != 0 && errno != EEXIST) {
            std::cerr << "[cmdfifo] mkfifo " << cfg_.path << " failed: "
                      << std::strerror(errno) << "\n";
            return false;
        }
    } else if (!S_ISFIFO(st.st_mode)) {
        std::cerr << "[cmdfifo] " << cfg_.path << " exists but is not a FIFO\n";
        return false;
    }

    // Open O_RDWR so the FIFO always has a writer end too — on Linux that means
    // poll() never sees POLLHUP (no busy-spin) and the fd survives writers coming
    // and going. O_NONBLOCK keeps reads from blocking the poll loop.
    fd_ = ::open(cfg_.path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        std::cerr << "[cmdfifo] open " << cfg_.path << " failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&CmdFifo::reader_loop, this);
    std::cout << "[cmdfifo] watching " << cfg_.path
              << " (write a GpioFunc id, e.g. `echo menu_open > " << cfg_.path << "`)\n";
    return true;
}

void CmdFifo::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void CmdFifo::reader_loop() {
    std::string buf;
    while (running_.load()) {
        pollfd pfd{fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 200);   // 200 ms — lets us check running_
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        char chunk[256];
        const ssize_t n = ::read(fd_, chunk, sizeof chunk);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            continue;   // O_RDWR keeps the fd open; nothing fatal here
        }
        for (ssize_t i = 0; i < n; ++i) {
            const char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (!buf.empty()) {
                    // Parametric commands (e.g. "max_text:HI") get first refusal;
                    // otherwise resolve the line as a GpioFunc id.
                    if (!(raw_handler_ && raw_handler_(buf))) {
                        const GpioFunc f = gpio_func_from_id(buf);
                        if (f != GpioFunc::None && dispatch_) dispatch_(f);
                    }
                    buf.clear();
                }
            } else if (buf.size() < kMaxLine) {
                buf.push_back(c);
            } else {
                buf.clear();   // overflow → resync on the next newline
            }
        }
    }
}

}  // namespace input

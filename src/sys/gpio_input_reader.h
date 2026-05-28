#pragma once
// ── gpio_input_reader.h ───────────────────────────────────────────────────────
// Lightweight read-only GPIO snapshot helper for the System > GPIO Visualizer.
// Opens /dev/gpiochip0, attempts to claim each requested BCM offset as an
// input line, and exposes a per-frame read() that returns the current logic
// level. Lines already claimed by another consumer (SPI driver, piomatter,
// MAX7219 chain, etc.) silently fail to claim and report no reading — that's
// the expected case for active hardware, so we don't spam logs.

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sys {

class GpioInputReader {
public:
    GpioInputReader() = default;
    ~GpioInputReader() { close(); }

    GpioInputReader(const GpioInputReader&)            = delete;
    GpioInputReader& operator=(const GpioInputReader&) = delete;

    // Claim each BCM offset in `bcm_pins` as an input on `chip_dev`. Each
    // line is requested individually so a single busy pin doesn't kill the
    // whole batch. After open(), `read(bcm)` returns 0/1 for claimed lines
    // and -1 for any line we couldn't claim.
    bool open(const std::vector<int>& bcm_pins,
              const std::string& chip_dev = "/dev/gpiochip0") {
        close();
        if (bcm_pins.empty()) return true;
        int chip = ::open(chip_dev.c_str(), O_RDWR | O_CLOEXEC);
        if (chip < 0) return false;
        for (int bcm : bcm_pins) {
            if (bcm < 0) continue;
            gpio_v2_line_request req{};
            req.offsets[0]   = static_cast<uint32_t>(bcm);
            req.num_lines    = 1;
            req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
            std::strncpy(req.consumer, "protohud-viz", sizeof(req.consumer) - 1);
            if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) == 0 && req.fd >= 0) {
                line_fds_[bcm] = req.fd;
            }
        }
        ::close(chip);
        return true;
    }

    void close() {
        for (auto& [bcm, fd] : line_fds_) if (fd >= 0) ::close(fd);
        line_fds_.clear();
    }

    // 0 = low, 1 = high, -1 = unclaimable (busy elsewhere or no chip).
    int read(int bcm) const {
        auto it = line_fds_.find(bcm);
        if (it == line_fds_.end() || it->second < 0) return -1;
        gpio_v2_line_values vals{};
        vals.mask = 1ULL;
        if (ioctl(it->second, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0)
            return -1;
        return (vals.bits & 1ULL) ? 1 : 0;
    }

    bool claimed(int bcm) const {
        auto it = line_fds_.find(bcm);
        return it != line_fds_.end() && it->second >= 0;
    }

private:
    std::unordered_map<int, int> line_fds_;   // bcm → fd (-1 = failed)
};

} // namespace sys

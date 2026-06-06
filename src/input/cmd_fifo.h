#pragma once
// ── cmd_fifo.h ───────────────────────────────────────────────────────────────
// Reads newline-delimited GpioFunc id strings (e.g. "menu_open",
// "cam_capture_left", "phone_ring") from a local FIFO and dispatches each through
// the SAME input::GpioFunc path as the GPIO poller and button coprocessor. This
// lets any local tool drive ProtoHUD by writing a line — most usefully a KDE
// Connect "Run Command" that does:
//     echo menu_open > /run/protohud/cmd
// so the paired phone becomes a remote. Bytes from the FIFO are treated as
// untrusted: lines are length-bounded and unknown ids resolve to GpioFunc::None
// (no-op). Never required — disabled unless cfg.enabled.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "gpio_function.h"

namespace input {

struct CmdFifoConfig {
    bool        enabled = false;
    std::string path    = "/run/protohud/cmd";   // created (mkfifo) if absent
};

class CmdFifo {
public:
    CmdFifo(CmdFifoConfig cfg, std::function<void(GpioFunc)> dispatch);
    ~CmdFifo();

    bool start();   // create the FIFO + spawn the reader thread; false on failure
    void stop();
    bool running() const { return running_.load(); }
    const std::string& path() const { return cfg_.path; }

private:
    void reader_loop();

    CmdFifoConfig                 cfg_;
    std::function<void(GpioFunc)> dispatch_;
    std::atomic<bool>             running_{false};
    std::thread                   thread_;
    int                           fd_ = -1;
};

}  // namespace input

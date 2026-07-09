#pragma once
// ── pty_terminal.h ────────────────────────────────────────────────────────────
// A real shell in a pty, parsed into a fixed character grid the HUD renders
// inside the floating window (HUD > Floating Window > Content: Terminal).
//
// Scope: the practical VT100/xterm subset an interactive shell needs —
// printable ASCII, CR/LF/BS/TAB, CSI cursor motion (A B C D G H d f),
// erase (J K), scroll regions (r, S, T), SGR colors (16-color + the
// 256-color escape mapped down), cursor save/restore, alt-screen clear,
// OSC title sequences consumed. Multi-byte UTF-8 renders as '?' (the grid
// is one byte per cell). Enough for bash/ssh/htop-lite; not a vim daily
// driver.
//
// Threading: a reader thread owns the parser and grid (under mtx_);
// write_input() may be called from any thread (plain write() to the pty fd);
// snapshot() copies the grid out for rendering.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace term {

struct TermCell {
    char    ch   = ' ';
    uint8_t fg   = 16;    // 0-15 ANSI, 16 = default foreground
    uint8_t bg   = 17;    // 0-15 ANSI, 17 = default (transparent) background
    uint8_t attr = 0;     // bit0 bold, bit1 reverse
};

struct TermSnapshot {
    int cols = 0, rows = 0;
    int cur_x = 0, cur_y = 0;
    bool cursor_visible = true;
    bool running = false;
    std::vector<TermCell> cells;   // rows*cols, row-major
};

class PtyTerminal {
public:
    struct Config {
        std::string shell;         // empty = $SHELL, else /bin/bash
        int cols = 80;
        int rows = 24;
    };

    explicit PtyTerminal(Config cfg);
    ~PtyTerminal();

    bool start();                  // spawn shell + reader thread (idempotent)
    void stop();                   // SIGHUP + join
    bool running() const { return running_.load(); }

    // Keyboard bytes → pty (UTF-8 / escape sequences from the key router).
    void write_input(const char* data, size_t len);

    void snapshot(TermSnapshot& out) const;

private:
    void reader_loop();
    void feed(const uint8_t* data, size_t n);      // parser (reader thread)
    void put_char(char c);
    void newline();
    void scroll_up(int top, int bot);              // one line within region
    void scroll_down(int top, int bot);
    void clear_cells(int from, int to);            // linear range, current bg
    void csi_dispatch(char final_byte);
    void sgr();
    TermCell blank_cell() const;
    TermCell& cell(int x, int y) { return grid_[(size_t)y * cfg_.cols + x]; }

    Config cfg_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> stopping_{ false };
    int         pty_fd_ = -1;
    pid_t       child_  = -1;
    std::thread reader_;

    mutable std::mutex mtx_;       // guards everything below
    std::vector<TermCell> grid_;
    int  cur_x_ = 0, cur_y_ = 0;
    int  saved_x_ = 0, saved_y_ = 0;
    int  scroll_top_ = 0, scroll_bot_ = 0;         // inclusive rows
    bool cursor_visible_ = true;
    // current SGR state
    uint8_t fg_ = 16, bg_ = 17, attr_ = 0;
    // escape-parser state
    enum class Ps { Text, Esc, Csi, Osc, OscEsc, Charset };
    Ps  ps_ = Ps::Text;
    std::string csi_;              // accumulated CSI parameter bytes
};

} // namespace term

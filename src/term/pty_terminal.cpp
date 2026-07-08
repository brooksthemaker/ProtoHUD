#include "pty_terminal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace term {

PtyTerminal::PtyTerminal(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.cols = std::max(20, std::min(200, cfg_.cols));
    cfg_.rows = std::max(5,  std::min(60,  cfg_.rows));
    grid_.assign((size_t)cfg_.cols * cfg_.rows, blank_cell());
    scroll_bot_ = cfg_.rows - 1;
}

PtyTerminal::~PtyTerminal() { stop(); }

TermCell PtyTerminal::blank_cell() const {
    TermCell c; c.fg = fg_; c.bg = bg_; return c;
}

bool PtyTerminal::start() {
    if (running_.load()) return true;
    stopping_.store(false);

    struct winsize ws {};
    ws.ws_col = (unsigned short)cfg_.cols;
    ws.ws_row = (unsigned short)cfg_.rows;

    int fd = -1;
    const pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        std::fprintf(stderr, "[term] forkpty failed: %s\n", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child: plain interactive shell. 16-color xterm keeps the escape
        // stream inside what the parser implements.
        ::setenv("TERM", "xterm-16color", 1);
        std::string sh = cfg_.shell;
        if (sh.empty()) {
            const char* env = ::getenv("SHELL");
            sh = (env && *env) ? env : "/bin/bash";
        }
        ::execlp(sh.c_str(), sh.c_str(), "-i", (char*)nullptr);
        ::execlp("/bin/sh", "/bin/sh", "-i", (char*)nullptr);
        _exit(127);
    }

    pty_fd_ = fd;
    child_  = pid;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fg_ = 16; bg_ = 17; attr_ = 0;
        grid_.assign((size_t)cfg_.cols * cfg_.rows, blank_cell());
        cur_x_ = cur_y_ = saved_x_ = saved_y_ = 0;
        scroll_top_ = 0; scroll_bot_ = cfg_.rows - 1;
        cursor_visible_ = true;
        ps_ = Ps::Text; csi_.clear();
    }
    running_.store(true);
    reader_ = std::thread(&PtyTerminal::reader_loop, this);
    return true;
}

void PtyTerminal::stop() {
    if (!running_.load() && child_ < 0) return;
    stopping_.store(true);
    if (child_ > 0) ::kill(child_, SIGHUP);
    if (reader_.joinable()) reader_.join();
    if (pty_fd_ >= 0) { ::close(pty_fd_); pty_fd_ = -1; }
    if (child_ > 0) { ::waitpid(child_, nullptr, WNOHANG); child_ = -1; }
    running_.store(false);
}

void PtyTerminal::write_input(const char* data, size_t len) {
    if (pty_fd_ < 0 || !running_.load()) return;
    (void)!::write(pty_fd_, data, len);
}

void PtyTerminal::snapshot(TermSnapshot& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    out.cols = cfg_.cols; out.rows = cfg_.rows;
    out.cur_x = cur_x_;   out.cur_y = cur_y_;
    out.cursor_visible = cursor_visible_;
    out.running = running_.load();
    out.cells = grid_;
}

void PtyTerminal::reader_loop() {
    uint8_t buf[4096];
    while (!stopping_.load()) {
        struct pollfd pf { pty_fd_, POLLIN, 0 };
        const int pr = ::poll(&pf, 1, 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        const ssize_t n = ::read(pty_fd_, buf, sizeof(buf));
        if (n <= 0) break;                     // shell exited / pty closed
        std::lock_guard<std::mutex> lk(mtx_);
        feed(buf, (size_t)n);
    }
    running_.store(false);
}

// ── parser (mtx_ held) ────────────────────────────────────────────────────────

void PtyTerminal::scroll_up(int top, int bot) {
    for (int y = top; y < bot; ++y)
        std::memcpy(&cell(0, y), &cell(0, y + 1), sizeof(TermCell) * cfg_.cols);
    for (int x = 0; x < cfg_.cols; ++x) cell(x, bot) = blank_cell();
}

void PtyTerminal::scroll_down(int top, int bot) {
    for (int y = bot; y > top; --y)
        std::memcpy(&cell(0, y), &cell(0, y - 1), sizeof(TermCell) * cfg_.cols);
    for (int x = 0; x < cfg_.cols; ++x) cell(x, top) = blank_cell();
}

void PtyTerminal::clear_cells(int from, int to) {
    from = std::max(0, from);
    to   = std::min((int)grid_.size() - 1, to);
    for (int i = from; i <= to; ++i) grid_[(size_t)i] = blank_cell();
}

void PtyTerminal::newline() {
    if (cur_y_ == scroll_bot_) scroll_up(scroll_top_, scroll_bot_);
    else if (cur_y_ < cfg_.rows - 1) ++cur_y_;
}

void PtyTerminal::put_char(char c) {
    if (cur_x_ >= cfg_.cols) { cur_x_ = 0; newline(); }   // deferred wrap
    TermCell& tc = cell(cur_x_, cur_y_);
    tc.ch = c; tc.fg = fg_; tc.bg = bg_; tc.attr = attr_;
    ++cur_x_;
}

void PtyTerminal::sgr() {
    // Parse the accumulated "p1;p2;..." of an SGR (m) sequence.
    int p[16]; int np = 0; int v = 0; bool have = false;
    auto flush = [&]{ if (np < 16) p[np++] = have ? v : 0; v = 0; have = false; };
    for (char ch : csi_) {
        if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); have = true; }
        else if (ch == ';') flush();
        else return;                            // private markers: ignore all
    }
    flush();
    for (int i = 0; i < np; ++i) {
        const int n = p[i];
        if      (n == 0)  { fg_ = 16; bg_ = 17; attr_ = 0; }
        else if (n == 1)  attr_ |= 1;
        else if (n == 22) attr_ &= ~1;
        else if (n == 7)  attr_ |= 2;
        else if (n == 27) attr_ &= ~2;
        else if (n >= 30 && n <= 37)   fg_ = (uint8_t)(n - 30);
        else if (n == 39)              fg_ = 16;
        else if (n >= 40 && n <= 47)   bg_ = (uint8_t)(n - 40);
        else if (n == 49)              bg_ = 17;
        else if (n >= 90 && n <= 97)   fg_ = (uint8_t)(n - 90 + 8);
        else if (n >= 100 && n <= 107) bg_ = (uint8_t)(n - 100 + 8);
        else if ((n == 38 || n == 48) && i + 2 < np && p[i + 1] == 5) {
            // 256-color: 0-15 direct; 16-231 cube → nearest basic by bright
            // channels; 232-255 grayscale → black/white-ish.
            const int c256 = p[i + 2];
            uint8_t mapped;
            if (c256 < 16) mapped = (uint8_t)c256;
            else if (c256 < 232) {
                const int idx = c256 - 16;
                const int r = idx / 36, g = (idx / 6) % 6, b = idx % 6;
                mapped = (uint8_t)(((r >= 3) ? 1 : 0) | ((g >= 3) ? 2 : 0) |
                                   ((b >= 3) ? 4 : 0) |
                                   ((r >= 4 || g >= 4 || b >= 4) ? 8 : 0));
            } else mapped = (c256 >= 244) ? 15 : 8;
            if (n == 38) fg_ = mapped; else bg_ = mapped;
            i += 2;
        }
    }
}

void PtyTerminal::csi_dispatch(char fin) {
    // First numeric parameters (defaults 0), ignoring a leading '?'.
    int p[8] = {}; int np = 0; int v = 0; bool have = false; bool priv = false;
    for (char ch : csi_) {
        if (ch == '?') { priv = true; continue; }
        if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); have = true; }
        else if (ch == ';') { if (np < 8) p[np++] = have ? v : 0; v = 0; have = false; }
    }
    if (np < 8) p[np++] = have ? v : 0;
    auto p1 = [&](int def){ return (np >= 1 && p[0] > 0) ? p[0] : def; };

    const int W = cfg_.cols, H = cfg_.rows;
    switch (fin) {
    case 'A': cur_y_ = std::max(0, cur_y_ - p1(1)); break;
    case 'B': cur_y_ = std::min(H - 1, cur_y_ + p1(1)); break;
    case 'C': cur_x_ = std::min(W - 1, cur_x_ + p1(1)); break;
    case 'D': cur_x_ = std::max(0, cur_x_ - p1(1)); break;
    case 'G': cur_x_ = std::clamp(p1(1) - 1, 0, W - 1); break;
    case 'd': cur_y_ = std::clamp(p1(1) - 1, 0, H - 1); break;
    case 'H': case 'f':
        cur_y_ = std::clamp(((np >= 1 && p[0] > 0) ? p[0] : 1) - 1, 0, H - 1);
        cur_x_ = std::clamp(((np >= 2 && p[1] > 0) ? p[1] : 1) - 1, 0, W - 1);
        break;
    case 'J': {
        const int m = (np >= 1) ? p[0] : 0;
        const int cur = cur_y_ * W + cur_x_;
        if      (m == 0) clear_cells(cur, W * H - 1);
        else if (m == 1) clear_cells(0, cur);
        else             { clear_cells(0, W * H - 1); }
        break;
    }
    case 'K': {
        const int m = (np >= 1) ? p[0] : 0;
        const int row = cur_y_ * W;
        if      (m == 0) clear_cells(row + cur_x_, row + W - 1);
        else if (m == 1) clear_cells(row, row + cur_x_);
        else             clear_cells(row, row + W - 1);
        break;
    }
    case 'r':
        scroll_top_ = std::clamp(((np >= 1 && p[0] > 0) ? p[0] : 1) - 1, 0, H - 1);
        scroll_bot_ = std::clamp(((np >= 2 && p[1] > 0) ? p[1] : H) - 1, 0, H - 1);
        if (scroll_top_ >= scroll_bot_) { scroll_top_ = 0; scroll_bot_ = H - 1; }
        cur_x_ = 0; cur_y_ = scroll_top_;
        break;
    case 'S': for (int i = p1(1); i > 0; --i) scroll_up(scroll_top_, scroll_bot_); break;
    case 'T': for (int i = p1(1); i > 0; --i) scroll_down(scroll_top_, scroll_bot_); break;
    case 'm': sgr(); break;
    case 'h': case 'l': {
        const bool on = (fin == 'h');
        if (priv) {
            if (p[0] == 25) cursor_visible_ = on;
            else if (p[0] == 1049 || p[0] == 47 || p[0] == 1047) {
                // Alt screen enter/leave: we keep one grid — just clear it.
                clear_cells(0, W * H - 1);
                cur_x_ = cur_y_ = 0;
            }
        }
        break;
    }
    default: break;                              // unimplemented: ignore
    }
}

void PtyTerminal::feed(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        switch (ps_) {
        case Ps::Text:
            if      (b == 0x1B) { ps_ = Ps::Esc; }
            else if (b == '\n') newline();
            else if (b == '\r') cur_x_ = 0;
            else if (b == '\b') { if (cur_x_ > 0) --cur_x_; }
            else if (b == '\t') {
                do { put_char(' '); } while (cur_x_ % 8 != 0 && cur_x_ < cfg_.cols);
            }
            else if (b == 0x07) { /* bell */ }
            else if (b >= 0x20 && b < 0x7F) put_char((char)b);
            else if (b >= 0x80) {
                // UTF-8 lead byte: swallow continuations, draw one '?'.
                int cont = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : (b >= 0xC0) ? 1 : 0;
                while (cont-- > 0 && i + 1 < n && (data[i + 1] & 0xC0) == 0x80) ++i;
                put_char('?');
            }
            break;
        case Ps::Esc:
            if      (b == '[') { ps_ = Ps::Csi; csi_.clear(); }
            else if (b == ']') { ps_ = Ps::Osc; }
            else if (b == '(' || b == ')') { ps_ = Ps::Charset; }
            else if (b == '7') { saved_x_ = cur_x_; saved_y_ = cur_y_; ps_ = Ps::Text; }
            else if (b == '8') { cur_x_ = saved_x_; cur_y_ = saved_y_; ps_ = Ps::Text; }
            else if (b == 'M') {                  // reverse index
                if (cur_y_ == scroll_top_) scroll_down(scroll_top_, scroll_bot_);
                else if (cur_y_ > 0) --cur_y_;
                ps_ = Ps::Text;
            }
            else if (b == 'c') {                  // full reset
                fg_ = 16; bg_ = 17; attr_ = 0;
                clear_cells(0, cfg_.cols * cfg_.rows - 1);
                cur_x_ = cur_y_ = 0;
                scroll_top_ = 0; scroll_bot_ = cfg_.rows - 1;
                ps_ = Ps::Text;
            }
            else ps_ = Ps::Text;                  // unknown ESC x: drop
            break;
        case Ps::Csi:
            if (b >= 0x40 && b <= 0x7E) { csi_dispatch((char)b); ps_ = Ps::Text; }
            else if (csi_.size() < 64) csi_.push_back((char)b);
            else ps_ = Ps::Text;                  // runaway sequence
            break;
        case Ps::Osc:
            if (b == 0x07) ps_ = Ps::Text;        // BEL terminator
            else if (b == 0x1B) ps_ = Ps::OscEsc; // maybe ST (ESC \)
            break;
        case Ps::OscEsc:
            ps_ = (b == '\\') ? Ps::Text : Ps::Osc;
            break;
        case Ps::Charset:
            ps_ = Ps::Text;                       // consume designator byte
            break;
        }
    }
}

} // namespace term

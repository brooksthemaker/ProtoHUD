#include "crash_reporter.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ── Globals set by install() — read only inside signal handler ────────────────
static const AppState* g_state     = nullptr;
static char            g_crash_dir[256] = "/tmp";
static char            g_git_hash[64]   = "unknown";

static const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        default:      return "UNKNOWN";
    }
}

// Async-signal-safe integer-to-decimal writer into a buffer.
static int u64_to_str(char* buf, uint64_t v) {
    if (v == 0) { buf[0] = '0'; return 1; }
    char tmp[20]; int len = 0;
    while (v > 0) { tmp[len++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < len; ++i) buf[i] = tmp[len - 1 - i];
    return len;
}

// ── Async-signal-safe append helpers ──────────────────────────────────────────
// The child writes its report with these + write(2) only. The crashing process
// is multithreaded: another thread may have held the malloc lock at crash time,
// and fork() clones that locked state into the child — any fopen/fprintf/
// backtrace_symbols (all malloc) would deadlock the child forever.

static void append_str(char* dst, size_t cap, size_t& len, const char* s) {
    while (*s && len + 1 < cap) dst[len++] = *s++;
}

static void append_u64(char* dst, size_t cap, size_t& len, uint64_t v) {
    char tmp[20];
    int n = u64_to_str(tmp, v);
    for (int i = 0; i < n && len + 1 < cap; ++i) dst[len++] = tmp[i];
}

// Fixed-point decimal: decimals=0 → "%.0f", decimals=1 → "%.1f" (non-negative).
static void append_fixed(char* dst, size_t cap, size_t& len, double v, int decimals) {
    if (v < 0) v = 0;
    if (decimals <= 0) {
        append_u64(dst, cap, len, static_cast<uint64_t>(v + 0.5));
        return;
    }
    uint64_t tenths = static_cast<uint64_t>(v * 10.0 + 0.5);
    append_u64(dst, cap, len, tenths / 10);
    append_str(dst, cap, len, ".");
    append_u64(dst, cap, len, tenths % 10);
}

static void append_hex_ptr(char* dst, size_t cap, size_t& len, const void* p) {
    append_str(dst, cap, len, "0x");
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    char tmp[2 * sizeof(uintptr_t)]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        int d = static_cast<int>(v & 0xF);
        tmp[n++] = (d < 10) ? static_cast<char>('0' + d)
                            : static_cast<char>('a' + d - 10);
        v >>= 4;
    }
    for (int i = n - 1; i >= 0 && len + 1 < cap; --i) dst[len++] = tmp[i];
}

static void append_bool(char* dst, size_t cap, size_t& len, bool b) {
    append_str(dst, cap, len, b ? "true" : "false");
}

// Write crash report from the forked child. Async-signal-safe calls only —
// open/write/backtrace_symbols_fd; no stdio, no malloc (see helper comment).
static void write_report_child(int sig) {
    // Build filename: /tmp/protohud_crash_<epoch>.json
    time_t now = time(nullptr);
    char path[512]; size_t plen = 0;
    append_str(path, sizeof(path), plen, g_crash_dir);
    append_str(path, sizeof(path), plen, "/protohud_crash_");
    append_u64(path, sizeof(path), plen, static_cast<uint64_t>(now));
    append_str(path, sizeof(path), plen, ".json");
    path[plen] = '\0';

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    // Capture backtrace. install() primed backtrace() in the parent so the
    // lazy libgcc load (which mallocs) already happened before the crash.
    void* bt[64];
    int   bt_depth = backtrace(bt, 64);

    // Snapshot health — no mutex needed (parent is dead/suspended)
    bool cam_usb1 = false, cam_usb2 = false, cam_usb3 = false;
    bool teensy   = false, lora = false, audio = false, wifi = false;
    uint64_t uptime_s = 0;
    float    cpu_pct  = 0.f;
    float    ram_used = 0.f, ram_total = 0.f;
    if (g_state) {
        cam_usb1  = g_state->health.cam_usb1;
        cam_usb2  = g_state->health.cam_usb2;
        cam_usb3  = g_state->health.cam_usb3;
        teensy    = g_state->health.teensy_ok;
        lora      = g_state->health.lora_ok;
        audio     = g_state->health.audio_ok;
        wifi      = g_state->health.wifi_ok;
        uptime_s  = g_state->sys_metrics.uptime_s;
        cpu_pct   = g_state->sys_metrics.cpu_pct;
        ram_used  = g_state->sys_metrics.ram_used_mb;
        ram_total = g_state->sys_metrics.ram_total_mb;
    }

    // Build the structured JSON in a static buffer, then one write(2).
    // (Static: keep the signal-context stack small.)
    static char rep[8192];
    const size_t cap = sizeof(rep);
    size_t len = 0;
    append_str(rep, cap, len, "{\n  \"signal\": \"");
    append_str(rep, cap, len, sig_name(sig));
    append_str(rep, cap, len, "\",\n  \"timestamp\": ");
    append_u64(rep, cap, len, static_cast<uint64_t>(now));
    append_str(rep, cap, len, ",\n  \"git_hash\": \"");
    append_str(rep, cap, len, g_git_hash);
    append_str(rep, cap, len, "\",\n  \"uptime_s\": ");
    append_u64(rep, cap, len, uptime_s);
    append_str(rep, cap, len, ",\n  \"cpu_pct\": ");
    append_fixed(rep, cap, len, cpu_pct, 1);
    append_str(rep, cap, len, ",\n  \"ram_used_mb\": ");
    append_fixed(rep, cap, len, ram_used, 0);
    append_str(rep, cap, len, ",\n  \"ram_total_mb\": ");
    append_fixed(rep, cap, len, ram_total, 0);
    append_str(rep, cap, len, ",\n  \"health\": {\n    \"cam_usb1\": ");
    append_bool(rep, cap, len, cam_usb1);
    append_str(rep, cap, len, ",\n    \"cam_usb2\": ");
    append_bool(rep, cap, len, cam_usb2);
    append_str(rep, cap, len, ",\n    \"cam_usb3\": ");
    append_bool(rep, cap, len, cam_usb3);
    append_str(rep, cap, len, ",\n    \"teensy\": ");
    append_bool(rep, cap, len, teensy);
    append_str(rep, cap, len, ",\n    \"lora\": ");
    append_bool(rep, cap, len, lora);
    append_str(rep, cap, len, ",\n    \"audio\": ");
    append_bool(rep, cap, len, audio);
    append_str(rep, cap, len, ",\n    \"wifi\": ");
    append_bool(rep, cap, len, wifi);
    append_str(rep, cap, len, "\n  },\n  \"backtrace\": [\n");
    // Raw frame addresses (hex) — backtrace_symbols() mallocs, so symbol names
    // can't go in the JSON safely. Resolve offline with addr2line + git_hash;
    // the symbolised trace still goes to stderr below.
    for (int i = 0; i < bt_depth; ++i) {
        append_str(rep, cap, len, "    \"");
        append_hex_ptr(rep, cap, len, bt[i]);
        append_str(rep, cap, len, (i < bt_depth - 1) ? "\",\n" : "\"\n");
    }
    append_str(rep, cap, len, "  ]\n}\n");

    size_t off = 0;
    while (off < len) {
        ssize_t w = ::write(fd, rep + off, len - off);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        off += static_cast<size_t>(w);
    }
    ::close(fd);

    // Also dump the symbolised backtrace to stderr (backtrace_symbols_fd is
    // async-signal-safe — it formats straight to the fd without malloc).
    if (bt_depth > 0) {
        const char hdr[] = "\n[CrashReporter] raw backtrace:\n";
        ::write(STDERR_FILENO, hdr, sizeof(hdr) - 1);
        backtrace_symbols_fd(bt, bt_depth, STDERR_FILENO);
    }
}

static void crash_handler(int sig) {
    // Restore default so re-raise terminates normally.
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: write report then exit.
        write_report_child(sig);
        _exit(0);
    } else if (pid > 0) {
        // Parent: wait for the child, but bounded (~5 s). A blocking
        // waitpid(...,0) hangs the handler forever if the child deadlocks.
        for (int i = 0; i < 500; ++i) {
            pid_t r = waitpid(pid, nullptr, WNOHANG);
            if (r == pid) break;
            if (r < 0 && errno != EINTR) break;
            struct timespec ts { 0, 10 * 1000 * 1000 };  // 10 ms
            nanosleep(&ts, nullptr);
        }
    }
    raise(sig); // re-raise to get proper exit status / core dump
}

namespace CrashReporter {

void install(const AppState* state,
             const std::string& crash_dir,
             const std::string& git_hash) {
    g_state = state;
    strncpy(g_crash_dir, crash_dir.c_str(), sizeof(g_crash_dir) - 1);
    g_crash_dir[sizeof(g_crash_dir) - 1] = '\0';
    strncpy(g_git_hash,  git_hash.c_str(),  sizeof(g_git_hash)  - 1);
    g_git_hash[sizeof(g_git_hash) - 1] = '\0';

    // Prime backtrace() now: glibc lazily dlopens libgcc_s on first call,
    // which allocates — unsafe in the forked crash child.
    void* warm[2];
    backtrace(warm, 2);

    struct sigaction sa {};
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND; // restore default after first signal

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
}

} // namespace CrashReporter

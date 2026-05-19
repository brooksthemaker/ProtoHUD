#include "crash_reporter.h"

#include <csignal>
#include <cstdio>
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

// Write crash report from child process (no malloc restrictions here after fork).
static void write_report_child(int sig) {
    // Build filename: /tmp/protohud_crash_<epoch>.json
    time_t now = time(nullptr);
    char path[512];
    snprintf(path, sizeof(path), "%s/protohud_crash_%lld.json",
             g_crash_dir, static_cast<long long>(now));

    FILE* f = fopen(path, "w");
    if (!f) return;

    // Capture backtrace (safe in child after fork)
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

    // Write structured JSON
    fprintf(f, "{\n");
    fprintf(f, "  \"signal\": \"%s\",\n", sig_name(sig));
    fprintf(f, "  \"timestamp\": %lld,\n", static_cast<long long>(now));
    fprintf(f, "  \"git_hash\": \"%s\",\n", g_git_hash);
    fprintf(f, "  \"uptime_s\": %llu,\n", static_cast<unsigned long long>(uptime_s));
    fprintf(f, "  \"cpu_pct\": %.1f,\n", static_cast<double>(cpu_pct));
    fprintf(f, "  \"ram_used_mb\": %.0f,\n", static_cast<double>(ram_used));
    fprintf(f, "  \"ram_total_mb\": %.0f,\n", static_cast<double>(ram_total));
    fprintf(f, "  \"health\": {\n");
    fprintf(f, "    \"cam_usb1\": %s,\n", cam_usb1 ? "true" : "false");
    fprintf(f, "    \"cam_usb2\": %s,\n", cam_usb2 ? "true" : "false");
    fprintf(f, "    \"cam_usb3\": %s,\n", cam_usb3 ? "true" : "false");
    fprintf(f, "    \"teensy\": %s,\n",   teensy   ? "true" : "false");
    fprintf(f, "    \"lora\": %s,\n",     lora     ? "true" : "false");
    fprintf(f, "    \"audio\": %s,\n",    audio    ? "true" : "false");
    fprintf(f, "    \"wifi\": %s\n",      wifi     ? "true" : "false");
    fprintf(f, "  },\n");
    fprintf(f, "  \"backtrace\": [\n");
    // Write symbol strings using backtrace_symbols_fd would go to a separate fd;
    // here we use backtrace_symbols for the JSON (malloc is safe in child).
    char** syms = backtrace_symbols(bt, bt_depth);
    for (int i = 0; i < bt_depth; ++i) {
        const char* s = (syms && syms[i]) ? syms[i] : "??";
        fprintf(f, "    \"%s\"%s\n", s, (i < bt_depth - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    // Also dump raw addresses to stderr for addr2line
    if (bt_depth > 0) {
        int err_fd = open("/dev/stderr", O_WRONLY);
        if (err_fd >= 0) {
            const char hdr[] = "\n[CrashReporter] raw backtrace:\n";
            write(err_fd, hdr, sizeof(hdr) - 1);
            backtrace_symbols_fd(bt, bt_depth, err_fd);
            close(err_fd);
        }
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
        // Parent: wait for child to finish writing, then re-raise.
        waitpid(pid, nullptr, 0);
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

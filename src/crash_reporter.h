#pragma once
#include "app_state.h"
#include <string>

// Installs SIGSEGV / SIGABRT / SIGFPE / SIGILL / SIGBUS handlers.
// On crash: forks a child that writes a structured crash report to crash_dir,
// then the parent calls _exit(1).
//
// Call install() once from main() before any other threads are started.
// The state pointer is stored in a global so the handler can snapshot health flags.

namespace CrashReporter {
    void install(const AppState* state,
                 const std::string& crash_dir = "/tmp",
                 const std::string& git_hash  = "unknown");
}

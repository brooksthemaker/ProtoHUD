#include "scheduler_monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Staleness window: if events.json hasn't been rewritten in this long, treat the
// daemon as down (events still render from the last good read).
static constexpr time_t kStaleAfterS = 300;

static bool file_mtime(const std::string& path, time_t& out) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    out = st.st_mtime;
    return true;
}

// Parse a JSON file; returns discarded() json on missing/partial/invalid file so a
// torn read mid-rename is handled gracefully (caller keeps last good state).
static json parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::value_t::discarded;
    return json::parse(f, nullptr, /*allow_exceptions=*/false);
}

void SchedulerMonitor::start(AppState* state, std::string events_path,
                             std::string status_path, int poll_interval_s) {
    state_           = state;
    events_path_     = std::move(events_path);
    status_path_     = std::move(status_path);
    poll_interval_s_ = poll_interval_s > 0 ? poll_interval_s : 20;
    first_load_      = true;
    running_         = true;
    thread_          = std::thread(&SchedulerMonitor::thread_fn, this);
}

void SchedulerMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void SchedulerMonitor::thread_fn() {
    while (running_) {
        const time_t now = time(nullptr);

        // ── events.json ────────────────────────────────────────────────────────
        std::vector<ScheduledEvent> fresh;
        bool events_ok = false;
        json je = parse_file(events_path_);
        if (!je.is_discarded()) {
            const json* arr = je.is_array() ? &je
                            : (je.contains("events") && je["events"].is_array()
                                   ? &je["events"] : nullptr);
            if (arr) {
                events_ok = true;
                for (const auto& e : *arr) {
                    ScheduledEvent ev;
                    ev.uid       = e.value("uid", std::string());
                    ev.title     = e.value("title", std::string("(untitled)"));
                    ev.location  = e.value("location", std::string());
                    ev.start_utc = static_cast<time_t>(e.value("start_utc", (int64_t)0));
                    ev.end_utc   = static_cast<time_t>(e.value("end_utc", (int64_t)0));
                    ev.all_day   = e.value("all_day", false);
                    ev.source    = (e.value("source", std::string("manual")) == "google")
                                       ? EventSource::Google : EventSource::Manual;
                    if (ev.uid.empty()) continue;  // uid is required for dedupe
                    // Suppress reminders for events already past on first load.
                    if (first_load_ && ev.start_utc != 0 && ev.start_utc < now) {
                        ev.fired_lead = ev.fired_start = true;
                    }
                    fresh.push_back(std::move(ev));
                }
                std::sort(fresh.begin(), fresh.end(),
                          [](const ScheduledEvent& a, const ScheduledEvent& b) {
                              return a.start_utc < b.start_utc;
                          });
            }
        }

        // ── scheduler_status.json ────────────────────────────────────────────────
        SchedulerStatus status;
        json js = parse_file(status_path_);
        if (!js.is_discarded() && js.is_object()) {
            status.web_url         = js.value("web_url", std::string());
            status.gcal_state      = js.value("gcal_state", std::string("disconnected"));
            status.gcal_user_code  = js.value("gcal_user_code", std::string());
            status.gcal_verify_url = js.value("gcal_verify_url", std::string());
            status.last_sync_utc   = static_cast<time_t>(js.value("last_sync_utc", (int64_t)0));
        }

        // Daemon considered healthy only if events.json parsed and is fresh.
        time_t mt = 0;
        const bool fresh_file = file_mtime(events_path_, mt) && (now - mt) < kStaleAfterS;
        status.daemon_ok   = events_ok && fresh_file;
        status.event_count = static_cast<int>(fresh.size());

        // ── Publish under the lock, carrying fire-state forward by uid ───────────
        if (events_ok) {
            std::lock_guard<std::mutex> lk(state_->mtx);
            for (auto& nv : fresh) {
                for (const auto& old : state_->scheduler_events) {
                    if (old.uid != nv.uid) continue;
                    // Re-arm reminders only when the event was rescheduled.
                    if (old.start_utc == nv.start_utc) {
                        nv.fired_lead   = old.fired_lead;
                        nv.fired_start  = old.fired_start;
                        nv.snooze_until = old.snooze_until;
                    }
                    break;
                }
            }
            state_->scheduler_events = std::move(fresh);
            state_->scheduler_status = std::move(status);
            first_load_ = false;
        } else {
            // Keep last good event list; just refresh daemon health/status.
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->scheduler_status = std::move(status);
        }

        // Sleep in short slices so stop() is responsive.
        for (int i = 0; i < poll_interval_s_ * 5 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

#pragma once
#include "../app_state.h"
#include <atomic>
#include <string>
#include <thread>

// Polls the scheduler daemon's atomically-written JSON files every poll_interval_s:
//   events.json          — the merged calendar event list (manual + Google)
//   scheduler_status.json — daemon web URL, Google auth state, last sync
// Results are written to AppState::scheduler_events / scheduler_status under the
// state mutex. The daemon owns all networking; this side only reads files.
//
// fired_lead / fired_start / snooze_until flags are preserved across reloads by
// merging on event uid, so a poll never re-fires reminders already shown.

class SchedulerMonitor {
public:
    SchedulerMonitor()  = default;
    ~SchedulerMonitor() { stop(); }

    void start(AppState* state, std::string events_path, std::string status_path,
               int poll_interval_s);
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();

    AppState*         state_ = nullptr;
    std::string       events_path_;
    std::string       status_path_;
    int               poll_interval_s_ = 20;
    bool              first_load_ = true;   // suppress reminder backfill for past events
    std::thread       thread_;
    std::atomic<bool> running_ { false };
};

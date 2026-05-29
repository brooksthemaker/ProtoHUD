#pragma once
// ── phone_inbox.h ────────────────────────────────────────────────────────────
// Watches a directory for files dropped in by the phone (typically via KDE
// Connect's Share & Receive → ~/Downloads, but any path works — sftp, scp,
// manual move) and surfaces interactive toasts: "Phone shared happy.png —
// Import as 'happy'?" → [Import] [Dismiss]. Import calls a host-supplied
// handler so the watcher stays decoupled from NativeFaceController; the
// host wires the handler with whatever face/gif controller is live.
//
// Worker polls at 2 Hz (cheap; the directory rarely has more than a handful
// of files). On import, the file is moved to <inbox>/.processed/; on
// dismiss, to <inbox>/.dismissed/. That preserves the user's data and
// keeps the inbox itself tidy so the next poll doesn't re-notify.

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

struct AppState;

namespace integrations {

struct PhoneInboxConfig {
    bool        enabled        = true;
    // Directory the watcher scans. Defaults at construction time to
    // $XDG_DOWNLOAD_DIR / ~/Downloads (KDE Connect's default share target).
    std::string watch_dir;
    // Seconds before an unmodified file is considered "settled" and worth
    // notifying about — guards against picking up a half-transferred file.
    int         settle_seconds = 2;
    // Auto-dismiss applied to the import-prompt toast. 0 keeps it sticky
    // until the user acts on it (recommended — dropping in a face you
    // wanted to import would be annoying).
    float       auto_dismiss_s = 0.0f;
};

class PhoneInbox {
public:
    // Returns true on success. The host populates these from main's scope so
    // the inbox can stay free of face/controller dependencies. src_path is
    // an absolute path inside the watch dir; the watcher takes care of
    // moving it into .processed/ on success.
    using ImportFaceFn = std::function<bool(const std::string& src_path,
                                            const std::string& expression)>;
    using ImportGifFn  = std::function<bool(const std::string& src_path,
                                            const std::string& filename)>;

    PhoneInbox(AppState& state, PhoneInboxConfig cfg,
               ImportFaceFn import_face, ImportGifFn import_gif);
    ~PhoneInbox();

    PhoneInbox(const PhoneInbox&)            = delete;
    PhoneInbox& operator=(const PhoneInbox&) = delete;

    bool start();
    void stop();

    const std::string& watch_dir() const { return cfg_.watch_dir; }

private:
    void worker();

    AppState&            state_;
    PhoneInboxConfig     cfg_;
    ImportFaceFn         import_face_;
    ImportGifFn          import_gif_;

    std::atomic<bool>    running_{false};
    std::thread          thread_;

    // Per-file tracker: notif id we pushed (so we can dismiss the toast
    // after the user acts), and the last-seen mtime so we re-notify when
    // the user re-shares the same name with new content.
    struct Seen {
        uint32_t notif_id  = 0;
        int64_t  mtime_ns  = 0;
    };
    mutable std::mutex   seen_mtx_;
    std::unordered_map<std::string, Seen> seen_;
};

} // namespace integrations

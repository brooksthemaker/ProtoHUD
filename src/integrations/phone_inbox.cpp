#include "phone_inbox.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "../app_state.h"

namespace fs = std::filesystem;

namespace integrations {

namespace {

std::string xdg_download_dir() {
    // KDE Connect uses xdg-user-dirs; we don't link xdg, so resolve the
    // same way the spec describes: $XDG_DOWNLOAD_DIR if set, else
    // $HOME/Downloads.
    const char* x = std::getenv("XDG_DOWNLOAD_DIR");
    if (x && *x) return std::string(x);
    const char* h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp") + "/Downloads";
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

bool is_ext(const fs::path& p, const std::string& want_lower) {
    return to_lower(p.extension().string()) == want_lower;
}

int64_t mtime_ns(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        ft.time_since_epoch()).count();
}

void move_to_subdir(const fs::path& src, const fs::path& subdir) {
    std::error_code ec;
    fs::create_directories(subdir, ec);
    fs::path dst = subdir / src.filename();
    // Suffix with a counter if a same-named file is already there so we
    // don't clobber prior history.
    if (fs::exists(dst, ec)) {
        for (int i = 1; i < 1000; ++i) {
            char buf[32]; std::snprintf(buf, sizeof(buf), ".%d", i);
            fs::path candidate = subdir /
                (src.stem().string() + buf + src.extension().string());
            if (!fs::exists(candidate, ec)) { dst = candidate; break; }
        }
    }
    fs::rename(src, dst, ec);
    // rename across devices fails with EXDEV — fall back to copy+remove.
    if (ec) {
        ec.clear();
        fs::copy_file(src, dst,
                      fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(src, ec);
    }
}

} // namespace

PhoneInbox::PhoneInbox(AppState& state, PhoneInboxConfig cfg,
                       ImportFaceFn import_face, ImportGifFn import_gif)
    : state_(state), cfg_(std::move(cfg)),
      import_face_(std::move(import_face)),
      import_gif_(std::move(import_gif)) {
    if (cfg_.watch_dir.empty()) cfg_.watch_dir = xdg_download_dir();
}

PhoneInbox::~PhoneInbox() { stop(); }

bool PhoneInbox::start() {
    if (running_.load() || !cfg_.enabled) return false;
    std::error_code ec;
    fs::create_directories(cfg_.watch_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[phone-inbox] cannot create %s: %s\n",
                     cfg_.watch_dir.c_str(), ec.message().c_str());
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&PhoneInbox::worker, this);
    return true;
}

void PhoneInbox::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(seen_mtx_);
    seen_.clear();
}

void PhoneInbox::worker() {
    const fs::path root   = cfg_.watch_dir;
    const fs::path proc   = root / ".processed";
    const fs::path dismd  = root / ".dismissed";

    auto push_face_toast = [this, proc, dismd](const fs::path& src) {
        const std::string expression =
            to_lower(src.stem().string());
        const std::string path_str   = src.string();

        Notification n;
        n.type           = NotifType::App;
        n.title          = "Phone Inbox";
        n.body           = std::string("Got ") + src.filename().string() +
                           "  —  import as '" + expression + "'?";
        n.auto_dismiss_s = cfg_.auto_dismiss_s;

        // Action lambdas just touch the filesystem — pushing into
        // state.notifs here would invalidate the toast-renderer's for-loop
        // iterators. The user gets feedback when the import-prompt toast
        // disappears (and, for face PNGs, when the new face shows on the
        // panels on the next render tick).
        ImportFaceFn handler = import_face_;
        n.actions.push_back({"IMPORT",
            [handler, path_str, expression, proc](AppState&) {
                if (handler) handler(path_str, expression);
                move_to_subdir(path_str, proc);
            }});
        n.actions.push_back({"DISMISS",
            [path_str, dismd](AppState&) {
                move_to_subdir(path_str, dismd);
            }});

        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.notifs.push(std::move(n));
            id = state_.notifs.items.front().id;
        }
        std::lock_guard<std::mutex> lk(seen_mtx_);
        seen_[path_str] = {id, mtime_ns(src)};
    };

    auto push_gif_toast = [this, proc, dismd](const fs::path& src) {
        const std::string filename = src.filename().string();
        const std::string path_str = src.string();

        Notification n;
        n.type           = NotifType::App;
        n.title          = "Phone Inbox";
        n.body           = std::string("Got ") + filename +
                           "  —  add to GIF library?";
        n.auto_dismiss_s = cfg_.auto_dismiss_s;

        ImportGifFn handler = import_gif_;
        n.actions.push_back({"IMPORT",
            [handler, path_str, filename, proc](AppState&) {
                if (handler) handler(path_str, filename);
                move_to_subdir(path_str, proc);
            }});
        n.actions.push_back({"DISMISS",
            [path_str, dismd](AppState&) {
                move_to_subdir(path_str, dismd);
            }});

        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.notifs.push(std::move(n));
            id = state_.notifs.items.front().id;
        }
        std::lock_guard<std::mutex> lk(seen_mtx_);
        seen_[path_str] = {id, mtime_ns(src)};
    };

    while (running_.load()) {
        const auto loop_start = std::chrono::steady_clock::now();

        std::error_code ec;
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t settle_ns =
                static_cast<int64_t>(cfg_.settle_seconds) * 1'000'000'000LL;

            for (auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                fs::path p = entry.path();
                // Skip dotfiles + our own subdirs.
                if (!p.filename().empty() && p.filename().string()[0] == '.') continue;

                const int64_t mt = mtime_ns(p);
                if (mt == 0) continue;
                if ((now_ns - mt) < settle_ns) continue;   // still transferring

                const std::string key = p.string();
                {
                    std::lock_guard<std::mutex> lk(seen_mtx_);
                    auto it = seen_.find(key);
                    if (it != seen_.end() && it->second.mtime_ns == mt) continue;
                }

                if (is_ext(p, ".png"))      push_face_toast(p);
                else if (is_ext(p, ".gif")) push_gif_toast(p);
                // Other extensions are left in the inbox unannounced —
                // they're presumably general downloads the user is
                // handling outside ProtoHUD.
            }
        }

        // Garbage-collect seen entries whose files are gone (imported /
        // dismissed) so the map doesn't grow without bound.
        {
            std::lock_guard<std::mutex> lk(seen_mtx_);
            for (auto it = seen_.begin(); it != seen_.end(); ) {
                std::error_code e;
                if (!fs::exists(it->first, e)) it = seen_.erase(it);
                else ++it;
            }
        }

        std::this_thread::sleep_until(loop_start + std::chrono::milliseconds(500));
    }
}

} // namespace integrations

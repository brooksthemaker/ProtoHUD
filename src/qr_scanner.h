#pragma once
#include <functional>
#include <string>
#include <vector>

#ifdef HAVE_ZBAR
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// Async QR/barcode scanner backed by a single ZBar worker thread.
//
// submit_gray() and submit_rgba() are thread-safe and return immediately.
// Internally rate-limited: frames submitted faster than scan_interval_ms
// are silently dropped, so callers can submit every frame without overhead.
// Duplicate results are suppressed for cooldown_ms after the last decode.
//
// Call set_callback() before submit_*() — callback fires on the worker thread.
class QrScanner {
public:
    // (text, symbol_type_name) e.g. ("https://...", "QR-Code")
    using Callback = std::function<void(const std::string& text,
                                        const std::string& type)>;

    QrScanner();
    ~QrScanner();

    void set_callback(Callback cb);
    void set_scan_interval_ms(int ms);   // min ms between scans, default 500
    void set_cooldown_ms(int ms);        // suppress same result, default 5000

    // Submit a single-channel grayscale frame. Rate-limited; excess ignored.
    void submit_gray(std::vector<uint8_t> gray, int w, int h);

    // Convenience: converts RGBA (4 ch) to gray then calls submit_gray.
    void submit_rgba(const uint8_t* rgba, int w, int h);

private:
    void worker();

    struct Frame { std::vector<uint8_t> gray; int w = 0, h = 0; };

    Callback   callback_;
    int        scan_interval_ms_ = 500;
    int        cooldown_ms_      = 5000;

    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    has_pending_ = false;
    Frame                   pending_;
    bool                    running_     = false;
    std::thread             thread_;

    std::chrono::steady_clock::time_point last_submit_{};
    std::chrono::steady_clock::time_point last_found_{};
    std::string                            last_result_;
};

#else // !HAVE_ZBAR

// No-op stub when ZBar is not installed.
class QrScanner {
public:
    using Callback = std::function<void(const std::string&, const std::string&)>;
    void set_callback(Callback)         {}
    void set_scan_interval_ms(int)      {}
    void set_cooldown_ms(int)           {}
    void submit_gray(std::vector<uint8_t>, int, int) {}
    void submit_rgba(const uint8_t*, int, int)       {}
};

#endif // HAVE_ZBAR

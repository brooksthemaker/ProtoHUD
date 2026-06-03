#ifdef HAVE_ZBAR

#include "qr_scanner.h"
#include <zbar.h>
#include <algorithm>
#include <chrono>

// Some distros ship ZBar as a C++ library with all symbols in namespace zbar.
using namespace zbar;

using clock_t2 = std::chrono::steady_clock;

QrScanner::QrScanner() {
    running_ = true;
    thread_  = std::thread(&QrScanner::worker, this);
}

QrScanner::~QrScanner() {
    {
        std::lock_guard lk(mtx_);
        running_ = false;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void QrScanner::set_callback(Callback cb)       { callback_         = std::move(cb); }
void QrScanner::set_scan_interval_ms(int ms)    { scan_interval_ms_ = ms; }
void QrScanner::set_cooldown_ms(int ms)         { cooldown_ms_      = ms; }

void QrScanner::submit_rgba(const uint8_t* rgba, int w, int h) {
    // Luma (BT.601): Y = 0.299R + 0.587G + 0.114B  (integer: *77 *150 *29 >> 8)
    std::vector<uint8_t> gray(static_cast<size_t>(w * h));
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = rgba + i * 4;
        gray[i] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }
    submit_gray(std::move(gray), w, h);
}

void QrScanner::submit_gray(std::vector<uint8_t> gray, int w, int h) {
    // Rate limit on the calling thread — cheap check before acquiring any lock.
    auto now     = clock_t2::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_submit_).count();
    if (elapsed < scan_interval_ms_) return;
    last_submit_ = now;

    {
        std::lock_guard lk(mtx_);
        pending_     = { std::move(gray), w, h };
        has_pending_ = true;
    }
    cv_.notify_one();
}

void QrScanner::worker() {
    zbar_image_scanner_t* scanner = zbar_image_scanner_create();
    // Enable all symbologies; restrict to QR + Code128 for a common use-case.
    zbar_image_scanner_set_config(scanner, ZBAR_NONE,    ZBAR_CFG_ENABLE, 0);
    zbar_image_scanner_set_config(scanner, ZBAR_QRCODE,  ZBAR_CFG_ENABLE, 1);
    zbar_image_scanner_set_config(scanner, ZBAR_CODE128, ZBAR_CFG_ENABLE, 1);
    zbar_image_scanner_set_config(scanner, ZBAR_EAN13,   ZBAR_CFG_ENABLE, 1);

    while (true) {
        Frame frame;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [this] { return has_pending_ || !running_; });
            if (!running_) break;
            frame       = std::move(pending_);
            has_pending_ = false;
        }

        zbar_image_t* img = zbar_image_create();
        // GREY fourcc: little-endian 'G','R','E','Y'
        zbar_image_set_format(img, 0x59455247UL);
        zbar_image_set_size(img, static_cast<unsigned>(frame.w),
                                 static_cast<unsigned>(frame.h));
        // ZBar does not own the data; we keep frame.gray alive until destroy.
        zbar_image_set_data(img, frame.gray.data(), frame.gray.size(), nullptr);

        if (zbar_scan_image(scanner, img) > 0) {
            const zbar_symbol_t* sym = zbar_image_first_symbol(img);
            while (sym) {
                std::string text = zbar_symbol_get_data(sym);
                std::string type = zbar_get_symbol_name(zbar_symbol_get_type(sym));

                auto now2      = clock_t2::now();
                auto since     = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now2 - last_found_).count();
                bool duplicate = (text == last_result_) && (since < cooldown_ms_);

                if (!duplicate) {
                    last_result_ = text;
                    last_found_  = now2;
                    if (callback_) callback_(text, type, frame.gray, frame.w, frame.h);
                }
                sym = zbar_symbol_next(sym);
            }
        }

        zbar_image_destroy(img);
    }

    zbar_image_scanner_destroy(scanner);
}

#endif // HAVE_ZBAR

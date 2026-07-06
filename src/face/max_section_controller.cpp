#include "max_section_controller.h"

#include <algorithm>
#include <chrono>

#include "max_section_content.h"

namespace face {

MaxSectionController::MaxSectionController(std::unique_ptr<PanelOutput> out,
                                          int canvas_w, int canvas_h)
    : out_(std::move(out)) {
    canvas_ = cv::Mat(std::max(1, canvas_h), std::max(1, canvas_w), CV_8UC3,
                      cv::Scalar(0, 0, 0));
}

MaxSectionController::~MaxSectionController() { stop(); }

bool MaxSectionController::start() {
    if (running_.load()) return true;
    if (!out_ || !out_->open()) return false;
    running_.store(true);
    thread_ = std::thread(&MaxSectionController::loop, this);
    return true;
}

void MaxSectionController::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (out_) out_->close();
}

void MaxSectionController::render_locked() {
    canvas_.setTo(cv::Scalar(0, 0, 0));
    if (kind_ == "symbol") {
        max_content::draw_symbol(canvas_, value_);
    } else if (kind_ == "pattern") {
        max_content::draw_pattern(canvas_, value_, phase_.load());
    } else if (kind_ == "text") {
        // Left-aligned, 1px top margin so a 7px glyph fits an 8px-tall panel.
        max_content::draw_text(canvas_, value_, 0, 1);
    }
    // "blank" → all-off canvas.
}

void MaxSectionController::loop() {
    using namespace std::chrono;
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Re-render each tick so animated patterns advance; static content
            // just repaints the same frame (also re-pushes after a dropped SPI
            // line, since the MAX chips hold their last frame otherwise).
            render_locked();
            if (out_) out_->show(canvas_);
        }
        phase_.fetch_add(1);
        std::this_thread::sleep_for(milliseconds(120));   // ~8 fps
    }
}

void MaxSectionController::set_symbol(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    kind_ = "symbol"; value_ = name;
    const auto& names = max_content::symbol_names();
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) { sym_idx_ = static_cast<int>(i); break; }
}
void MaxSectionController::set_text(const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);
    kind_ = "text"; value_ = text;
}
void MaxSectionController::set_pattern(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    kind_ = "pattern"; value_ = name;
}
void MaxSectionController::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    kind_ = "blank"; value_.clear();
}
void MaxSectionController::cycle(int dir) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto& names = max_content::symbol_names();
    if (names.empty()) return;
    const int n = static_cast<int>(names.size());
    if (kind_ != "symbol") { kind_ = "symbol"; }
    else                   { sym_idx_ = (sym_idx_ + dir % n + n) % n; }
    value_ = names[(sym_idx_ % n + n) % n];
}

std::string MaxSectionController::current() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return value_.empty() ? kind_ : (kind_ + ":" + value_);
}

}  // namespace face

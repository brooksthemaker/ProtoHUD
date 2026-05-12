#include "track_queue.h"
#include <algorithm>
#include <random>
#include <ctime>

static std::mt19937 rng(static_cast<unsigned>(time(nullptr)));

void TrackQueue::load(std::vector<std::string> paths, bool shuffle) {
    original_ = paths;
    queue_    = std::move(paths);
    idx_      = 0;
    if (shuffle) shuffle_from(0);
}

const std::string& TrackQueue::current() const {
    static const std::string empty;
    return queue_.empty() ? empty : queue_[idx_];
}

bool TrackQueue::next(RepeatMode repeat) {
    if (queue_.empty()) return false;
    if (repeat == RepeatMode::ONE) return true;  // stay on same track
    if (idx_ + 1 < queue_.size()) { ++idx_; return true; }
    if (repeat == RepeatMode::ALL) { idx_ = 0; return true; }
    return false;  // end of queue, no repeat
}

void TrackQueue::prev() {
    if (queue_.empty()) return;
    idx_ = (idx_ > 0) ? idx_ - 1 : queue_.size() - 1;
}

bool TrackQueue::jump(size_t target) {
    if (target >= queue_.size()) return false;
    idx_ = target;
    return true;
}

void TrackQueue::reshuffle() {
    if (queue_.empty()) return;
    const std::string cur = queue_[idx_];
    shuffle_from(0);
    // Keep current track at the front.
    auto it = std::find(queue_.begin(), queue_.end(), cur);
    if (it != queue_.end()) std::iter_swap(queue_.begin(), it);
    idx_ = 0;
}

void TrackQueue::shuffle_from(size_t start) {
    if (start >= queue_.size()) return;
    std::shuffle(queue_.begin() + static_cast<ptrdiff_t>(start), queue_.end(), rng);
}

#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "../app_state.h"

// Ordered play queue with shuffle and repeat support.
// Not thread-safe — call only from Core 1 (audio decode context).

class TrackQueue {
public:
    void load(std::vector<std::string> paths, bool shuffle = false);

    const std::string& current() const;
    bool  empty()         const { return queue_.empty(); }
    size_t size()         const { return queue_.size(); }
    size_t current_index()const { return idx_; }

    // Advance to next track. Returns false if at end (and repeat == OFF).
    bool next(RepeatMode repeat);

    // Step back to previous track (wraps to end if at start).
    void prev();

    bool jump(size_t idx);

    void reshuffle();   // re-randomise, keeping current track first

private:
    void shuffle_from(size_t start);

    std::vector<std::string> queue_;
    std::vector<std::string> original_;  // unshuffle reference
    size_t idx_ = 0;
};

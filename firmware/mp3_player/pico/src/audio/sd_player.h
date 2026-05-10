#pragma once
#include <SD.h>
#include <string>
#include <functional>
#include <atomic>
#include "track_queue.h"
#include "../app_state.h"

// MP3/FLAC decoder running on Core 1.
// Reads compressed audio from SD, decodes frame-by-frame, and calls
// on_pcm_frame() with raw s16le stereo PCM for the bridge to forward to ESP32.
//
// Uses minimp3 (header-only) for MP3 and dr_flac (header-only) for FLAC.
// Both are vendored in src/audio/vendor/.

class SdPlayer {
public:
    explicit SdPlayer(AppState& state);

    // Called once from setup1() on Core 1 — does not return.
    void run();

    // Thread-safe controls (called from Core 0 tasks).
    void play();
    void pause();
    void toggle();
    void stop();
    void skip_next();
    void skip_prev();
    void set_volume(uint8_t vol_0_100);   // applied as integer gain shift
    void load_queue(TrackQueue q, bool play_immediately = true);
    void jump_to(size_t queue_index);

    bool        is_playing()   const { return playing_.load(); }
    uint8_t     volume()       const { return volume_.load(); }

    // Called with decoded PCM frames. Set before run().
    std::function<void(const int16_t* pcm, size_t sample_pairs)> on_pcm_frame;

    // Called when track changes — provides updated TrackInfo.
    std::function<void(const TrackInfo&)> on_track_changed;

private:
    void decode_mp3(File& f);
    void decode_flac(File& f);
    bool open_track(const std::string& path);
    void apply_volume(int16_t* pcm, size_t samples);
    void read_tags(const std::string& path, TrackInfo& out);
    // Scans for cover.jpg / folder.jpg in the track directory and fills
    // state_.cover_art. Increments generation (release) so Core 0 sees the data.
    void extract_cover_art(const std::string& track_path);

    AppState& state_;

    TrackQueue queue_;
    TrackInfo  current_info_;

    std::atomic<bool>    playing_      { false };
    std::atomic<bool>    skip_next_    { false };
    std::atomic<bool>    skip_prev_    { false };
    std::atomic<bool>    stop_req_     { false };
    std::atomic<bool>    queue_dirty_  { false };
    std::atomic<uint8_t> volume_       { 80 };

    mutex_t queue_mtx_;

    static constexpr size_t DECODE_BUF_SAMPLES = 2304;  // max mp3 frame × 2 ch
};

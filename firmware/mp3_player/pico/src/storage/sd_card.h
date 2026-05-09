#pragma once
#include <SD.h>
#include <string>
#include <vector>
#include "../audio/track_queue.h"

struct DirEntry {
    std::string name;
    bool        is_dir;
    uint32_t    size_bytes;
};

// FAT32 SD card access via Arduino SPI SD library.
// All methods must be called from Core 0 (SPI is not multicore-safe).

class SdCard {
public:
    bool begin(int cs_pin);
    bool is_mounted() const { return mounted_; }

    // List a directory (sorted: dirs first, then supported audio files).
    std::vector<DirEntry> list(const std::string& path);

    // Recursively collect all .mp3 and .flac files under path.
    std::vector<std::string> collect_tracks(const std::string& path);

    // Build a TrackQueue from all tracks under path, optionally shuffled.
    TrackQueue build_queue(const std::string& path, bool shuffle = false);

    uint64_t total_bytes() const;
    uint64_t used_bytes()  const;

private:
    bool mounted_ = false;

    bool is_audio(const char* name);
    void collect_recursive(const std::string& path, std::vector<std::string>& out);
};

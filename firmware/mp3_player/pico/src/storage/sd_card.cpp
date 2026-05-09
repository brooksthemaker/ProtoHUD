#include "sd_card.h"
#include <algorithm>
#include <cctype>
#include <cstring>

bool SdCard::begin(int cs_pin) {
    mounted_ = SD.begin(cs_pin);
    return mounted_;
}

bool SdCard::is_audio(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i + 1]; ++i)
        ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(dot[i + 1])));
    return strcmp(ext, "mp3") == 0 || strcmp(ext, "flac") == 0;
}

std::vector<DirEntry> SdCard::list(const std::string& path) {
    std::vector<DirEntry> dirs, files;

    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) return {};

    File entry = dir.openNextFile();
    while (entry) {
        const char* name = entry.name();
        if (name[0] != '.') {
            if (entry.isDirectory()) {
                dirs.push_back({ name, true, 0 });
            } else if (is_audio(name)) {
                files.push_back({ name, false,
                                  static_cast<uint32_t>(entry.size()) });
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    auto cmp = [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; };
    std::sort(dirs.begin(),  dirs.end(),  cmp);
    std::sort(files.begin(), files.end(), cmp);
    dirs.insert(dirs.end(), files.begin(), files.end());
    return dirs;
}

void SdCard::collect_recursive(const std::string& path, std::vector<std::string>& out) {
    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) return;

    File entry = dir.openNextFile();
    while (entry) {
        const char* name = entry.name();
        if (name[0] != '.') {
            const std::string full = path + "/" + name;
            if (entry.isDirectory()) {
                collect_recursive(full, out);
            } else if (is_audio(name)) {
                out.push_back(full);
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    std::sort(out.begin(), out.end());
}

std::vector<std::string> SdCard::collect_tracks(const std::string& path) {
    std::vector<std::string> tracks;
    collect_recursive(path, tracks);
    return tracks;
}

TrackQueue SdCard::build_queue(const std::string& path, bool shuffle) {
    TrackQueue q;
    q.load(collect_tracks(path), shuffle);
    return q;
}

uint64_t SdCard::total_bytes() const { return SD.totalBytes(); }
uint64_t SdCard::used_bytes()  const { return SD.usedBytes(); }

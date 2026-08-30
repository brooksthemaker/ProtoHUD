#pragma once
// Atomic JSON file writes.
//
// ⚠⚠ THIS EXISTS BECAUSE OF A REAL BUG, and the shape of that bug is worth
// keeping in mind whenever a file is written and then read back.
//
// The blink setters used to do:
//     std::ofstream out(path);
//     out << j.dump(2);
//     reload_active_face();          // <-- `out` is STILL OPEN on this line
//
// Opening an ofstream TRUNCATES the file immediately, but the content sits in
// the stream's buffer until it is flushed or the stream is destroyed. The
// reload therefore re-read a TRUNCATED file, json parsing threw, the config
// fell back to {} and the entire `blink_anim` block vanished — so every
// per-expression blink setting read back as "not configured" and the menu's
// radio snapped to "Use Face Default" and looked permanently stuck.
//
// The cruel part: by the time anyone inspected the file from a shell, the
// function had returned, the stream had destructed and flushed, and the file on
// disk was perfectly correct. The file and the running program genuinely
// disagreed, and both observations were true.
//
// Writing a sibling temp file and renaming it removes the window entirely: a
// concurrent reader sees either the whole old file or the whole new one.

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace face {

// Returns false if the file could not be written. ⚠ CHECK IT: callers that
// reload from the file afterwards must skip the reload on failure, or they will
// load stale (or absent) data and quietly present it as the truth.
inline bool write_json_atomic(const std::filesystem::path& path,
                              const nlohmann::json& j) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (!fs::is_directory(path.parent_path(), ec)) return false;
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << "\n";
        out.flush();
        if (!out) { fs::remove(tmp, ec); return false; }
    }   // closed here — the bytes are on disk before the rename below
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

}  // namespace face

#include "profile_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

void ProfileManager::init(const std::string& profiles_dir) {
    dir_ = profiles_dir;
    try {
        fs::create_directories(dir_);
    } catch (const std::exception& e) {
        std::cerr << "[profiles] cannot create " << dir_ << ": " << e.what() << "\n";
    }
    scan();
}

void ProfileManager::scan() {
    names_.clear();
    if (dir_.empty()) return;
    try {
        for (const auto& ent : fs::directory_iterator(dir_)) {
            if (!ent.is_regular_file()) continue;
            const fs::path& p = ent.path();
            if (p.extension() != ".json") continue;
            names_.push_back(p.stem().string());
        }
    } catch (const std::exception& e) {
        std::cerr << "[profiles] scan failed: " << e.what() << "\n";
    }
    std::sort(names_.begin(), names_.end(),
              [](const std::string& a, const std::string& b) {
                  // case-insensitive, stable-ish ordering
                  auto la = a, lb = b;
                  std::transform(la.begin(), la.end(), la.begin(), ::tolower);
                  std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
                  return la < lb;
              });
}

const std::string& ProfileManager::name(int i) const {
    static const std::string empty;
    if (i < 0 || i >= static_cast<int>(names_.size())) return empty;
    return names_[i];
}

std::string ProfileManager::path(int i) const {
    if (i < 0 || i >= static_cast<int>(names_.size())) return std::string();
    return path_for(names_[i]);
}

std::string ProfileManager::path_for(const std::string& nm) const {
    if (dir_.empty() || nm.empty()) return std::string();
    return (fs::path(dir_) / (nm + ".json")).string();
}

int ProfileManager::index_of(const std::string& nm) const {
    for (int i = 0; i < static_cast<int>(names_.size()); ++i)
        if (names_[i] == nm) return i;
    return -1;
}

std::string ProfileManager::last_name() const {
    if (dir_.empty()) return std::string();
    std::ifstream f((fs::path(dir_) / ".last").string());
    if (!f) return std::string();
    std::string nm;
    std::getline(f, nm);
    // trim trailing whitespace/newline
    while (!nm.empty() && (nm.back() == '\n' || nm.back() == '\r' || nm.back() == ' '))
        nm.pop_back();
    return nm;
}

std::string ProfileManager::last_path() const {
    std::string nm = last_name();
    if (nm.empty()) return std::string();
    std::string p = path_for(nm);
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::string();
    return p;
}

void ProfileManager::set_last(const std::string& nm) {
    if (dir_.empty()) return;
    std::ofstream f((fs::path(dir_) / ".last").string(), std::ios::trunc);
    if (f) f << nm << "\n";
}

bool ProfileManager::remove(const std::string& nm) {
    std::string p = path_for(nm);
    if (p.empty()) return false;
    std::error_code ec;
    bool ok = fs::remove(p, ec);
    if (ok) {
        if (last_name() == nm) set_last("");   // clear stale last marker
        scan();
    }
    return ok;
}

std::string ProfileManager::sanitize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_')
            out += static_cast<char>(c);
        else
            out += '_';
    }
    // trim leading/trailing spaces and underscores
    auto trimmable = [](char c) { return c == ' ' || c == '_'; };
    size_t b = 0, e = out.size();
    while (b < e && trimmable(out[b])) ++b;
    while (e > b && trimmable(out[e - 1])) --e;
    out = out.substr(b, e - b);
    if (out.size() > 40) out.resize(40);
    if (out.empty()) out = "profile";
    return out;
}

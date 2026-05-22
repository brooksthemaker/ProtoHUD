#pragma once
#include <string>
#include <vector>

// ── Profile manager ─────────────────────────────────────────────────────────────
// A "profile" is a full config snapshot stored as <profiles_dir>/<name>.json.
// Profiles capture everything the exit-save writes (HUD layout, menu style,
// camera/vision settings, Protoface look, etc.) so a convention-goer can switch
// their whole setup in one step.  Applying a profile = relaunch ProtoHUD with the
// profile file as its config path (see main.cpp's re-exec at shutdown).
//
// This class only manages the directory: enumerating profiles, resolving paths,
// remembering the last-loaded one (in a ".last" marker file), and deleting.  The
// actual serialization is done by main.cpp (save_config_to) which owns all the
// runtime state.
class ProfileManager {
public:
    // Point the manager at a directory; creates it if missing, then scans.
    void init(const std::string& profiles_dir);

    // Re-read the directory for *.json profiles (call after a save/delete).
    void scan();

    const std::string& dir() const { return dir_; }

    int count() const { return static_cast<int>(names_.size()); }
    // Base name (no path, no extension); "" if out of range.
    const std::string& name(int i) const;
    // Full path <dir>/<name>.json for the i-th profile; "" if out of range.
    std::string path(int i) const;
    // Full path <dir>/<name>.json for an arbitrary (already-sanitized) name.
    std::string path_for(const std::string& name) const;
    int  index_of(const std::string& name) const;   // -1 if absent
    bool exists(const std::string& name) const { return index_of(name) >= 0; }

    // Last-loaded profile (persisted in <dir>/.last as a bare name).
    std::string last_name() const;                   // "" if none recorded
    std::string last_path() const;                   // "" if none / file gone
    void        set_last(const std::string& name);

    // Delete <dir>/<name>.json (and refresh the list). Returns true on success.
    bool remove(const std::string& name);

    // Turn a free-form, user-typed name into a safe file stem
    // (keeps [A-Za-z0-9 _-], collapses the rest to '_', trims, caps length).
    static std::string sanitize(const std::string& raw);

private:
    std::string              dir_;
    std::vector<std::string> names_;   // sorted, no extension
};

#pragma once
#include <lvgl.h>
#include <string>
#include <functional>
#include "../../storage/sd_card.h"

// Scrollable file browser. Encoder rotate scrolls the list; select enters a
// folder or plays a file; left button goes up one level.

class FileBrowserScreen {
public:
    void create(lv_obj_t* parent);
    void navigate(const std::string& path);  // load and show directory

    // Fired when user selects an audio file.
    std::function<void(const std::string& path)> on_play;

    // Fired when user enters a directory (for queue building).
    std::function<void(const std::string& dir_path)> on_enter_dir;

private:
    static void list_event_cb(lv_event_t* e);

    lv_obj_t*   list_    = nullptr;
    lv_obj_t*   lbl_path_= nullptr;
    std::string cur_path_;
    std::vector<DirEntry> entries_;
};

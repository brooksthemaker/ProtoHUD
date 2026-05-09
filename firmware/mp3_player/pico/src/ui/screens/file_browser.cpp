#include "file_browser.h"
#include "../theme.h"
#include <SD.h>
#include <cstring>

void FileBrowserScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, Theme::BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Path label / breadcrumb.
    lbl_path_ = lv_label_create(parent);
    lv_obj_set_width(lbl_path_, 280);
    lv_obj_align(lbl_path_, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_text_color(lbl_path_, Theme::ACCENT(), 0);
    lv_obj_set_style_text_font(lbl_path_, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_path_, "/");

    // Scrollable list.
    list_ = lv_list_create(parent);
    lv_obj_set_size(list_, 320, 216);
    lv_obj_align(list_, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(list_, Theme::BG(), 0);
    lv_obj_set_style_border_width(list_, 0, 0);
}

void FileBrowserScreen::navigate(const std::string& path) {
    cur_path_ = path;
    lv_label_set_text(lbl_path_, path.c_str());
    lv_obj_clean(list_);  // remove old list items

    // Up-one-level button (not shown at root).
    if (path != "/" && path.size() > 1) {
        lv_obj_t* btn = lv_list_add_btn(list_, LV_SYMBOL_LEFT, ".. (up)");
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 1), Theme::TEXT_DIM(), 0);
        lv_obj_add_event_cb(btn, list_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(-1));
    }

    // List directory contents.
    SdCard sd;  // listing only — sd is already mounted globally
    entries_ = sd.list(path);

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e   = entries_[i];
        const char* icon = e.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO;
        lv_obj_t* btn = lv_list_add_btn(list_, icon, e.name.c_str());
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 1),
                                    e.is_dir ? Theme::ACCENT() : Theme::TEXT(), 0);
        lv_obj_add_event_cb(btn, list_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
}

void FileBrowserScreen::list_event_cb(lv_event_t* e) {
    auto* self    = static_cast<FileBrowserScreen*>(lv_event_get_user_data(e));
    auto* btn     = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const intptr_t idx = reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn));

    if (idx < 0) {
        // Up one level.
        const size_t slash = self->cur_path_.rfind('/');
        std::string parent = (slash == 0) ? "/" : self->cur_path_.substr(0, slash);
        self->navigate(parent);
        return;
    }

    const auto& entry = self->entries_[static_cast<size_t>(idx)];
    if (entry.is_dir) {
        const std::string child = self->cur_path_ == "/"
            ? "/" + entry.name
            : self->cur_path_ + "/" + entry.name;
        self->navigate(child);
        if (self->on_enter_dir) self->on_enter_dir(child);
    } else {
        const std::string full = self->cur_path_ == "/"
            ? "/" + entry.name
            : self->cur_path_ + "/" + entry.name;
        if (self->on_play) self->on_play(full);
    }
}

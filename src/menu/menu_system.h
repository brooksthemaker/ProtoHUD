#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>

struct MenuItem {
    std::string label;
    std::function<void()> action;        // leaf item: execute this
    std::vector<MenuItem> children;      // submenu items
};

// Stack-based menu driven by SmartKnob detents.
// The HUD calls navigate() on knob events and draw() each frame.
// When the selected menu changes, send_detents() callback fires with
// the new detent count so the caller can update the knob haptics.
class MenuSystem {
public:
    using DetentCallback = std::function<void(int count)>;

    explicit MenuSystem(std::vector<MenuItem> root);

    void set_detent_callback(DetentCallback cb) { detent_cb_ = std::move(cb); }

    // Drive from knob events
    void navigate(int direction);   // +1 = next, -1 = prev
    void select();                  // confirm current item (also callable from GPIO)
    void back();                    // pop menu level

    // Render the menu overlay using Dear ImGui windows
    void draw(int screen_w, int screen_h);

    bool is_open()    const { return open_; }
    void open()             { open_ = true;  push_level(root_items_); }
    void close()            { open_ = false; stack_.clear(); }

    int  current_index() const { return cursor_; }
    int  menu_depth()     const { return static_cast<int>(stack_.size()); }
    const std::string& current_label() const;

private:
    struct Level { std::vector<MenuItem> items; };

    void push_level(const std::vector<MenuItem>& items);
    void pop_level();
    void emit_detents();

    std::vector<MenuItem>       root_items_;
    std::vector<Level>          stack_;
    int                         cursor_ = 0;
    bool                        open_   = false;
    DetentCallback              detent_cb_;
};

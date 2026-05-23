#pragma once
#include <functional>
#include <string>
#include <atomic>

// SDL2 gamepad input — optional at compile time (HAVE_SDL2).
// Maps a standard BLE/USB HID gamepad to the same callback interface used
// by GpioButtons, so the rest of the codebase treats both identically.
//
// Button layout (standard SDL_GameControllerButton mapping):
//   A (South)  → select/confirm
//   B (East)   → back/dismiss
//   X (West)   → PiP left toggle
//   Y (North)  → PiP right toggle
//   LB / L1    → autofocus both cameras
//   RB / R1    → capture stereo
//   Start      → toggle menu
//   D-pad      → navigate (up/down/left/right)

#ifdef HAVE_SDL2

class GamepadInput {
public:
    using Cb = std::function<void()>;

    GamepadInput();
    ~GamepadInput();

    // Call once at startup. Returns true if SDL gamecontroller subsystem inited.
    bool init();
    void shutdown();

    // Call once per frame from the main loop (same thread as callbacks).
    void poll();

    bool        connected()       const { return connected_.load(); }
    std::string controller_name() const;
    // Coarse battery level (0–100), or -1 if unknown / wired / disconnected.
    int         battery_pct()     const { return battery_pct_.load(); }

    void on_select    (Cb cb) { select_cb_    = std::move(cb); }
    void on_back      (Cb cb) { back_cb_      = std::move(cb); }
    void on_menu      (Cb cb) { menu_cb_      = std::move(cb); }
    void on_pip_left  (Cb cb) { pip_left_cb_  = std::move(cb); }
    void on_pip_right (Cb cb) { pip_right_cb_ = std::move(cb); }
    void on_af        (Cb cb) { af_cb_        = std::move(cb); }
    void on_capture   (Cb cb) { capture_cb_   = std::move(cb); }
    void on_nav_up    (Cb cb) { nav_up_cb_    = std::move(cb); }
    void on_nav_down  (Cb cb) { nav_down_cb_  = std::move(cb); }
    void on_nav_left  (Cb cb) { nav_left_cb_  = std::move(cb); }
    void on_nav_right (Cb cb) { nav_right_cb_ = std::move(cb); }

private:
    void try_open();
    void close();

    void*              controller_  = nullptr;  // SDL_GameController*
    std::atomic<bool>  connected_   { false };
    std::atomic<int>   battery_pct_ { -1 };

    struct BtnState {
        bool a=0,b=0,x=0,y=0;
        bool lb=0,rb=0,start=0;
        bool dup=0,ddown=0,dleft=0,dright=0;
    } prev_;

    Cb select_cb_, back_cb_, menu_cb_;
    Cb pip_left_cb_, pip_right_cb_;
    Cb af_cb_, capture_cb_;
    Cb nav_up_cb_, nav_down_cb_, nav_left_cb_, nav_right_cb_;
};

#else // !HAVE_SDL2

// Stub — zero overhead when SDL2 is not installed.
class GamepadInput {
public:
    using Cb = std::function<void()>;
    bool        init()            { return false; }
    void        shutdown()        {}
    void        poll()            {}
    bool        connected() const { return false; }
    std::string controller_name() const { return {}; }
    int         battery_pct() const { return -1; }
    void on_select    (Cb) {} void on_back      (Cb) {}
    void on_menu      (Cb) {} void on_pip_left  (Cb) {}
    void on_pip_right (Cb) {} void on_af        (Cb) {}
    void on_capture   (Cb) {} void on_nav_up    (Cb) {}
    void on_nav_down  (Cb) {} void on_nav_left  (Cb) {}
    void on_nav_right (Cb) {}
};

#endif // HAVE_SDL2

#ifdef HAVE_SDL2

#include "gamepad_input.h"
#include <SDL2/SDL.h>
#include <iostream>

GamepadInput::GamepadInput()  = default;
GamepadInput::~GamepadInput() { shutdown(); }

bool GamepadInput::init() {
    // Disable SDL's signal handlers so they don't interfere with the process.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // Allow joystick events even when the SDL window is not focused (we have none).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "[gamepad] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }
    std::cout << "[gamepad] SDL2 gamecontroller subsystem ready\n";
    try_open();
    return true;
}

void GamepadInput::shutdown() {
    close();
    SDL_Quit();
}

std::string GamepadInput::controller_name() const {
    if (!controller_) return {};
    const char* name = SDL_GameControllerName(
        static_cast<SDL_GameController*>(controller_));
    return name ? name : "Unknown";
}

void GamepadInput::try_open() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        controller_ = SDL_GameControllerOpen(i);
        if (controller_) {
            connected_ = true;
            std::cout << "[gamepad] connected: "
                      << SDL_GameControllerName(
                             static_cast<SDL_GameController*>(controller_))
                      << "\n";
            return;
        }
    }
}

void GamepadInput::close() {
    if (controller_) {
        SDL_GameControllerClose(static_cast<SDL_GameController*>(controller_));
        controller_ = nullptr;
    }
    connected_   = false;
    battery_pct_ = -1;
}

void GamepadInput::poll() {
    // Pump SDL's internal event queue so hot-plug events are processed.
    SDL_PumpEvents();

    // Handle connect / disconnect.
    if (controller_) {
        if (!SDL_GameControllerGetAttached(
                static_cast<SDL_GameController*>(controller_))) {
            std::cout << "[gamepad] disconnected\n";
            close();
            prev_ = {};
        }
    } else {
        try_open();
    }

    if (!controller_) return;

    SDL_GameController* gc = static_cast<SDL_GameController*>(controller_);

    // Coarse battery level (SDL only exposes buckets, not a %). Wired/unknown → -1.
    if (SDL_Joystick* js = SDL_GameControllerGetJoystick(gc)) {
        switch (SDL_JoystickCurrentPowerLevel(js)) {
            case SDL_JOYSTICK_POWER_EMPTY:  battery_pct_ = 5;   break;
            case SDL_JOYSTICK_POWER_LOW:    battery_pct_ = 20;  break;
            case SDL_JOYSTICK_POWER_MEDIUM: battery_pct_ = 60;  break;
            case SDL_JOYSTICK_POWER_FULL:   battery_pct_ = 95;  break;
            case SDL_JOYSTICK_POWER_MAX:    battery_pct_ = 100; break;
            default:                        battery_pct_ = -1;  break;  // unknown / wired
        }
    }

    auto edge = [&](SDL_GameControllerButton btn, bool& prev,
                    const Cb& cb) {
        bool now   = SDL_GameControllerGetButton(gc, btn) != 0;
        bool fired = now && !prev;
        prev = now;
        if (fired && cb) cb();
    };

    edge(SDL_CONTROLLER_BUTTON_A,            prev_.a,      select_cb_);
    edge(SDL_CONTROLLER_BUTTON_B,            prev_.b,      back_cb_);
    edge(SDL_CONTROLLER_BUTTON_X,            prev_.x,      pip_left_cb_);
    edge(SDL_CONTROLLER_BUTTON_Y,            prev_.y,      pip_right_cb_);
    edge(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, prev_.lb,     af_cb_);
    edge(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,prev_.rb,     capture_cb_);
    edge(SDL_CONTROLLER_BUTTON_START,        prev_.start,  menu_cb_);
    edge(SDL_CONTROLLER_BUTTON_DPAD_UP,      prev_.dup,    nav_up_cb_);
    edge(SDL_CONTROLLER_BUTTON_DPAD_DOWN,    prev_.ddown,  nav_down_cb_);
    edge(SDL_CONTROLLER_BUTTON_DPAD_LEFT,    prev_.dleft,  nav_left_cb_);
    edge(SDL_CONTROLLER_BUTTON_DPAD_RIGHT,   prev_.dright, nav_right_cb_);
}

#endif // HAVE_SDL2

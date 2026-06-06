#pragma once
// ── gpio_function.h ─────────────────────────────────────────────────────────────
// Functions a GPIO switch can be assigned to (short or long press). Used by the
// configurable GPIO input map (gpio_inputs.*) and the GPIO Buttons menu. Kept
// header-only so both the poller and main.cpp can share the name/id tables.

#include <cstdint>
#include <string>

namespace input {

enum class GpioFunc : int {
    None = 0,
    BoopSnout, BoopLeft, BoopRight, BoopBoth,
    MenuOpen, MenuSelect, MenuBack,
    SystemRestart, SystemShutdown,
    CamAfLeft, CamAfRight,
    CamPipLeft, CamPipRight,
    CamCaptureLeft, CamCaptureRight,
    CamSwap,
    PhoneRing,
    GameToggle,
    Count
};

// Human-readable label for the menu.
inline const char* gpio_func_name(GpioFunc f) {
    switch (f) {
    case GpioFunc::None:            return "None (unused)";
    case GpioFunc::BoopSnout:       return "Boop: Snout";
    case GpioFunc::BoopLeft:        return "Boop: Left Cheek";
    case GpioFunc::BoopRight:       return "Boop: Right Cheek";
    case GpioFunc::BoopBoth:        return "Boop: Both Cheeks";
    case GpioFunc::MenuOpen:        return "Menu: Open/Close";
    case GpioFunc::MenuSelect:      return "Menu: Select";
    case GpioFunc::MenuBack:        return "Menu: Back";
    case GpioFunc::SystemRestart:   return "System: Restart ProtoHUD";
    case GpioFunc::SystemShutdown:  return "System: Power Off";
    case GpioFunc::CamAfLeft:       return "Camera: Autofocus Left";
    case GpioFunc::CamAfRight:      return "Camera: Autofocus Right";
    case GpioFunc::CamPipLeft:      return "Camera: PiP Left Toggle";
    case GpioFunc::CamPipRight:     return "Camera: PiP Right Toggle";
    case GpioFunc::CamCaptureLeft:  return "Camera: Capture Left";
    case GpioFunc::CamCaptureRight: return "Camera: Capture Right";
    case GpioFunc::CamSwap:         return "Camera: Swap L/R";
    case GpioFunc::PhoneRing:       return "Phone: Ring (find)";
    case GpioFunc::GameToggle:      return "Game: Toggle";
    default:                        return "?";
    }
}

// Stable string id used in config.json.
inline const char* gpio_func_id(GpioFunc f) {
    switch (f) {
    case GpioFunc::None:            return "none";
    case GpioFunc::BoopSnout:       return "boop_snout";
    case GpioFunc::BoopLeft:        return "boop_left";
    case GpioFunc::BoopRight:       return "boop_right";
    case GpioFunc::BoopBoth:        return "boop_both";
    case GpioFunc::MenuOpen:        return "menu_open";
    case GpioFunc::MenuSelect:      return "menu_select";
    case GpioFunc::MenuBack:        return "menu_back";
    case GpioFunc::SystemRestart:   return "system_restart";
    case GpioFunc::SystemShutdown:  return "system_shutdown";
    case GpioFunc::CamAfLeft:       return "cam_af_left";
    case GpioFunc::CamAfRight:      return "cam_af_right";
    case GpioFunc::CamPipLeft:      return "cam_pip_left";
    case GpioFunc::CamPipRight:     return "cam_pip_right";
    case GpioFunc::CamCaptureLeft:  return "cam_capture_left";
    case GpioFunc::CamCaptureRight: return "cam_capture_right";
    case GpioFunc::CamSwap:         return "cam_swap";
    case GpioFunc::PhoneRing:       return "phone_ring";
    case GpioFunc::GameToggle:      return "game_toggle";
    default:                        return "none";
    }
}

inline GpioFunc gpio_func_from_id(const std::string& id) {
    for (int i = 0; i < static_cast<int>(GpioFunc::Count); ++i) {
        const auto f = static_cast<GpioFunc>(i);
        if (id == gpio_func_id(f)) return f;
    }
    return GpioFunc::None;
}

inline int gpio_func_count() { return static_cast<int>(GpioFunc::Count); }

} // namespace input

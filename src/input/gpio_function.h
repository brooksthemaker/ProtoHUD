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
    // Jump to a face expression; FaceReturn restores whatever was set before the
    // first jump (so a momentary expression can pop, then snap back).
    FaceNeutral, FaceHappy, FaceAngry, FaceSad, FaceSurprised, FaceReturn,
    // Jump to a material (rainbow + the pride-flag gradients).
    MatRainbow, MatPride, MatProgress, MatTrans, MatBi, MatPan, MatLesbian,
    MatNonbinary, MatAsexual, MatGenderfluid, MatGenderqueer, MatAromantic, MatIntersex,
    // Camera / capture / display helpers — handy on a phone (KDE Connect Run Command).
    CamCaptureStereo, CamZoomIn, CamZoomOut, NightVisionToggle, RecToggle, TheaterToggle,
    XrRecenter,
    // Face browse + look adjust (cycle expression/material/effect, nudge brightness,
    // reboot the face daemon).
    FaceNext, FacePrev, MaterialNext, FaceBrightUp, FaceBrightDown, EffectNext, FaceRestart,
    // MAX7219 section-panel content: next/prev built-in symbol, clear.
    MaxNext, MaxPrev, MaxClear,
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
    case GpioFunc::FaceNeutral:     return "Face: Neutral";
    case GpioFunc::FaceHappy:       return "Face: Happy";
    case GpioFunc::FaceAngry:       return "Face: Angry";
    case GpioFunc::FaceSad:         return "Face: Sad";
    case GpioFunc::FaceSurprised:   return "Face: Surprised";
    case GpioFunc::FaceReturn:      return "Face: Return to Set";
    case GpioFunc::MatRainbow:      return "Material: Rainbow";
    case GpioFunc::MatPride:        return "Material: Pride (Rainbow)";
    case GpioFunc::MatProgress:     return "Material: Progress";
    case GpioFunc::MatTrans:        return "Material: Trans";
    case GpioFunc::MatBi:           return "Material: Bisexual";
    case GpioFunc::MatPan:          return "Material: Pansexual";
    case GpioFunc::MatLesbian:      return "Material: Lesbian";
    case GpioFunc::MatNonbinary:    return "Material: Nonbinary";
    case GpioFunc::MatAsexual:      return "Material: Asexual";
    case GpioFunc::MatGenderfluid:  return "Material: Genderfluid";
    case GpioFunc::MatGenderqueer:  return "Material: Genderqueer";
    case GpioFunc::MatAromantic:    return "Material: Aromantic";
    case GpioFunc::MatIntersex:     return "Material: Intersex";
    case GpioFunc::CamCaptureStereo:  return "Camera: Capture Stereo";
    case GpioFunc::CamZoomIn:         return "Camera: Zoom In";
    case GpioFunc::CamZoomOut:        return "Camera: Zoom Out";
    case GpioFunc::NightVisionToggle: return "Camera: Night Vision Toggle";
    case GpioFunc::RecToggle:         return "Camera: Record Toggle";
    case GpioFunc::TheaterToggle:     return "Display: Theater Mode Toggle";
    case GpioFunc::XrRecenter:        return "XR: Recenter Display";
    case GpioFunc::FaceNext:          return "Face: Next Expression";
    case GpioFunc::FacePrev:          return "Face: Prev Expression";
    case GpioFunc::MaterialNext:      return "Material: Next";
    case GpioFunc::FaceBrightUp:      return "Face: Brightness Up";
    case GpioFunc::FaceBrightDown:    return "Face: Brightness Down";
    case GpioFunc::EffectNext:        return "Face: Next Effect";
    case GpioFunc::FaceRestart:       return "Face: Reboot ProtoFace";
    case GpioFunc::MaxNext:           return "MAX Panels: Next Symbol";
    case GpioFunc::MaxPrev:           return "MAX Panels: Prev Symbol";
    case GpioFunc::MaxClear:          return "MAX Panels: Clear";
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
    case GpioFunc::FaceNeutral:     return "face_neutral";
    case GpioFunc::FaceHappy:       return "face_happy";
    case GpioFunc::FaceAngry:       return "face_angry";
    case GpioFunc::FaceSad:         return "face_sad";
    case GpioFunc::FaceSurprised:   return "face_surprised";
    case GpioFunc::FaceReturn:      return "face_return";
    case GpioFunc::MatRainbow:      return "material_rainbow";
    case GpioFunc::MatPride:        return "material_pride";
    case GpioFunc::MatProgress:     return "material_progress";
    case GpioFunc::MatTrans:        return "material_trans";
    case GpioFunc::MatBi:           return "material_bisexual";
    case GpioFunc::MatPan:          return "material_pansexual";
    case GpioFunc::MatLesbian:      return "material_lesbian";
    case GpioFunc::MatNonbinary:    return "material_nonbinary";
    case GpioFunc::MatAsexual:      return "material_asexual";
    case GpioFunc::MatGenderfluid:  return "material_genderfluid";
    case GpioFunc::MatGenderqueer:  return "material_genderqueer";
    case GpioFunc::MatAromantic:    return "material_aromantic";
    case GpioFunc::MatIntersex:     return "material_intersex";
    case GpioFunc::CamCaptureStereo:  return "cam_capture_stereo";
    case GpioFunc::CamZoomIn:         return "cam_zoom_in";
    case GpioFunc::CamZoomOut:        return "cam_zoom_out";
    case GpioFunc::NightVisionToggle: return "nv_toggle";
    case GpioFunc::RecToggle:         return "rec_toggle";
    case GpioFunc::TheaterToggle:     return "theater_toggle";
    case GpioFunc::XrRecenter:        return "xr_recenter";
    case GpioFunc::FaceNext:          return "face_next";
    case GpioFunc::FacePrev:          return "face_prev";
    case GpioFunc::MaterialNext:      return "material_next";
    case GpioFunc::FaceBrightUp:      return "face_bright_up";
    case GpioFunc::FaceBrightDown:    return "face_bright_down";
    case GpioFunc::EffectNext:        return "effect_next";
    case GpioFunc::FaceRestart:       return "face_restart";
    case GpioFunc::MaxNext:           return "max_next";
    case GpioFunc::MaxPrev:           return "max_prev";
    case GpioFunc::MaxClear:          return "max_clear";
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

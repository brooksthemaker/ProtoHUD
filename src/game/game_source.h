#pragma once
// ── game_source.h ────────────────────────────────────────────────────────────
// Abstraction for an in-process "game" that renders RGBA frames and consumes a
// held-button mask. ProtoHUD ticks the active source once per render frame, then
// shows its frame on BOTH outputs: fullscreen (or windowed) on the XR glasses and
// downscaled on the HUB75 panels. The controller / keyboard are mapped to the
// abstract Button bits below, so a source never touches SDL or GL.
//
// The built-in SnakeGame is the reference source; a future DoomGame (doomgeneric)
// implements the same interface — its DG_DrawFrame fills frame() and DG_GetKey
// reads the button mask.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <opencv2/core.hpp>

namespace game {

// Held-button mask (OR'd together). Mapped from the gamepad/keyboard by the host.
enum Button : uint32_t {
    BtnUp     = 1u << 0,
    BtnDown   = 1u << 1,
    BtnLeft   = 1u << 2,
    BtnRight  = 1u << 3,
    BtnA      = 1u << 4,   // primary / fire / confirm
    BtnB      = 1u << 5,   // secondary / use / back
    BtnX      = 1u << 6,
    BtnY      = 1u << 7,
    BtnL      = 1u << 8,
    BtnR      = 1u << 9,
    BtnStart  = 1u << 10,  // pause / restart
    BtnSelect = 1u << 11,
};

class GameSource {
public:
    virtual ~GameSource() = default;

    virtual const char* name() const = 0;

    // Restart from a clean state (new game).
    virtual void reset() = 0;

    // Advance by dt seconds given the currently-held buttons; updates frame().
    virtual void tick(double dt, uint32_t buttons) = 0;

    // Latest rendered frame — RGBA (CV_8UC4), the source's native resolution.
    // The host scales it per output (fullscreen glasses + downscaled HUB75).
    virtual const cv::Mat& frame() const = 0;

    // ── Runtime options (libretro core options) ───────────────────────────────
    // A source may expose a list of named multiple-choice options discovered when
    // it loads (e.g. an emulator core's internal resolution / region / BIOS). The
    // host surfaces these in the menu. Default: no options. All calls happen on
    // the render thread (same as tick), so no locking is required.
    virtual int         option_count() const { return 0; }
    // "Title: currentValue" for row i (i in [0, option_count)).
    virtual std::string option_label(int /*i*/) const { return {}; }
    // Stable key / current value of option i — used to persist choices to config.
    virtual std::string option_key(int /*i*/)   const { return {}; }
    virtual std::string option_value(int /*i*/) const { return {}; }
    // Advance option i by dir (+1 / -1), wrapping; takes effect on the next tick.
    virtual void        option_cycle(int /*i*/, int /*dir*/) {}

    // Enable/disable this source's audio output at runtime (default: no-op).
    virtual void        set_audio_enabled(bool /*on*/) {}
};

// Reference game (no external assets) used to validate the pipeline.
std::unique_ptr<GameSource> make_snake();

// Doom (doomgeneric) source. Built only in the Doom-enabled build
// (PROTOHUD_HAVE_DOOM); needs a DOOM / Freedoom IWAD at wad_path. The
// declaration is always visible, but the factory is only linked when Doom is
// compiled in — guard call sites with #ifdef PROTOHUD_HAVE_DOOM.
std::unique_ptr<GameSource> make_doom(const std::string& wad_path);

// Libretro core source (the emulator ABI behind RetroArch). Built only in the
// libretro-enabled build (PROTOHUD_HAVE_LIBRETRO). dlopen()s a software- or
// GLES-hardware-rendered core (.so) and runs the ROM; system_dir is handed back
// for BIOS/saves. audio_device is an ALSA playback device for the core's audio
// (empty string disables audio). option_overrides pins core-option values by key
// (from config) so they survive restarts. Guard call sites with PROTOHUD_HAVE_LIBRETRO.
std::unique_ptr<GameSource> make_libretro(
    const std::string& core_path,
    const std::string& rom_path,
    const std::string& system_dir,
    const std::string& audio_device,
    const std::vector<std::pair<std::string, std::string>>& option_overrides);

}  // namespace game

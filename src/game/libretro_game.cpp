// ── libretro_game.cpp ────────────────────────────────────────────────────────
// GameSource backed by a libretro core (the emulator ABI behind RetroArch). A
// core is a shared library exposing ~25 retro_* symbols; the frontend dlopen()s
// it, feeds input, calls retro_run() once per emulated frame, and receives a
// video framebuffer through a callback. That maps cleanly onto GameSource:
//   tick()   → pace + retro_run()      frame() → the converted RGBA framebuffer
//   buttons  → retro_input_state       (held mask, sampled once per tick)
//
// SCOPE: software-rendered cores only (NES/SNES/Genesis/GB/GBA, PS1-software,
// arcade, …). Hardware-rendered cores (N64/Dreamcast/PSP) ask for a GL render
// target via RETRO_ENVIRONMENT_SET_HW_RENDER, which we currently refuse — that
// path (sharing ProtoHUD's GLES context) is a planned follow-up. Audio is
// dropped for now (a silent no-op batch sink), like the Doom source.
//
// Built only when PROTOHUD_HAVE_LIBRETRO is defined (see CMakeLists). No core or
// ROM ships with ProtoHUD; point game.libretro_core / game.libretro_rom at your
// own files (e.g. snes9x_libretro.so + a legally-obtained ROM).

#include "game_source.h"

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include <opencv2/imgproc.hpp>

extern "C" {
#include "libretro.h"
}

namespace game {
namespace {

// The libretro callbacks are plain C function pointers with no user-data slot,
// so (like doomgeneric) we route them through a single live-instance pointer.
class LibretroGame;
LibretroGame* g_lr = nullptr;

class LibretroGame : public GameSource {
public:
    LibretroGame(std::string core, std::string rom, std::string sysdir)
        : core_path_(std::move(core)), rom_path_(std::move(rom)),
          sys_dir_(std::move(sysdir)) {
        frame_.create(240, 320, CV_8UC4);
        frame_.setTo(cv::Scalar(0, 0, 0, 255));
    }
    ~LibretroGame() override {
        if (loaded_) { sym_.retro_unload_game(); sym_.retro_deinit(); }
        if (handle_)  dlclose(handle_);
        if (g_lr == this) g_lr = nullptr;
    }

    const char* name() const override { return "Libretro"; }

    // No re-init: a core is started once; "reset" maps to retro_reset (soft reset)
    // once it's running, which is what a player expects from a reset button.
    void reset() override { if (loaded_) sym_.retro_reset(); }

    void tick(double dt, uint32_t buttons) override {
        if (failed_) return;
        if (!loaded_) { if (!start()) { failed_ = true; } return; }

        buttons_ = buttons;
        // Pace retro_run() to the core's native fps so games run at real speed
        // regardless of the glasses' refresh rate. Cap the catch-up to avoid a
        // spiral of death after a stall.
        const double step = (fps_ > 1.0) ? 1.0 / fps_ : 1.0 / 60.0;
        acc_ += dt;
        int ran = 0;
        while (acc_ >= step && ran < 4) { sym_.retro_run(); acc_ -= step; ++ran; }
        if (ran == 0 && !ran_once_) { sym_.retro_run(); ran_once_ = true; }  // first frame
    }

    const cv::Mat& frame() const override { return frame_; }

    // ── libretro C callbacks (via g_lr) ───────────────────────────────────────
    bool on_environment(unsigned cmd, void* data) {
        switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            pixfmt_ = *static_cast<const enum retro_pixel_format*>(data);
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;       // we keep the last frame on NULL
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = sys_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = false;      // we never change core options
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            auto* cb = static_cast<struct retro_log_callback*>(data);
            cb->log = &LibretroGame::log_trampoline;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            return true;                            // accepted, ignored
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return false;                           // software only (for now)
        default:
            return false;
        }
    }

    void on_video(const void* data, unsigned w, unsigned h, size_t pitch) {
        if (!data || w == 0 || h == 0) return;       // dupe: keep previous frame
        if (frame_.cols != (int)w || frame_.rows != (int)h)
            frame_.create((int)h, (int)w, CV_8UC4);
        switch (pixfmt_) {
            case RETRO_PIXEL_FORMAT_XRGB8888: conv_xrgb8888(data, w, h, pitch); break;
            case RETRO_PIXEL_FORMAT_RGB565:   conv_rgb565  (data, w, h, pitch); break;
            case RETRO_PIXEL_FORMAT_0RGB1555:
            default:                          conv_0rgb1555(data, w, h, pitch); break;
        }
    }

    int16_t on_input_state(unsigned port, unsigned device, unsigned, unsigned id) {
        if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
        uint32_t bit = 0;
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_UP:     bit = BtnUp;     break;
            case RETRO_DEVICE_ID_JOYPAD_DOWN:   bit = BtnDown;   break;
            case RETRO_DEVICE_ID_JOYPAD_LEFT:   bit = BtnLeft;   break;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT:  bit = BtnRight;  break;
            case RETRO_DEVICE_ID_JOYPAD_A:      bit = BtnA;      break;
            case RETRO_DEVICE_ID_JOYPAD_B:      bit = BtnB;      break;
            case RETRO_DEVICE_ID_JOYPAD_X:      bit = BtnX;      break;
            case RETRO_DEVICE_ID_JOYPAD_Y:      bit = BtnY;      break;
            case RETRO_DEVICE_ID_JOYPAD_L:      bit = BtnL;      break;
            case RETRO_DEVICE_ID_JOYPAD_R:      bit = BtnR;      break;
            case RETRO_DEVICE_ID_JOYPAD_START:  bit = BtnStart;  break;
            case RETRO_DEVICE_ID_JOYPAD_SELECT: bit = BtnSelect; break;
            default: return 0;
        }
        return (buttons_ & bit) ? 1 : 0;
    }

private:
    // All the core entry points we resolve from the .so.
    struct Syms {
        void (*retro_init)();
        void (*retro_deinit)();
        void (*retro_set_environment)(retro_environment_t);
        void (*retro_set_video_refresh)(retro_video_refresh_t);
        void (*retro_set_audio_sample)(retro_audio_sample_t);
        void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
        void (*retro_set_input_poll)(retro_input_poll_t);
        void (*retro_set_input_state)(retro_input_state_t);
        void (*retro_get_system_info)(struct retro_system_info*);
        void (*retro_get_system_av_info)(struct retro_system_av_info*);
        bool (*retro_load_game)(const struct retro_game_info*);
        void (*retro_unload_game)();
        void (*retro_run)();
        void (*retro_reset)();
    } sym_{};

    bool resolve() {
        auto get = [&](auto& fp, const char* n) {
            fp = reinterpret_cast<std::remove_reference_t<decltype(fp)>>(dlsym(handle_, n));
            return fp != nullptr;
        };
        return get(sym_.retro_init, "retro_init")
            && get(sym_.retro_deinit, "retro_deinit")
            && get(sym_.retro_set_environment, "retro_set_environment")
            && get(sym_.retro_set_video_refresh, "retro_set_video_refresh")
            && get(sym_.retro_set_audio_sample, "retro_set_audio_sample")
            && get(sym_.retro_set_audio_sample_batch, "retro_set_audio_sample_batch")
            && get(sym_.retro_set_input_poll, "retro_set_input_poll")
            && get(sym_.retro_set_input_state, "retro_set_input_state")
            && get(sym_.retro_get_system_info, "retro_get_system_info")
            && get(sym_.retro_get_system_av_info, "retro_get_system_av_info")
            && get(sym_.retro_load_game, "retro_load_game")
            && get(sym_.retro_unload_game, "retro_unload_game")
            && get(sym_.retro_run, "retro_run")
            && get(sym_.retro_reset, "retro_reset");
    }

    bool start() {
        if (core_path_.empty()) { msg("No libretro core set", "game.libretro_core"); return false; }
        handle_ = dlopen(core_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) { msg("Core failed to load", dlerror_str()); return false; }
        if (!resolve()) { msg("Core missing retro_* symbols", core_path_); return false; }

        g_lr = this;
        sym_.retro_set_environment(&LibretroGame::env_trampoline);
        sym_.retro_set_video_refresh(&LibretroGame::video_trampoline);
        sym_.retro_set_audio_sample(&LibretroGame::audio_sample_trampoline);
        sym_.retro_set_audio_sample_batch(&LibretroGame::audio_batch_trampoline);
        sym_.retro_set_input_poll(&LibretroGame::input_poll_trampoline);
        sym_.retro_set_input_state(&LibretroGame::input_state_trampoline);
        sym_.retro_init();

        struct retro_system_info si{};
        sym_.retro_get_system_info(&si);
        need_fullpath_ = si.need_fullpath;

        // Load the ROM. need_fullpath cores read the file themselves (path only);
        // others want the bytes in memory.
        struct retro_game_info gi{};
        gi.path = rom_path_.empty() ? nullptr : rom_path_.c_str();
        std::string buf;
        if (!need_fullpath_ && !rom_path_.empty()) {
            if (FILE* f = std::fopen(rom_path_.c_str(), "rb")) {
                std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
                if (n > 0) { buf.resize((size_t)n); if (std::fread(&buf[0], 1, (size_t)n, f) != (size_t)n) buf.clear(); }
                std::fclose(f);
            }
            if (buf.empty()) { msg("ROM not found / unreadable", rom_path_); return false; }
            gi.data = buf.data(); gi.size = buf.size();
        } else if (need_fullpath_ && rom_path_.empty()) {
            msg("This core needs a ROM path", "game.libretro_rom"); return false;
        }

        if (!sym_.retro_load_game(&gi)) { msg("Core rejected the ROM", rom_path_); return false; }

        struct retro_system_av_info av{};
        sym_.retro_get_system_av_info(&av);
        fps_ = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
        if (av.geometry.base_width && av.geometry.base_height)
            frame_.create((int)av.geometry.base_height, (int)av.geometry.base_width, CV_8UC4);
        loaded_ = true;
        return true;
    }

    // ── pixel conversion → opaque RGBA ────────────────────────────────────────
    void conv_xrgb8888(const void* src, unsigned w, unsigned h, size_t pitch) {
        for (unsigned y = 0; y < h; ++y) {
            const uint32_t* in = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(src) + y * pitch);
            uint8_t* out = frame_.ptr<uint8_t>((int)y);
            for (unsigned x = 0; x < w; ++x) {
                uint32_t p = in[x];                 // 0x00RRGGBB
                *out++ = (p >> 16) & 0xFF;          // R
                *out++ = (p >> 8)  & 0xFF;          // G
                *out++ =  p        & 0xFF;          // B
                *out++ = 0xFF;
            }
        }
    }
    void conv_rgb565(const void* src, unsigned w, unsigned h, size_t pitch) {
        for (unsigned y = 0; y < h; ++y) {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(src) + y * pitch);
            uint8_t* out = frame_.ptr<uint8_t>((int)y);
            for (unsigned x = 0; x < w; ++x) {
                uint16_t p = in[x];                 // RRRRRGGGGGGBBBBB
                uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                *out++ = (r << 3) | (r >> 2);
                *out++ = (g << 2) | (g >> 4);
                *out++ = (b << 3) | (b >> 2);
                *out++ = 0xFF;
            }
        }
    }
    void conv_0rgb1555(const void* src, unsigned w, unsigned h, size_t pitch) {
        for (unsigned y = 0; y < h; ++y) {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(src) + y * pitch);
            uint8_t* out = frame_.ptr<uint8_t>((int)y);
            for (unsigned x = 0; x < w; ++x) {
                uint16_t p = in[x];                 // 0RRRRRGGGGGBBBBB
                uint8_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
                *out++ = (r << 3) | (r >> 2);
                *out++ = (g << 3) | (g >> 2);
                *out++ = (b << 3) | (b >> 2);
                *out++ = 0xFF;
            }
        }
    }

    void msg(const std::string& l1, const std::string& l2) {
        frame_.create(240, 320, CV_8UC4);
        frame_.setTo(cv::Scalar(8, 8, 12, 255));
        cv::putText(frame_, l1, {12, 112}, cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    cv::Scalar(230, 80, 80, 255), 1);
        if (!l2.empty())
            cv::putText(frame_, l2.substr(0, 40), {12, 140}, cv::FONT_HERSHEY_SIMPLEX, 0.4,
                        cv::Scalar(180, 180, 190, 255), 1);
    }
    static const char* dlerror_str() { const char* e = dlerror(); return e ? e : "?"; }

    // ── trampolines (C ABI → instance) ────────────────────────────────────────
    static bool env_trampoline(unsigned cmd, void* data) {
        return g_lr ? g_lr->on_environment(cmd, data) : false;
    }
    static void video_trampoline(const void* d, unsigned w, unsigned h, size_t p) {
        if (g_lr) g_lr->on_video(d, w, h, p);
    }
    static void input_poll_trampoline() {}
    static int16_t input_state_trampoline(unsigned port, unsigned dev, unsigned idx, unsigned id) {
        return g_lr ? g_lr->on_input_state(port, dev, idx, id) : 0;
    }
    static void audio_sample_trampoline(int16_t, int16_t) {}
    static size_t audio_batch_trampoline(const int16_t*, size_t frames) { return frames; }
    static void log_trampoline(enum retro_log_level, const char*, ...) {}

    std::string core_path_, rom_path_, sys_dir_;
    void*       handle_  = nullptr;
    bool        loaded_  = false;
    bool        failed_  = false;
    bool        need_fullpath_ = false;
    bool        ran_once_ = false;
    enum retro_pixel_format pixfmt_ = RETRO_PIXEL_FORMAT_0RGB1555;
    cv::Mat     frame_;
    double      fps_ = 60.0;
    double      acc_ = 0.0;
    uint32_t    buttons_ = 0;
};

}  // namespace

std::unique_ptr<GameSource> make_libretro(const std::string& core_path,
                                          const std::string& rom_path,
                                          const std::string& system_dir) {
    return std::make_unique<LibretroGame>(core_path, rom_path, system_dir);
}

}  // namespace game

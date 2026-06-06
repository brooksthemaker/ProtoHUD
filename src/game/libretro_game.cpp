// ── libretro_game.cpp ────────────────────────────────────────────────────────
// GameSource backed by a libretro core (the emulator ABI behind RetroArch). A
// core is a shared library exposing ~25 retro_* symbols; the frontend dlopen()s
// it, feeds input, calls retro_run() once per emulated frame, and receives a
// video framebuffer through a callback. That maps cleanly onto GameSource:
//   tick()   → pace + retro_run()      frame() → the converted RGBA framebuffer
//   buttons  → retro_input_state       (held mask, sampled once per tick)
//
// SCOPE: software-rendered cores (NES/SNES/Genesis/GB/GBA, PS1-software, arcade)
// AND hardware-rendered OpenGL ES cores (N64/Dreamcast/PSP) via the libretro HW
// render interface: the core renders into a frontend-provided FBO, and we read
// it back into the RGBA frame each tick (a readback we need anyway to mirror to
// the HUB75 panels). Desktop-GL / Vulkan cores are declined. Audio is dropped
// for now (a silent no-op batch sink), like the Doom source.
//
// Built only when PROTOHUD_HAVE_LIBRETRO is defined (see CMakeLists). No core or
// ROM ships with ProtoHUD; point game.libretro_core / game.libretro_rom at your
// own files (e.g. snes9x_libretro.so + a legally-obtained ROM).

#include "game_source.h"

#include <dlfcn.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <alsa/asoundlib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <opencv2/imgproc.hpp>

extern "C" {
#include "libretro.h"
}

// Packed depth/stencil renderbuffer (OES ext) — declare the enums so we can try
// it for HW cores that need depth+stencil without pulling in gl2ext.h.
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_OES
#define GL_DEPTH_STENCIL_OES 0x84F9
#endif

namespace game {
namespace {

// ── ALSA playback sink for core audio ─────────────────────────────────────────
// A core delivers interleaved stereo S16 from inside retro_run() (render thread);
// we copy it into a ring buffer that a dedicated thread drains to ALSA with
// blocking writes, so audio never stalls the render loop. ALSA soft-resample is
// enabled, so the device can run at the core's native sample rate even if the
// hardware can't. Self-contained: it doesn't touch ProtoHUD's spatial AudioEngine.
class AudioSink {
public:
    AudioSink(std::string device, int rate) : dev_(std::move(device)), rate_(rate) {}
    ~AudioSink() { stop(); }

    bool start() {
        if (dev_.empty() || rate_ <= 0) return false;
        if (snd_pcm_open(&pcm_, dev_.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            pcm_ = nullptr; return false;
        }
        // S16 stereo, soft-resample on, ~120 ms of buffering.
        if (snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               2, (unsigned)rate_, 1, 120000) < 0) {
            snd_pcm_close(pcm_); pcm_ = nullptr; return false;
        }
        ring_.assign((size_t)rate_ * 2, 0);   // ~1 s capacity (samples, not frames)
        running_.store(true);
        thr_ = std::thread([this]{ run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) { if (pcm_) { snd_pcm_close(pcm_); pcm_ = nullptr; } return; }
        cv_.notify_all();
        if (thr_.joinable()) thr_.join();
        if (pcm_) { snd_pcm_close(pcm_); pcm_ = nullptr; }
    }

    // Interleaved stereo, `frames` sample-pairs. Never blocks: drops on overflow.
    void push(const int16_t* data, size_t frames) {
        if (!running_.load()) return;
        std::unique_lock<std::mutex> lk(mtx_);
        const size_t cap = ring_.size();
        for (size_t i = 0; i < frames * 2; ++i) {
            size_t next = (w_ + 1) % cap;
            if (next == r_) break;               // full → drop the rest
            ring_[w_] = data[i]; w_ = next;
        }
        lk.unlock();
        cv_.notify_one();
    }

private:
    void run() {
        std::vector<int16_t> chunk(1024 * 2);
        while (running_.load()) {
            size_t got = 0;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(20),
                             [this]{ return r_ != w_ || !running_.load(); });
                while (got < chunk.size() && r_ != w_) {
                    chunk[got++] = ring_[r_]; r_ = (r_ + 1) % ring_.size();
                }
            }
            if (!running_.load()) break;
            if (got == 0) continue;
            size_t frames = got / 2, off = 0;
            while (frames > 0 && running_.load()) {
                snd_pcm_sframes_t wr = snd_pcm_writei(pcm_, chunk.data() + off * 2, frames);
                if (wr < 0) { snd_pcm_recover(pcm_, (int)wr, 1); continue; }
                frames -= (size_t)wr; off += (size_t)wr;
            }
        }
    }

    std::string          dev_;
    int                  rate_ = 0;
    snd_pcm_t*           pcm_  = nullptr;
    std::vector<int16_t> ring_;
    size_t               r_ = 0, w_ = 0;
    std::mutex           mtx_;
    std::condition_variable cv_;
    std::atomic<bool>    running_{false};
    std::thread          thr_;
};

// The libretro callbacks are plain C function pointers with no user-data slot,
// so (like doomgeneric) we route them through a single live-instance pointer.
class LibretroGame;
LibretroGame* g_lr = nullptr;

class LibretroGame : public GameSource {
public:
    LibretroGame(std::string core, std::string rom, std::string sysdir, std::string audio_dev,
                 std::vector<std::pair<std::string, std::string>> overrides)
        : core_path_(std::move(core)), rom_path_(std::move(rom)),
          sys_dir_(std::move(sysdir)), audio_dev_(std::move(audio_dev)),
          overrides_(std::move(overrides)) {
        frame_.create(240, 320, CV_8UC4);
        frame_.setTo(cv::Scalar(0, 0, 0, 255));
    }
    ~LibretroGame() override {
        audio_.reset();   // stop the playback thread before tearing the core down
        if (hw_ && hw_cb_.context_destroy) hw_cb_.context_destroy();
        if (loaded_) { sym_.retro_unload_game(); sym_.retro_deinit(); }
        if (hw_fbo_) glDeleteFramebuffers(1, &hw_fbo_);
        if (hw_tex_) glDeleteTextures(1, &hw_tex_);
        if (hw_ds_)  glDeleteRenderbuffers(1, &hw_ds_);
        if (handle_) dlclose(handle_);
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

        // A HW core renders through its own GL programs/buffers/state; reset to a
        // neutral state so ProtoHUD's subsequent eye/HUD passes aren't corrupted.
        if (hw_ && ran > 0) restore_gl_state();
    }

    const cv::Mat& frame() const override { return frame_; }

    // ── core options (surfaced in the menu) ───────────────────────────────────
    int option_count() const override { return (int)opts_.size(); }
    std::string option_label(int i) const override {
        if (i < 0 || i >= (int)opts_.size()) return {};
        const auto& o = opts_[i];
        return o.desc + ": " + o.values[o.idx];
    }
    void option_cycle(int i, int dir) override {
        if (i < 0 || i >= (int)opts_.size()) return;
        auto& o = opts_[i];
        const int n = (int)o.values.size();
        o.idx = ((o.idx + dir) % n + n) % n;
        opts_dirty_ = true;                          // core re-reads on next GET_VARIABLE_UPDATE
    }

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
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            // Legacy core-options API. Modern cores fall back to this because we
            // don't advertise a newer core-options version, so this single path
            // covers old and new cores. Parse the {key,"Title; a|b|c"} array.
            parse_variables(static_cast<const struct retro_variable*>(data));
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = static_cast<struct retro_variable*>(data);
            v->value = nullptr;
            for (const auto& o : opts_)
                if (o.key == v->key) { v->value = o.values[o.idx].c_str(); break; }
            return v->value != nullptr;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = opts_dirty_;
            opts_dirty_ = false;                    // edge: cleared once consumed
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            auto* cb = static_cast<struct retro_log_callback*>(data);
            cb->log = &LibretroGame::log_trampoline;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            return true;                            // accepted, ignored
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* cb = static_cast<struct retro_hw_render_callback*>(data);
            // We provide an OpenGL ES context; accept GLES requests, decline
            // desktop GL / Vulkan (the core would fail against our GLES context).
            if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES2 &&
                cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
                cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION)
                return false;
            hw_cb_ = *cb;
            hw_cb_.get_current_framebuffer = &LibretroGame::get_fbo_trampoline;
            hw_cb_.get_proc_address        = &LibretroGame::get_proc_trampoline;
            *cb = hw_cb_;                            // hand our pointers back to the core
            hw_ = true;
            return true;
        }
        default:
            return false;
        }
    }

    void on_video(const void* data, unsigned w, unsigned h, size_t pitch) {
        if (!data || w == 0 || h == 0) return;       // dupe: keep previous frame
        if (frame_.cols != (int)w || frame_.rows != (int)h)
            frame_.create((int)h, (int)w, CV_8UC4);
        // Hardware frame: the core rendered into hw_fbo_. Read it back (bottom-up)
        // and flip to top-down so it matches the software path's orientation.
        if (data == RETRO_HW_FRAME_BUFFER_VALID) {
            if (!hw_fbo_) return;
            GLint prev = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
            glBindFramebuffer(GL_FRAMEBUFFER, hw_fbo_);
            readback_.create((int)h, (int)w, CV_8UC4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE,
                         readback_.data);
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
            cv::flip(readback_, frame_, 0);          // GL bottom-left → image top-left
            // Force opaque — some cores leave alpha at 0, which the eye blit reads.
            const int n = frame_.rows * frame_.cols;
            uint8_t* p = frame_.data;
            for (int i = 0; i < n; ++i) p[i * 4 + 3] = 0xFF;
            return;
        }
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

        // Bring up audio at the core's native sample rate (ALSA soft-resamples to
        // the device). Non-fatal: a game with no audio device still plays muted.
        if (!audio_dev_.empty() && av.timing.sample_rate > 1.0) {
            audio_ = std::make_unique<AudioSink>(audio_dev_, (int)(av.timing.sample_rate + 0.5));
            if (!audio_->start()) audio_.reset();   // couldn't open device → silent
        }

        // For a HW core, build the render-target FBO (sized to the core's max
        // geometry) and signal the core that its GL context is ready.
        if (hw_) {
            int mw = (int)av.geometry.max_width, mh = (int)av.geometry.max_height;
            if (mw <= 0) mw = frame_.cols; if (mh <= 0) mh = frame_.rows;
            if (!build_hw_fbo(mw, mh)) { msg("HW render setup failed", core_path_); return false; }
            if (hw_cb_.context_reset) hw_cb_.context_reset();
            restore_gl_state();
        }
        loaded_ = true;
        return true;
    }

    // Build the color FBO the HW core renders into. Tries a packed depth/stencil
    // renderbuffer (for cores that need both), falling back to plain depth16.
    bool build_hw_fbo(int w, int h) {
        GLint prev_fbo = 0, prev_rb = 0, prev_tex = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

        glGenTextures(1, &hw_tex_);
        glBindTexture(GL_TEXTURE_2D, hw_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &hw_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, hw_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, hw_tex_, 0);

        glGenRenderbuffers(1, &hw_ds_);
        glBindRenderbuffer(GL_RENDERBUFFER, hw_ds_);
        bool packed = false;
        if (hw_cb_.depth) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
            if (glGetError() == GL_NO_ERROR) {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, hw_ds_);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                          GL_RENDERBUFFER, hw_ds_);
                packed = true;
            }
            if (!packed) {
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, hw_ds_);
            }
        }

        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
        return st == GL_FRAMEBUFFER_COMPLETE;
    }

    // Return ProtoHUD's GL to a neutral state after a HW core has rendered.
    void restore_gl_state() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
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

    // Parse a SET_VARIABLES array: each entry is {key, "Title; v1|v2|v3"} where
    // the first value is the default. Honour any config override for the key.
    void parse_variables(const struct retro_variable* vars) {
        opts_.clear();
        if (!vars) return;
        for (const struct retro_variable* v = vars; v->key && v->value; ++v) {
            std::string spec = v->value;
            size_t semi = spec.find(';');
            Opt o;
            o.key  = v->key;
            o.desc = (semi == std::string::npos) ? o.key : spec.substr(0, semi);
            std::string vals = (semi == std::string::npos) ? spec : spec.substr(semi + 1);
            size_t a = vals.find_first_not_of(' ');         // skip the single space after ';'
            if (a == std::string::npos) continue;
            for (size_t p = a; p <= vals.size();) {
                size_t bar = vals.find('|', p);
                std::string tok = vals.substr(p, bar == std::string::npos ? std::string::npos : bar - p);
                if (!tok.empty()) o.values.push_back(tok);
                if (bar == std::string::npos) break;
                p = bar + 1;
            }
            if (o.values.empty()) continue;
            o.idx = 0;                                       // first value = default
            for (const auto& ov : overrides_)               // pin from config if present
                if (ov.first == o.key)
                    for (int k = 0; k < (int)o.values.size(); ++k)
                        if (o.values[k] == ov.second) { o.idx = k; break; }
            opts_.push_back(std::move(o));
        }
        opts_dirty_ = true;
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
    static void audio_sample_trampoline(int16_t l, int16_t r) {
        if (g_lr && g_lr->audio_) { int16_t s[2] = {l, r}; g_lr->audio_->push(s, 1); }
    }
    static size_t audio_batch_trampoline(const int16_t* data, size_t frames) {
        if (g_lr && g_lr->audio_) g_lr->audio_->push(data, frames);
        return frames;
    }
    static void log_trampoline(enum retro_log_level, const char*, ...) {}
    static uintptr_t get_fbo_trampoline() { return g_lr ? (uintptr_t)g_lr->hw_fbo_ : 0; }
    static retro_proc_address_t get_proc_trampoline(const char* sym) {
        return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
    }

    struct Opt {
        std::string key, desc;
        std::vector<std::string> values;
        int idx = 0;
    };
    std::vector<Opt> opts_;
    bool             opts_dirty_ = false;
    std::vector<std::pair<std::string, std::string>> overrides_;

    std::string core_path_, rom_path_, sys_dir_, audio_dev_;
    std::unique_ptr<AudioSink> audio_;
    void*       handle_  = nullptr;
    bool        loaded_  = false;
    bool        failed_  = false;
    bool        need_fullpath_ = false;
    bool        ran_once_ = false;
    enum retro_pixel_format pixfmt_ = RETRO_PIXEL_FORMAT_0RGB1555;
    cv::Mat     frame_;
    cv::Mat     readback_;            // scratch for HW glReadPixels (bottom-up)
    double      fps_ = 60.0;
    double      acc_ = 0.0;
    uint32_t    buttons_ = 0;

    // Hardware-render (OpenGL ES) state. hw_ is set when a core requests an FBO
    // render target via SET_HW_RENDER; we render it into hw_fbo_ and read it back.
    bool        hw_     = false;
    GLuint      hw_fbo_ = 0;
    GLuint      hw_tex_ = 0;
    GLuint      hw_ds_  = 0;          // depth (or packed depth/stencil) renderbuffer
    struct retro_hw_render_callback hw_cb_{};
};

}  // namespace

std::unique_ptr<GameSource> make_libretro(
    const std::string& core_path,
    const std::string& rom_path,
    const std::string& system_dir,
    const std::string& audio_device,
    const std::vector<std::pair<std::string, std::string>>& option_overrides) {
    return std::make_unique<LibretroGame>(core_path, rom_path, system_dir,
                                          audio_device, option_overrides);
}

}  // namespace game

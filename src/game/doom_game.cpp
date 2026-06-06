// ── doom_game.cpp ────────────────────────────────────────────────────────────
// GameSource backed by doomgeneric (a portable, embeddable Doom). doomgeneric is
// all-global C: one DG_ScreenBuffer, one set of DG_* platform callbacks, and no
// support for re-initialisation — so this wraps a single engine instance routed
// through a file-local g_doom pointer. We drive it on the render thread:
//   • first tick()  → doomgeneric_Create (loads the IWAD, renders one frame)
//   • later ticks    → doomgeneric_Tick   (advances + renders one frame)
// The host's held-button mask is diffed into key down/up events for DG_GetKey,
// and DG_DrawFrame copies DG_ScreenBuffer (640×400 BGRA) into our RGBA frame.
//
// Built only when PROTOHUD_HAVE_DOOM is defined (see CMakeLists). The engine
// calls exit() via I_Error on a missing/corrupt IWAD, so tick() verifies the
// WAD exists before ever calling into Doom and otherwise shows an error frame.

#include "game_source.h"

#include <chrono>
#include <cstdint>
#include <vector>
#include <string>
#include <sys/stat.h>

#include <opencv2/imgproc.hpp>

extern "C" {
#include "doomgeneric.h"
#include "doomkeys.h"
}

namespace game {

namespace {

class DoomGame;
DoomGame* g_doom = nullptr;   // the single live engine (doomgeneric is all-global)

class DoomGame : public GameSource {
public:
    explicit DoomGame(std::string wad) : wad_(std::move(wad)) {
        frame_.create(DOOMGENERIC_RESY, DOOMGENERIC_RESX, CV_8UC4);
        frame_.setTo(cv::Scalar(0, 0, 0, 255));
    }
    ~DoomGame() override { if (g_doom == this) g_doom = nullptr; }

    const char* name() const override { return "Doom"; }

    // Doom holds the whole game in global state and can't be re-initialised, so
    // a "reset" can't restart the engine. New games are reached via Doom's own
    // menu (Select/Start map to ESC/Enter); this is a no-op.
    void reset() override {}

    void tick(double /*dt*/, uint32_t buttons) override {
        if (failed_) return;
        if (!started_) {
            // I_Error → exit() would take the whole process down if the IWAD is
            // missing, so check first and fail soft into an error frame instead.
            struct stat st{};
            if (wad_.empty() || stat(wad_.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                render_message("DOOM IWAD not found", wad_);
                failed_ = true;
                return;
            }
            start_us_ = now_us();
            g_doom    = this;
            // argv strings must outlive the engine (Doom keeps myargv).
            argv_storage_ = { std::string("protohud-doom"),
                              std::string("-iwad"), wad_ };
            argv_.clear();
            for (auto& s : argv_storage_) argv_.push_back(const_cast<char*>(s.c_str()));
            doomgeneric_Create(static_cast<int>(argv_.size()), argv_.data());
            started_ = true;
            return;   // Create already rendered the first frame via DG_DrawFrame
        }
        sync_keys(buttons);
        doomgeneric_Tick();   // DG_DrawFrame copies the new frame into frame_
    }

    const cv::Mat& frame() const override { return frame_; }

    // ── Called from the C platform callbacks below ────────────────────────────
    void on_draw() {
        // DG_ScreenBuffer is RESX×RESY 32-bit, memory order B,G,R,A (alpha 0).
        // Convert to opaque RGBA for the host (R,G,B,255).
        cv::Mat bgra(DOOMGENERIC_RESY, DOOMGENERIC_RESX, CV_8UC4, DG_ScreenBuffer);
        cv::Mat bgr;
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr, frame_, cv::COLOR_BGR2RGBA);
    }
    uint32_t ticks_ms() const {
        return static_cast<uint32_t>((now_us() - start_us_) / 1000);
    }
    int pop_key(int* pressed, unsigned char* key) {
        if (kq_r_ == kq_w_) return 0;
        uint16_t kd = kq_[kq_r_];
        kq_r_ = (kq_r_ + 1) % kKQ;
        *pressed = kd >> 8;
        *key     = kd & 0xFF;
        return 1;
    }

private:
    static constexpr int kKQ = 64;

    static int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void push_key(bool pressed, unsigned char k) {
        kq_[kq_w_] = static_cast<uint16_t>((pressed ? 1u : 0u) << 8) | k;
        kq_w_ = (kq_w_ + 1) % kKQ;
        if (kq_w_ == kq_r_) kq_r_ = (kq_r_ + 1) % kKQ;   // drop oldest on overflow
    }

    // Diff the held mask vs. last frame and emit a down/up event per change.
    void sync_keys(uint32_t b) {
        static const struct { uint32_t bit; unsigned char key; } kMap[] = {
            { BtnUp,     KEY_UPARROW    },   // forward
            { BtnDown,   KEY_DOWNARROW  },   // back
            { BtnLeft,   KEY_LEFTARROW  },   // turn left
            { BtnRight,  KEY_RIGHTARROW },   // turn right
            { BtnA,      KEY_FIRE       },   // fire
            { BtnB,      KEY_USE        },   // use / open
            { BtnX,      KEY_RSHIFT     },   // run
            { BtnY,      KEY_TAB        },   // automap
            { BtnL,      KEY_STRAFE_L   },   // strafe left
            { BtnR,      KEY_STRAFE_R   },   // strafe right
            { BtnStart,  KEY_ENTER      },   // menu confirm
            { BtnSelect, KEY_ESCAPE     },   // menu
        };
        for (const auto& m : kMap) {
            const bool now = (b & m.bit) != 0, was = (prev_ & m.bit) != 0;
            if      (now && !was) push_key(true,  m.key);
            else if (!now && was) push_key(false, m.key);
        }
        prev_ = b;
    }

    void render_message(const std::string& l1, const std::string& l2) {
        frame_.setTo(cv::Scalar(8, 8, 12, 255));
        const int cy = DOOMGENERIC_RESY / 2;
        cv::putText(frame_, l1, {30, cy - 12}, cv::FONT_HERSHEY_SIMPLEX, 0.8,
                    cv::Scalar(230, 80, 80, 255), 2);
        if (!l2.empty())
            cv::putText(frame_, l2, {30, cy + 18}, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(180, 180, 190, 255), 1);
        cv::putText(frame_, "Set game.doom_wad to a DOOM / Freedoom .wad",
                    {30, cy + 52}, cv::FONT_HERSHEY_SIMPLEX, 0.45,
                    cv::Scalar(150, 150, 160, 255), 1);
    }

    std::string              wad_;
    std::vector<std::string> argv_storage_;
    std::vector<char*>       argv_;
    cv::Mat   frame_;
    bool      started_  = false;
    bool      failed_   = false;
    int64_t   start_us_ = 0;
    uint32_t  prev_     = 0;
    uint16_t  kq_[kKQ]  = {};
    int       kq_r_     = 0;
    int       kq_w_     = 0;
};

}  // namespace

std::unique_ptr<GameSource> make_doom(const std::string& wad_path) {
    return std::make_unique<DoomGame>(wad_path);
}

}  // namespace game

// ── doomgeneric platform callbacks ────────────────────────────────────────────
// One global engine; route everything through game::g_doom (set on first tick).
extern "C" {

void DG_Init() {}

void DG_DrawFrame() { if (game::g_doom) game::g_doom->on_draw(); }

void DG_SleepMs(uint32_t /*ms*/) { /* the host drives frame timing */ }

uint32_t DG_GetTicksMs() { return game::g_doom ? game::g_doom->ticks_ms() : 0; }

int DG_GetKey(int* pressed, unsigned char* key) {
    return game::g_doom ? game::g_doom->pop_key(pressed, key) : 0;
}

void DG_SetWindowTitle(const char* /*title*/) {}

}  // extern "C"

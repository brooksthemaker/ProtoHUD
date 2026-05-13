#pragma once
#include "../app_state.h"
#include <nanovg.h>
#include <vector>
#include <cstdint>

// ── ToastRenderer ─────────────────────────────────────────────────────────────
// Draws slide-in toast notifications using NanoVG.
// Called from within a live nvgBeginFrame/nvgEndFrame block.
//
// Toasts slide in from the right edge, stack top-to-bottom, and
// auto-dismiss after Notification::auto_dismiss_s seconds.
// Up to 4 toasts are visible simultaneously.

class ToastRenderer {
public:
    // Draw all active toasts. Must be called inside an NVG frame.
    // dt — frame delta in seconds.
    // font_ui, font_mono — NVGcontext font handle IDs.
    void draw(NVGcontext* vg, NotificationQueue& q,
              float fw, float fh, float dt,
              int font_ui, int font_mono);

    // Returns true if any toast with actions is currently focused.
    bool has_focused_toast() const { return focused_id_ != 0; }

    // Cycle the action buttons on the focused toast.
    void navigate(int delta);

    // Fire the currently selected action on the focused toast.
    void select(AppState& state);

private:
    static constexpr float kToastW    = 340.f;
    static constexpr float kToastH    = 72.f;
    static constexpr float kGap       = 6.f;
    static constexpr float kMarginR   = 16.f;
    static constexpr float kMarginT   = 16.f;
    static constexpr int   kMaxVisible = 4;

    struct ToastAnim {
        uint32_t id;
        float    slide_x;     // current x offset from resting position (0 = in place)
        float    alpha;       // 0–1
        float    age;         // seconds since push
    };

    std::vector<ToastAnim> anims_;
    uint32_t focused_id_    = 0;
    int      action_cursor_ = 0;

    void ensure_anim(uint32_t id);
    static ImU32 type_color(NotifType t);
};

#include "toast_renderer.h"
#include <imgui.h>   // for IM_COL32 / ImU32 used in app_state.h
#include <cmath>
#include <cstring>
#include <algorithm>
#include <ctime>

static ImU32 with_alpha_t(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24u);
}

static NVGcolor nvg_col_t(ImU32 c) {
    return nvgRGBA(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF);
}

ImU32 ToastRenderer::type_color(NotifType t) {
    switch (t) {
        case NotifType::Alarm: return IM_COL32(255,  60,  60, 255);
        case NotifType::Timer: return IM_COL32(255, 160,  32, 255);
        case NotifType::LoRa:  return IM_COL32(  0, 180, 255, 255);
        default:               return IM_COL32( 80, 140, 255, 255);
    }
}

void ToastRenderer::ensure_anim(uint32_t id) {
    for (const auto& a : anims_) if (a.id == id) return;
    anims_.push_back({id, kToastW, 0.f, 0.f});
}

static std::string fmt_toast_time(int64_t ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm* tm = localtime(&t);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", tm);
    return buf;
}

void ToastRenderer::draw(NVGcontext* vg, NotificationQueue& q,
                          float fw, float fh, float dt,
                          int font_ui, int font_mono, IconCache* icons) {
    // Remove anims for dismissed/old notifications
    anims_.erase(std::remove_if(anims_.begin(), anims_.end(), [&](const ToastAnim& a) {
        for (const auto& n : q.items) if (n.id == a.id && !n.dismissed) return false;
        return true;
    }), anims_.end());

    // Collect visible (non-dismissed) notifications — newest first, cap at kMaxVisible
    std::vector<Notification*> visible;
    for (auto& n : q.items) {
        if (n.dismissed) continue;
        if ((int)visible.size() >= kMaxVisible) break;
        visible.push_back(&n);
    }

    // Pick focused id: topmost toast that has actions, or first toast
    uint32_t new_focus = 0;
    for (auto* n : visible) {
        if (!n->actions.empty()) { new_focus = n->id; break; }
    }
    if (new_focus != focused_id_) {
        focused_id_    = new_focus;
        action_cursor_ = 0;
    }

    const float rest_x = fw - kToastW - kMarginR;

    int slot = 0;
    for (auto* np : visible) {
        Notification& n = *np;
        ensure_anim(n.id);

        // Find anim
        ToastAnim* anim = nullptr;
        for (auto& a : anims_) if (a.id == n.id) { anim = &a; break; }
        if (!anim) { ++slot; continue; }

        anim->age += dt;

        // Auto-dismiss
        if (n.auto_dismiss_s > 0.f && anim->age > n.auto_dismiss_s) {
            n.dismissed = true; n.read = true; ++slot; continue;
        }

        // Slide in (over 0.2s)
        const float target_slide = 0.f;
        anim->slide_x += (target_slide - anim->slide_x) * std::min(1.f, dt / 0.2f * 5.f);

        // Alpha: fade in fast, fade out last 0.5s
        float life_frac = (n.auto_dismiss_s > 0.f)
            ? (n.auto_dismiss_s - anim->age) / n.auto_dismiss_s
            : 1.f;
        float fade_alpha = (n.auto_dismiss_s > 0.f && life_frac < (0.5f / n.auto_dismiss_s * 1.f))
            ? life_frac / (0.5f / n.auto_dismiss_s)
            : 1.f;
        anim->alpha += (fade_alpha - anim->alpha) * std::min(1.f, dt * 8.f);

        const float alpha = anim->alpha;
        const uint8_t a = static_cast<uint8_t>(alpha * 240.f);

        const float tx = rest_x + anim->slide_x;
        const float ty = kMarginT + slot * (kToastH + kGap);

        const ImU32 tc  = type_color(n.type);
        const bool focused = (n.id == focused_id_);

        // Background
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, ty, kToastW, kToastH, 6.f);
        nvgFillColor(vg, nvgRGBA(10, 15, 20, a));
        nvgFill(vg);

        // Left color bar (4px)
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, ty, 4.f, kToastH, 3.f);
        nvgFillColor(vg, nvg_col_t(with_alpha_t(tc, a)));
        nvgFill(vg);

        // Border
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, ty, kToastW, kToastH, 6.f);
        nvgStrokeColor(vg, nvg_col_t(with_alpha_t(IM_COL32(255,255,255,60), a)));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        // Type icon (left). Per-notification override, else by type. When an icon
        // is drawn, text shifts right to make room; otherwise layout is unchanged.
        float text_x = tx + 12.f;
        if (icons) {
            const std::string& name = n.icon.empty()
                ? std::string(notif_type_icon(n.type)) : n.icon;
            if (icons->draw(vg, name, tx + 26.f, ty + kToastH * 0.5f, 28.f, a / 255.f))
                text_x = tx + 46.f;
        }

        // Title
        nvgFontFaceId(vg, font_ui);
        nvgFontSize(vg, 13.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, a));
        nvgText(vg, text_x, ty + 8.f, n.title.c_str(), nullptr);

        // Timestamp (top-right)
        if (n.timestamp > 0) {
            std::string ts = fmt_toast_time(n.timestamp);
            nvgFontFaceId(vg, font_mono);
            nvgFontSize(vg, 11.f);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvgRGBA(180, 180, 180, a));
            nvgText(vg, tx + kToastW - 8.f, ty + 8.f, ts.c_str(), nullptr);
        }

        // Body
        if (!n.body.empty()) {
            nvgFontFaceId(vg, font_mono);
            nvgFontSize(vg, 12.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvgRGBA(200, 200, 200, a));
            nvgText(vg, text_x, ty + 26.f, n.body.c_str(), nullptr);
        }

        // Action buttons (only shown when focused)
        if (focused && !n.actions.empty()) {
            nvgFontFaceId(vg, font_ui);
            nvgFontSize(vg, 11.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            float bx = text_x;
            const float by = ty + kToastH - 20.f;
            for (int i = 0; i < (int)n.actions.size(); ++i) {
                const char* lbl = n.actions[i].label.c_str();
                float bounds[4];
                nvgTextBounds(vg, 0, 0, lbl, nullptr, bounds);
                float bw = bounds[2] - bounds[0] + 12.f;
                const bool sel = (action_cursor_ == i);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, bx, by, bw, 14.f, 3.f);
                nvgFillColor(vg, sel ? nvg_col_t(with_alpha_t(tc, a))
                                     : nvgRGBA(60, 60, 60, a));
                nvgFill(vg);
                nvgFillColor(vg, nvgRGBA(255, 255, 255, a));
                nvgText(vg, bx + 6.f, by + 2.f, lbl, nullptr);
                bx += bw + 6.f;
            }
        }

        n.read = true;
        ++slot;
    }
}

void ToastRenderer::navigate(int delta) {
    if (focused_id_ == 0) return;
    // Find action count for focused toast — navigate without AppState by cycling cursor
    // Actual clamping happens at draw time
    action_cursor_ += delta;
    if (action_cursor_ < 0) action_cursor_ = 0;
}

void ToastRenderer::select(AppState& state) {
    if (focused_id_ == 0) return;
    for (auto& n : state.notifs.items) {
        if (n.id != focused_id_) continue;
        if (action_cursor_ < (int)n.actions.size()) {
            n.actions[action_cursor_].fn(state);
        }
        n.dismissed = true;
        focused_id_    = 0;
        action_cursor_ = 0;
        return;
    }
}

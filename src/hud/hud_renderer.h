#pragma once
// ── hud_renderer.h ────────────────────────────────────────────────────────────
// Rendering contract (called each frame):
//
//   hud.set_dt(dt);                            // tick particles, cache glow state
//   hud.draw_hud_frame(snap, w, h, show_fps);  // NanoVG HUD chrome pass
//   hud.draw_fps_overlay(snap, w, h, active);  // NanoVG fps counter (own frame)
//   hud.begin_menu_frame();                    // ImGui::NewFrame
//     hud.draw_sys_panel(snap, w, h, active);
//     hud.draw_pip(tex, label, w, h, active, cfg, ...);
//     hud.draw_android_overlay(tex, w, h, ...);
//     hud.draw_panel_preview(tex, w, h, scale);
//     menu.draw(w, h);
//     hud.draw_popups(state, w, h);
//   hud.render_menu_overlay();                 // ImGui::Render + RenderDrawData

#include "../app_state.h"
#include "toast_renderer.h"
#include <imgui.h>
#include <nanovg.h>
#include <string>
#include <deque>
#include <vector>

struct HudColors {
    ImU32 primary    = IM_COL32(  0, 220, 180, 220);
    ImU32 accent     = IM_COL32(  0, 180, 255, 255);
    ImU32 warn       = IM_COL32(255, 180,   0, 255);
    ImU32 danger     = IM_COL32(255,  60,  60, 255);
    ImU32 background = IM_COL32( 10,  15,  20, 195);
    ImU32 panel_bg   = IM_COL32(  8,  12,  18, 210);
    ImU32 text       = IM_COL32(200, 240, 230, 255);
    ImU32 text_dim   = IM_COL32(120, 160, 150, 200);
    ImU32 separator  = IM_COL32(  0, 160, 130, 140);
    // Compass-matched orange palette
    ImU32 orange      = IM_COL32(255, 160,  32, 255);
    ImU32 orange_glow = IM_COL32(255, 160,  32,  70);
    ImU32 orange_dim  = IM_COL32(255, 160,  32,  28);
    // Runtime-configurable HUD palette
    ImU32 glow_base       = IM_COL32(255, 255, 255, 255);
    ImU32 glow_color      = IM_COL32(255, 255, 255, 255);
    ImU32 text_fill       = IM_COL32(255, 255, 255, 255);
    ImU32 ind_good        = IM_COL32(255, 160,  32, 255);
    ImU32 ind_inactive    = IM_COL32(120, 120, 120, 255);
    ImU32 ind_fail        = IM_COL32(255,  60,  60, 255);
    ImU32 compass_tick    = IM_COL32(255, 255, 255, 255);
    ImU32 compass_glow    = IM_COL32(255, 255, 255, 180);
    ImU32 compass_bg_color= IM_COL32(  8,  12,  18, 255);
};

struct HudConfig {
    int   compass_height        = 72;
    int   compass_bottom_margin = 20;
    int   compass_tick_length   = 24;
    bool  compass_tick_glow     = true;
    float compass_bg_opacity    = 0.75f;
    int   compass_bg_side_fade  = 80;
    int   panel_width           = 200;
    int   top_bar_height        = 52;
    float opacity               = 0.85f;
    float scale                 = 1.0f;
    float text_scale            = 1.0f;
    bool  glow_enabled          = true;
    float health_panel_opacity  = 0.71f;
    float pip_corner_clip_px    = 16.f;
    bool  indicator_bg_enabled  = true;
    float glow_intensity     = 1.0f;
    bool  hud_flip_vertical  = false;
};

class HudRenderer {
public:
    HudRenderer(const HudConfig& cfg, const HudColors& colors);
    ~HudRenderer();

    HudColors&       colors()       { return col_; }
    const HudColors& colors() const { return col_; }
    HudConfig&       config()       { return cfg_; }
    const HudConfig& config() const { return cfg_; }

    // Call after GL context is current.
    void load(void* glfw_window);
    void unload();

    // ── Per-frame API ─────────────────────────────────────────────────────────

    // Store dt, tick particle physics, update glow globals.
    // Call this first each frame before any HUD drawing.
    void set_dt(float dt);

    // NanoVG HUD chrome pass — compass, arms, indicators, particles.
    // show_fps=true also draws the FPS counter inline (avoids a second NVG frame).
    void draw_hud_frame(const AppState& snap, int w, int h, bool show_fps = false);

    // NanoVG FPS counter pass — small independent NVG frame for the fps text.
    // If draw_hud_frame was called with show_fps=true this is a no-op safe to skip.
    void draw_fps_overlay(const AppState& snap, int w, int h, bool active);

    // NanoVG toast pass — separate NVG frame; takes live queue for dismiss mutations.
    void draw_toasts(NotificationQueue& live_q, int w, int h);
    bool toast_has_focused() const { return toast_renderer_.has_focused_toast(); }
    void toast_navigate(int delta)  { toast_renderer_.navigate(delta); }
    void toast_select(AppState& s)  { toast_renderer_.select(s); }

    // Start an ImGui frame for menu + debug overlays.
    void begin_menu_frame();

    // Build PiP draw commands for a USB camera overlay (ImGui pass).
    void draw_pip(unsigned int tex, const char* label,
                  int w, int h, bool active, const OverlayConfig& cfg,
                  const CameraFocusState& focus = {},
                  bool nv_active = false);

    // Build Android mirror overlay (ImGui pass).
    void draw_android_overlay(unsigned int tex, int w, int h,
                              bool active, bool connecting, const OverlayConfig& cfg,
                              float frame_aspect = 9.f / 16.f);

    // Draw floating panel preview (ImGui pass).
    void draw_panel_preview(unsigned int tex, int screen_w, int screen_h,
                            float scale = 3.f);

    // Draw system status panel (ImGui pass).
    void draw_sys_panel(const AppState& snap, int w, int h, bool active);

    // Flush ImGui draw data to current GL framebuffer.
    void render_menu_overlay();

    // ── Popup API ─────────────────────────────────────────────────────────────

    bool popup_active() const;
    void popup_navigate(int delta);
    void popup_select();
    bool draw_popups(AppState& state, int w, int h);

private:
    // ── NanoVG helpers ────────────────────────────────────────────────────────
    void nvg_set_font_ui  (float size_override = 0.f);
    void nvg_set_font_mono(float size_override = 0.f);

    // ── NanoVG HUD draw methods ───────────────────────────────────────────────
    void draw_top_bar      (NVGcontext* vg, const AppState& s, float w, float bar_y = 0.f);
    void draw_health_side  (NVGcontext* vg, const SystemHealth& h,
                            float fw, float fh, bool right_side,
                            const CameraFocusState& focus_left,
                            const CameraFocusState& focus_right,
                            bool nv_enabled);
    void draw_audio_strip    (NVGcontext* vg, const AudioState& a, float ox, float oy, float w);
    void draw_face_indicator (NVGcontext* vg, const FaceState& f, float fw, float fh);
    void draw_lora_indicator (NVGcontext* vg, const AppState& s,  float fw, float fh);
    void draw_clock_indicator(NVGcontext* vg, const AppState& s,  float fw, float fh);
    void draw_lora_messages  (NVGcontext* vg, const AppState& s,
                            float ox, float oy, float pw, float ph);
    void draw_compass_tape (NVGcontext* vg, const AppState& s,
                            float ox, float oy, float tw, float th);
    void draw_timer_alarm_indicator(NVGcontext* vg, const AppState& s,
                            float fw, float fh);
    void draw_fps_nvg      (NVGcontext* vg, const AppState& snap, float fw, float fh);

    // ── ImGui popup draw methods (stay in ImGui pass) ─────────────────────────
    static const char* cardinal_str(float deg);
    void draw_alarm_popup (ImDrawList* dl, float fw, float fh);
    void draw_timer_popup (ImDrawList* dl, float fw, float fh,
                           const TimerAlarmState& ta);

    // ── Particle effects ──────────────────────────────────────────────────────
    struct Particle {
        float x, y;
        float vx, vy;
        float life;
        float life_total;
        ImU32 color;
        float size;
    };
    static constexpr int kMaxParticles = 256;
    Particle  particles_[kMaxParticles] = {};
    int       n_particles_ = 0;
    float     fx_prev_heading_ = 0.f;
    float     fx_pulse_phase_  = 1.f;

    struct LineParticle {
        float x, y;
        float vx, vy;
        float life;
        float life_total;
        float len;
        ImU32 color;
    };
    static constexpr int kMaxLineParticles = 64;
    LineParticle  line_particles_[kMaxLineParticles] = {};
    int           n_line_particles_ = 0;

    ImU32 fx_palette_color(const AppState& s) const;
    void  fx_tick(float dt);
    void  fx_tick_lines(float dt);
    void  fx_emit(float x, float y, float vx, float vy,
                  float life, float size, ImU32 color);
    void  fx_emit_line(float x, float y, float vx, float vy,
                       float life, float len, ImU32 color);
    void  fx_draw      (NVGcontext* vg) const;
    void  fx_draw_lines(NVGcontext* vg) const;
    void  fx_draw_nebula_cloud(NVGcontext* vg, float fw, float fh) const;
    void  fx_draw_alarm_pulse (NVGcontext* vg, const AppState& s, float fw, float fh);

    void  fx_emit_arm_glint    (float ax, float ay, float dx, float dy,
                                 float diag_len, ImU32 c, float dt);
    void  fx_emit_corner_drift (float cx, float cy, ImU32 c, float dt);
    void  fx_emit_burst        (float cx, float cy, int count, ImU32 c);
    void  fx_emit_turbulence   (float tape_cx, float tape_y,
                                 float tw, float th, ImU32 c, float dt);
    void  fx_emit_nebula_edge  (float fw, float fh, float dt);

    void  fx_update(NVGcontext* vg, const AppState& s,
                    float fw, float fh, float dt);

    ToastRenderer toast_renderer_;

    HudConfig     cfg_;
    HudColors     col_;
    ImGuiContext* ctx_ = nullptr;
    ImFont*       font_ui_    = nullptr;
    ImFont*       font_mono_  = nullptr;

    NVGcontext*   nvg_        = nullptr;
    int           nvg_font_ui_   = -1;
    int           nvg_font_mono_ = -1;

    float         frame_dt_  = 0.f;
    bool          fps_shown_in_hud_ = false;  // prevent double draw

    // ── Popup state ───────────────────────────────────────────────────────────
    enum class PopupKind   { None, Alarm, Timer };
    enum class PopupAction { None,
                             AlarmDismiss,
                             TimerDismiss, TimerAdd2, TimerAdd5, TimerAdd10 };

    PopupKind   popup_kind_    = PopupKind::None;
    int         popup_cursor_  = 0;
    PopupAction popup_pending_ = PopupAction::None;
    PopupKind   fx_prev_popup_ = PopupKind::None;
};

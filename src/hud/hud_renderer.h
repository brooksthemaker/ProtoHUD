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
#include "icon_cache.h"
#include <imgui.h>
#include <nanovg.h>
#include <string>
#include <deque>
#include <unordered_map>
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

    // Open a shared NVG frame that spans pip underlays + HUD chrome + toasts.
    // Call once before draw_pip_underlays; close with end_nvg_overlay() after
    // draw_toasts. Each draw_* method is a no-op Begin/End when a frame is active.
    void begin_nvg_overlay(int w, int h);
    void end_nvg_overlay();

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

    // Directory holding notification/widget icon PNGs (<dir>/<name>.png).
    void set_icon_dir(std::string dir) { icons_.set_dir(std::move(dir)); }

    // Start an ImGui frame for menu + debug overlays.
    void begin_menu_frame();

    // NanoVG underlay: draw corner-anchored PiP cameras BEFORE HUD chrome.
    // The anchor check is done internally; pass the full active state.
    // Call this before draw_hud_frame, after composite.
    void draw_pip_underlays(
        unsigned int tex1, bool act1, const OverlayConfig& c1,
        unsigned int tex2, bool act2, const OverlayConfig& c2,
        unsigned int tex3, bool act3, const OverlayConfig& c3,
        int ew, int eh);

    // NanoVG overlay: draw non-corner-anchored PiP cameras AFTER HUD chrome.
    // Identical visual style to draw_pip_underlays; call after draw_hud_frame.
    void draw_pip_overlays(
        unsigned int tex1, bool act1, const OverlayConfig& c1,
        unsigned int tex2, bool act2, const OverlayConfig& c2,
        unsigned int tex3, bool act3, const OverlayConfig& c3,
        int ew, int eh);

    // Legacy ImGui PiP stub — no-op; kept for call-site compatibility.
    void draw_pip(unsigned int tex, const char* label,
                  int w, int h, bool active, const OverlayConfig& cfg,
                  const CameraFocusState& focus = {},
                  bool nv_active = false);

    // Build Android mirror overlay (ImGui pass).
    void draw_android_overlay(unsigned int tex, int w, int h,
                              bool active, bool connecting, const OverlayConfig& cfg,
                              float frame_aspect = 9.f / 16.f);

    // Draw floating panel preview (ImGui pass). Positioned like the camera PiPs:
    // anchor_x/anchor_y are screen fractions (0=left/top .. 1=right/bottom),
    // pan_x/pan_y a pixel nudge, size_frac the image height as a fraction of the
    // screen height.
    // view: 0 = whole face (both panels), 1 = left half, 2 = right half.
    void draw_panel_preview(unsigned int tex, int tex_w, int tex_h,
                            int screen_w, int screen_h,
                            float anchor_x, float anchor_y,
                            float pan_x, float pan_y, float size_frac,
                            int view = 0);

    // Protoface "portrait" beside the minimap (ImGui — takes the LED GL texture).
    void draw_face_portrait(unsigned int tex, int tex_w, int tex_h,
                            bool tex_is_centred_face,
                            int screen_w, int screen_h,
                            const AppState& s);

    // Draw system status panel (ImGui pass).
    void draw_sys_panel(const AppState& snap, int w, int h, bool active,
                        float x_offset = 0.f, bool narrow = false);

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
    void draw_map_overlay  (NVGcontext* vg, const AppState& s, float fw, float fh);
    void draw_info_panel   (NVGcontext* vg, const AppState& s, float fw, float fh);
    void draw_compass_ring (NVGcontext* vg, const AppState& s,
                            float cx, float cy, float radius, bool bold = false);
    void draw_map_expanded (NVGcontext* vg, const AppState& s, float fw, float fh);
    void draw_expanded_sidebar(NVGcontext* vg, const AppState& s, float fw, float fh);
    // Shared NVG pip drawing (no NVG frame management — caller handles Begin/EndFrame).
    void draw_pip_nvg_single(NVGcontext* vg, unsigned int tex,
                              const OverlayConfig& cfg, float fw, float fh);

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
    void  fx_draw_vignette    (NVGcontext* vg, float fw, float fh) const;
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
    IconCache     icons_;

    // Map overlay image state (render-thread only)
    int         map_img_      = -1;
    int         map_img_w_    = 0;
    int         map_img_h_    = 0;
    std::string map_img_path_;
    float       gpu_load_smooth_ = 0.f;  // EMA of the GPU/render-load gauge

    // NVG image cache for PiP underlays: GL tex ID → NVG image handle.
    // Created via nvglCreateImageFromHandleGLES2 on first use; freed in unload().
    std::unordered_map<unsigned int, int> pip_nvg_cache_;

    HudConfig     cfg_;
    HudColors     col_;
    ImGuiContext* ctx_ = nullptr;
    ImFont*       font_ui_    = nullptr;
    ImFont*       font_mono_  = nullptr;

    NVGcontext*   nvg_        = nullptr;
    int           nvg_font_ui_   = -1;
    int           nvg_font_mono_ = -1;

    float         frame_dt_  = 0.f;
    bool          fps_shown_in_hud_   = false;  // prevent double draw
    bool          nvg_frame_active_   = false;  // shared overlay frame in progress

    // Info-panel cycler (render-thread only). info_cycle_idx_ indexes into the
    // currently-enabled widget list, rebuilt each frame; info_cycle_t_ accumulates
    // dwell time. Overlay is drawn once per frame, so ticking here is frame-rate-safe.
    int           info_cycle_idx_ = 0;
    float         info_cycle_t_   = 0.f;

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

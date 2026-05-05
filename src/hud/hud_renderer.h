#pragma once
// ── hud_renderer.h ────────────────────────────────────────────────────────────
// Dear ImGui-based HUD renderer.
//
// Rendering contract (called each frame inside the ImGui frame):
//
//   hud.begin_frame(dt);           // ImGui_ImplGlfw_NewFrame + ImGui::NewFrame
//     hud.draw_frame(snap, w, h);  // build HUD draw commands via DrawList
//     menu.draw(w, h);             // build menu draw commands via ImGui windows
//   hud.render_overlay();          // ImGui::Render + ImGui_ImplOpenGL3_RenderDrawData
//
// The camera draws happen inside eye FBOs before hud.render_overlay() so the
// HUD composites on top of the camera in the default framebuffer.

#include "../app_state.h"
#include <imgui.h>
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
    // Runtime-configurable HUD palette (full RGB, alpha=255; glow alphas derived at draw time)
    ImU32 glow_base       = IM_COL32(255, 255, 255, 255); // line/outline base color (Halo default)
    ImU32 glow_color      = IM_COL32(255, 255, 255, 255); // text glow halo color (Halo default)
    ImU32 text_fill       = IM_COL32(255, 255, 255, 255); // main text fill
    ImU32 ind_good        = IM_COL32(255, 160,  32, 255); // indicator OK dot
    ImU32 ind_inactive    = IM_COL32(120, 120, 120, 255); // indicator inactive/disconnected dot
    ImU32 ind_fail        = IM_COL32(255,  60,  60, 255); // indicator failure dot
    ImU32 compass_tick    = IM_COL32(255, 255, 255, 255); // compass major tick (Halo default)
    ImU32 compass_glow    = IM_COL32(255, 255, 255, 180); // compass glow (Halo default)
    ImU32 compass_bg_color= IM_COL32(  8,  12,  18, 255); // compass bg RGB (alpha from opacity cfg)
};

struct HudConfig {
    int   compass_height        = 72;
    int   compass_bottom_margin = 20;
    int   compass_tick_length   = 24;   // major tick height in px; minor ticks scaled proportionally
    bool  compass_tick_glow     = true; // when false, wide glow lines behind ticks are skipped
    float compass_bg_opacity    = 0.75f;
    int   compass_bg_side_fade  = 80;    // extra px on each side; also the fade zone width
    int   panel_width           = 200;
    int   top_bar_height        = 52;
    float opacity               = 0.85f;
    float scale                 = 1.0f;
    float text_scale            = 1.0f;  // FontGlobalScale, applied each frame; live-editable
    bool  glow_enabled          = true;  // when false, glow outline layers are skipped globally
    float health_panel_opacity  = 0.71f;
    float pip_corner_clip_px    = 16.f;
    bool  indicator_bg_enabled  = true;   // parallelogram bg behind health indicators
    // Glow intensity multiplier (scales all glow alphas)
    float glow_intensity     = 1.0f;   // 0.0=no glow, 1.0=full
    bool  hud_flip_vertical  = false;  // move compass+arms to top, status bar to bottom
};

class HudRenderer {
public:
    HudRenderer(const HudConfig& cfg, const HudColors& colors);
    ~HudRenderer();

    // Runtime access to palette and layout config for live menu editing.
    HudColors&       colors()       { return col_; }
    const HudColors& colors() const { return col_; }
    HudConfig&       config()       { return cfg_; }
    const HudConfig& config() const { return cfg_; }

    // Call after GL context is current. Sets up ImGui context and loads fonts.
    // glfw_window: the GLFW window ImGui should attach to for input bridging.
    void load(void* glfw_window);
    void unload();

    // ── Per-frame API ─────────────────────────────────────────────────────────

    // Start an ImGui frame; bridge GLFW keyboard + mouse state.
    void begin_frame(float dt);

    // Build all HUD draw commands (pure ImGui DrawList — no GL calls).
    void draw_frame(const AppState& snap, int w, int h);

    // Build PiP draw commands for a single USB camera overlay (16:9 aspect).
    // tex: raw GLuint texture ID (0 = no image, shows placeholder).
    // label: short name shown in the overlay corner.
    // cfg controls anchor position and size as a fraction of screen height.
    void draw_pip(unsigned int tex, const char* label,
                  int w, int h, bool active, const OverlayConfig& cfg,
                  const CameraFocusState& focus = {},
                  bool nv_active = false);

    // Build Android mirror overlay (portrait 9:16 aspect).
    // tex: GLuint from AndroidMirror::get_frame() — 0 shows a status placeholder.
    // connecting: scrcpy is running but no frame has arrived yet.
    // cfg controls anchor position and size as a fraction of screen height.
    void draw_android_overlay(unsigned int tex, int w, int h,
                              bool active, bool connecting, const OverlayConfig& cfg,
                              float frame_aspect = 9.f / 16.f);

    // Draw a floating panel preview window showing the 64×32 HUB75 LED face
    // at *scale* times its native resolution with nearest-neighbour filtering.
    // tex: GLuint from ProtoFaceController::get_frame_texture() — 0 = hidden.
    // anchor: top-left corner of the window in screen pixels.
    void draw_panel_preview(unsigned int tex, int screen_w, int screen_h,
                            float scale = 3.f);

    // Execute ImGui::Render → flush to current GL framebuffer.
    void render_overlay();

    // ── Popup API (alarm / timer-expired overlays) ────────────────────────────

    // True when an alarm or timer-expired popup is currently visible.
    bool popup_active() const;

    // Move the popup button cursor (delta = +1 / -1 from knob rotate).
    void popup_navigate(int delta);

    // Activate the currently highlighted popup button.
    // Action is deferred and executed the next time draw_popups() is called.
    void popup_select();

    // Draw alarm / timer-expired popups and execute any pending button action.
    // Must be called inside an active ImGui frame, after draw_frame() and
    // menu.draw(), so the popup renders on top of both.
    // Returns true if a popup was drawn this frame.
    bool draw_popups(AppState& state, int w, int h);

private:
    void draw_top_bar      (ImDrawList* dl, const AppState& s, float w, float bar_y = 0.f);
    void draw_health_side  (ImDrawList* dl, const SystemHealth& h,
                            float fw, float fh, bool right_side,
                            const CameraFocusState& focus_left,
                            const CameraFocusState& focus_right,
                            bool nv_enabled);
    void draw_audio_strip    (ImDrawList* dl, const AudioState& a, ImVec2 origin, float w);
    void draw_face_indicator (ImDrawList* dl, const FaceState& f, float fw, float fh);
    void draw_lora_indicator (ImDrawList* dl, const AppState& s,  float fw, float fh);
    void draw_clock_indicator(ImDrawList* dl, const AppState& s,  float fw, float fh);
    void draw_lora_messages  (ImDrawList* dl, const AppState& s,
                            ImVec2 origin, float pw, float ph);
    void draw_compass_tape (ImDrawList* dl, const AppState& s,
                            ImVec2 origin, float tw, float th);
    void draw_timer_alarm_indicator(ImDrawList* dl, const AppState& s,
                            float fw, float fh);

    static const char* cardinal_str(float deg);

    // Popup drawing helpers — called from draw_popups().
    void draw_alarm_popup (ImDrawList* dl, float fw, float fh);
    void draw_timer_popup (ImDrawList* dl, float fw, float fh,
                           const TimerAlarmState& ta);

    // ── Particle effects (render-thread only) ─────────────────────────────────
    struct Particle {
        float x, y;          // position
        float vx, vy;        // velocity (px/s)
        float life;          // remaining life (seconds)
        float life_total;    // initial life (for alpha fade)
        ImU32 color;         // base color (alpha overridden at draw)
        float size;          // radius in pixels
    };
    static constexpr int kMaxParticles = 256;
    Particle  particles_[kMaxParticles] = {};
    int       n_particles_ = 0;
    float     fx_prev_heading_ = 0.f;   // for turbulence heading delta

    // Resolve palette → color given current snap
    ImU32 fx_palette_color(const AppState& s) const;

    // Low-level pool management
    void  fx_tick(float dt);
    void  fx_emit(float x, float y, float vx, float vy,
                  float life, float size, ImU32 color);
    void  fx_draw(ImDrawList* dl) const;

    // Per-effect emitters (called from fx_update each frame)
    void  fx_emit_arm_glint    (float ax, float ay, float dx, float dy,
                                 float diag_len, ImU32 c, float dt);
    void  fx_emit_corner_drift (float cx, float cy, ImU32 c, float dt);
    void  fx_emit_burst        (float cx, float cy, int count, ImU32 c);
    void  fx_emit_turbulence   (float tape_cx, float tape_y,
                                 float tw, float th, ImU32 c, float dt);

    // Master dispatcher — called at end of draw_frame
    void  fx_update(ImDrawList* dl, const AppState& s,
                    float fw, float fh, float dt);

    HudConfig     cfg_;
    HudColors     col_;
    ImGuiContext* ctx_ = nullptr;
    ImFont*       font_ui_   = nullptr;
    ImFont*       font_mono_ = nullptr;
    float         frame_dt_  = 0.f;  // set each begin_frame, used by fx_update

    // ── Popup state (render-thread only, no mutex needed) ─────────────────────
    enum class PopupKind   { None, Alarm, Timer };
    enum class PopupAction { None,
                             AlarmDismiss,
                             TimerDismiss, TimerAdd2, TimerAdd5, TimerAdd10 };

    PopupKind   popup_kind_    = PopupKind::None;
    int         popup_cursor_  = 0;
    PopupAction popup_pending_ = PopupAction::None;
    PopupKind   fx_prev_popup_ = PopupKind::None;  // burst trigger on popup open
};

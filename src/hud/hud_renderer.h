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
};

struct HudConfig {
    int   compass_height        = 72;
    int   compass_bottom_margin = 20;
    float compass_bg_opacity    = 0.75f;
    int   compass_bg_side_fade  = 80;    // extra px on each side; also the fade zone width
    int   panel_width           = 200;
    int   top_bar_height        = 52;
    float opacity               = 0.85f;
    float scale                 = 1.0f;
    float health_panel_opacity  = 0.71f;
    float pip_corner_clip_px    = 16.f;
};

class HudRenderer {
public:
    HudRenderer(const HudConfig& cfg, const HudColors& colors);
    ~HudRenderer();

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
                  int w, int h, bool active, const OverlayConfig& cfg);

    // Build Android mirror overlay (portrait 9:16 aspect).
    // tex: GLuint from AndroidMirror::get_frame() — 0 shows a status placeholder.
    // connecting: scrcpy is running but no frame has arrived yet.
    // cfg controls anchor position and size as a fraction of screen height.
    void draw_android_overlay(unsigned int tex, int w, int h,
                              bool active, bool connecting, const OverlayConfig& cfg);

    // Execute ImGui::Render → flush to current GL framebuffer.
    void render_overlay();

private:
    void draw_top_bar      (ImDrawList* dl, const AppState& s, float w);
    void draw_health_dots  (ImDrawList* dl, const SystemHealth& h, ImVec2 origin);
    void draw_audio_strip  (ImDrawList* dl, const AudioState& a, ImVec2 origin, float w);
    void draw_face_panel   (ImDrawList* dl, const FaceState& f,
                            ImVec2 origin, float pw, float ph);
    void draw_lora_panel   (ImDrawList* dl, const AppState& s,
                            ImVec2 origin, float pw, float ph);
    void draw_lora_messages(ImDrawList* dl, const AppState& s,
                            ImVec2 origin, float pw, float ph);
    void draw_compass_tape (ImDrawList* dl, const AppState& s,
                            ImVec2 origin, float tw, float th);

    static const char* cardinal_str(float deg);

    HudConfig     cfg_;
    HudColors     col_;
    ImGuiContext* ctx_ = nullptr;
    ImFont*       font_ui_   = nullptr;
    ImFont*       font_mono_ = nullptr;
};

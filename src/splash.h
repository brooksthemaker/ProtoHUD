#pragma once
#include <imgui.h>
#include <string>

struct SplashConfig {
    bool        enabled       = true;
    bool        animated      = true;
    std::string image_path;           // PNG path; empty = procedural hexagon mark
    float       logo_size_px  = 104.f;// Longest side of the logo in pixels
    bool        show_ring     = true; // Hide the spinning dot ring (useful for large logos)
    float       min_display_s = 2.0f;
    std::string title         = "PROTOHUD";
    std::string subtitle      = "XR HUD System";
};

// Renders a full-screen splash into an ImGui background DrawList.
// Draws into both eye halves (left 0..sw/2, right sw/2..sw).
// load_image() must be called after the GL context is active.
class SplashScreen {
public:
    explicit SplashScreen(const SplashConfig& cfg);
    ~SplashScreen();

    // Load logo PNG from path; returns false and falls back to procedural mark.
    bool load_image(const std::string& path);

    // Render one splash frame.  Call between hud.begin_frame() and hud.render_overlay().
    // t: seconds elapsed since splash started (drives animation).
    // status: short status string shown below the ring.
    // progress: 0.0–1.0 for progress bar fill.
    // font: optional font for title/body text (nullptr = ImGui default).
    void draw(ImDrawList* dl, float sw, float sh, float t,
              const char* status, float progress,
              ImFont* font = nullptr) const;

private:
    void draw_eye(ImDrawList* dl,
                  float eye_x, float eye_y, float eye_w, float eye_h,
                  float t, const char* status, float progress,
                  ImFont* font) const;

    SplashConfig  cfg_;
    unsigned int  logo_tex_ = 0;
    int           logo_w_   = 0;
    int           logo_h_   = 0;
};

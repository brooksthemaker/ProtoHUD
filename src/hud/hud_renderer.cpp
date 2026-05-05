#include "hud_renderer.h"
#include "../serial/shm_frame_reader.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <cmath>
#include <ctime>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

// Effect name table (matches Prototracer effect IDs)
static const char* EFFECT_NAMES[] = {
    "Idle","Blink","Angry","Happy","Sad",
    "Shocked","Rainbow","Pulse","Wave","Custom"
};
static const char* effect_name(uint8_t id) {
    return (id < 10) ? EFFECT_NAMES[id] : "Unknown";
}

static std::string fmt_time(time_t t) {
    struct tm* tm = localtime(&t);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", tm);
    return buf;
}

static std::string fmt_clock(bool h24, bool seconds) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    const char* fmt = h24 ? (seconds ? "%H:%M:%S" : "%H:%M")
                           : (seconds ? "%I:%M:%S %p" : "%I:%M %p");
    strftime(buf, sizeof(buf), fmt, t);
    return buf;
}

static std::string fmt_date() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%a %b %d", t);
    return buf;
}

static std::string fmt_countdown(time_t end) {
    int remaining = static_cast<int>(end - time(nullptr));
    if (remaining < 0) remaining = 0;
    int mm = remaining / 60, ss = remaining % 60;
    char buf[16]; snprintf(buf, sizeof(buf), "%02d:%02d", mm, ss);
    return buf;
}

// Replace the alpha byte of an ImU32 color (format: ABGR, alpha in high byte).
static ImU32 with_alpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24u);
}

// Frame-scope glow state — set each frame from HudConfig / HudColors.
static bool  s_glow            = true;
static float s_glow_intensity  = 1.0f;
static ImU32 s_glow_color_base = IM_COL32(255, 160, 32, 255); // text glow halo color

// Draw text with an orange glow outline matching the compass tick style.
// selected=true → full white + bright glow; false → dim white + faint glow.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           bool selected = true) {
    const ImU32 FILL_ON  = IM_COL32(255, 255, 255, 255);
    const ImU32 FILL_OFF = IM_COL32(255, 255, 255, 160);
    const ImU32 fill = selected ? FILL_ON  : FILL_OFF;
    if (s_glow && s_glow_intensity > 0.f) {
        const uint8_t ga = static_cast<uint8_t>((selected ? 72 : 22) * s_glow_intensity);
        const ImU32 glow = with_alpha(s_glow_color_base, ga);
        constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
        for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    }
    dl->AddText(pos, fill, text);
}

// Color-parameterized variant: glow_col and fill_col are full-brightness (alpha=255);
// glow alphas are derived internally and scaled by s_glow_intensity.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           bool selected, ImU32 glow_col, ImU32 fill_col) {
    const ImU32 fill = selected ? fill_col : with_alpha(fill_col, 160);
    if (s_glow && s_glow_intensity > 0.f) {
        const uint8_t ga = static_cast<uint8_t>((selected ? 72 : 22) * s_glow_intensity);
        const ImU32 glow = selected ? with_alpha(glow_col, ga) : with_alpha(glow_col, ga);
        constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
        for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    }
    dl->AddText(pos, fill, text);
}

// Font-size-explicit overload for scaled text (e.g., clock arm).
// Respects s_glow and uses caller-supplied palette colors.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           ImFont* font, float font_size,
                           ImU32 glow_col, ImU32 fill_col) {
    if (s_glow) {
        const ImU32 glow = with_alpha(glow_col, 72);
        constexpr int D[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
        for (auto& o : D)
            dl->AddText(font, font_size, {pos.x + o[0], pos.y + o[1]}, glow, text);
    }
    dl->AddText(font, font_size, pos, fill_col, text);
}

// ── Construction ──────────────────────────────────────────────────────────────

HudRenderer::HudRenderer(const HudConfig& cfg, const HudColors& colors)
    : cfg_(cfg), col_(colors) {}

HudRenderer::~HudRenderer() { unload(); }

// ── load / unload ─────────────────────────────────────────────────────────────

void HudRenderer::load(void* glfw_window) {
    ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx_);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename  = nullptr;   // no imgui.ini on embedded device

    // Dark style base, then override for the teal HUD palette
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.f;
    style.FrameRounding     = 3.f;
    style.WindowBorderSize  = 1.f;
    style.Alpha             = cfg_.opacity;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.04f, 0.06f, 0.07f, 0.88f);
    c[ImGuiCol_Border]          = ImVec4(0.00f, 0.63f, 0.51f, 0.55f);
    c[ImGuiCol_Header]          = ImVec4(0.00f, 0.55f, 0.43f, 0.70f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.00f, 0.70f, 0.55f, 0.80f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.00f, 0.86f, 0.70f, 0.90f);

    ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(glfw_window), true);
    ImGui_ImplOpenGL3_Init("#version 100");

    // Load a small UI font; fall back to default if not found
    ImFontConfig fc;
    fc.OversampleH = 2; fc.OversampleV = 2;
    font_ui_ = io.Fonts->AddFontFromFileTTF(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        16.f * cfg_.scale, &fc);
    if (!font_ui_) font_ui_ = io.FontDefault;

    font_mono_ = io.Fonts->AddFontFromFileTTF(
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        14.f * cfg_.scale, &fc);
    if (!font_mono_) font_mono_ = io.FontDefault;

    ImGui_ImplOpenGL3_CreateFontsTexture();
}

void HudRenderer::unload() {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(ctx_);
    ctx_ = nullptr;
}

// ── Per-frame ─────────────────────────────────────────────────────────────────

void HudRenderer::begin_frame(float dt) {
    frame_dt_ = dt;
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().FontGlobalScale = cfg_.text_scale;
    s_glow            = cfg_.glow_enabled;
    s_glow_intensity  = cfg_.glow_intensity;
    s_glow_color_base = col_.glow_color;
    fx_tick(dt);
}

void HudRenderer::render_overlay() {
    ImGui::SetCurrentContext(ctx_);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ── draw_frame ────────────────────────────────────────────────────────────────

void HudRenderer::draw_frame(const AppState& s, int w, int h) {
    // Use the background draw list so elements appear behind any ImGui windows
    ImGui::SetCurrentContext(ctx_);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    const float fw       = static_cast<float>(w);
    const float fh       = static_cast<float>(h);
    const float th       = static_cast<float>(cfg_.top_bar_height);
    const float ch       = static_cast<float>(cfg_.compass_height);
    const float mid_h    = fh - th;
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip     = cfg_.hud_flip_vertical;

    const float bar_y    = flip ? fh - th : 0.f;
    draw_top_bar(dl, s, fw, bar_y);

    if (!s.lora_messages.empty()) {
        float pw    = static_cast<float>(cfg_.panel_width);
        float msg_w = std::min(pw, fw / 3.f);
        // Flipped: messages appear just below the compass at top; normal: just below status bar.
        float msg_y = flip ? (c_margin + ch) : th;
        draw_lora_messages(dl, s, { 0.f, msg_y }, msg_w, mid_h);
    }

    const float cw        = fw / 3.f;
    const float compass_y = flip ? c_margin : fh - ch - c_margin;
    // Compass drawn first; indicator arms and health sides render on top.
    draw_compass_tape    (dl, s, {fw / 2.f - cw / 2.f, compass_y}, cw, ch);
    // Health sides drawn first so their background sits behind arm content.
    draw_health_side(dl, s.health, fw, fh, false,
                     s.focus_left, s.focus_right, s.night_vision.nv_enabled);
    draw_health_side(dl, s.health, fw, fh, true,
                     s.focus_left, s.focus_right, s.night_vision.nv_enabled);
    // Arm indicators drawn on top of the health-side background.
    draw_face_indicator         (dl, s.face, fw, fh);
    draw_clock_indicator        (dl, s,      fw, fh);
    draw_timer_alarm_indicator  (dl, s,      fw, fh);

    // Particle effects drawn on top of all HUD chrome.
    fx_update(dl, s, fw, fh, frame_dt_);
}

// ── Shared overlay layout helper ──────────────────────────────────────────────

// Returns the top-left pixel origin for an overlay box of size (ov_w, ov_h)
// given the chosen anchor. bottom_margin is extra space reserved at the bottom
// edge (e.g. compass tape height).
static ImVec2 overlay_origin(const OverlayConfig& cfg,
                              float sw, float sh,
                              float ov_w, float ov_h,
                              float bottom_margin) {
    constexpr float kEdge = 20.f;
    const float bottom_y = sh - ov_h - bottom_margin - kEdge;
    switch (cfg.anchor) {
        case OverlayConfig::Anchor::TOP_LEFT:      return { kEdge,                  kEdge    };
        case OverlayConfig::Anchor::TOP_CENTER:    return { (sw - ov_w) * 0.5f,     kEdge + bottom_margin };
        case OverlayConfig::Anchor::TOP_RIGHT:     return { sw - ov_w - kEdge,      kEdge    };
        case OverlayConfig::Anchor::BOTTOM_LEFT:   return { kEdge,                  bottom_y };
        case OverlayConfig::Anchor::BOTTOM_CENTER: return { (sw - ov_w) * 0.5f,     bottom_y };
        case OverlayConfig::Anchor::BOTTOM_RIGHT:  return { sw - ov_w - kEdge,      bottom_y };
    }
    return { kEdge, kEdge }; // unreachable
}

// ── PiP ──────────────────────────────────────────────────────────────────────

void HudRenderer::draw_pip(unsigned int tex, const char* label,
                            int w, int h, bool active, const OverlayConfig& cfg,
                            const CameraFocusState& focus, bool nv_active) {
    if (!active) return;

    ImGui::SetCurrentContext(ctx_);

    const float sw     = static_cast<float>(w);
    const float sh     = static_cast<float>(h);
    const float ov_h   = sh * cfg.size;
    const float ov_w   = ov_h * (16.f / 9.f);
    const float margin = static_cast<float>(cfg_.compass_height);
    const auto  pos    = overlay_origin(cfg, sw, sh, ov_w, ov_h, margin);

    // Passthrough window so the menu (drawn after) can appear on top via BringToDisplayFront
    char win_id[32]; snprintf(win_id, sizeof(win_id), "##pip_%s", label);
    ImGui::SetNextWindowPos ({0.f, 0.f});
    ImGui::SetNextWindowSize({sw,  sh });
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin(win_id, nullptr,
        ImGuiWindowFlags_NoDecoration         |
        ImGuiWindowFlags_NoInputs             |
        ImGuiWindowFlags_NoMove               |
        ImGuiWindowFlags_NoNav                |
        ImGuiWindowFlags_NoBringToFrontOnFocus|
        ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Build anchor-dependent chamfered polygon
    const float C = cfg_.pip_corner_clip_px;
    const float x = pos.x, y = pos.y, bw = ov_w, bh = ov_h;
    ImVec2 pts[8];
    int    n_pts;

    using A = OverlayConfig::Anchor;
    switch (cfg.anchor) {
        case A::TOP_LEFT:
        case A::BOTTOM_RIGHT:
            // clip TL + BR
            pts[0] = {x+C,    y   }; pts[1] = {x+bw,   y   };
            pts[2] = {x+bw,   y+bh-C}; pts[3] = {x+bw-C, y+bh};
            pts[4] = {x,      y+bh}; pts[5] = {x,      y+C };
            n_pts = 6;
            break;
        case A::TOP_RIGHT:
        case A::BOTTOM_LEFT:
            // clip TR + BL
            pts[0] = {x,      y   }; pts[1] = {x+bw-C, y   };
            pts[2] = {x+bw,   y+C }; pts[3] = {x+bw,   y+bh};
            pts[4] = {x+C,    y+bh}; pts[5] = {x,      y+bh-C};
            n_pts = 6;
            break;
        default:
            // TOP_CENTER / BOTTOM_CENTER — all 4 corners
            pts[0] = {x+C,    y     }; pts[1] = {x+bw-C, y     };
            pts[2] = {x+bw,   y+C   }; pts[3] = {x+bw,   y+bh-C};
            pts[4] = {x+bw-C, y+bh  }; pts[5] = {x+C,    y+bh  };
            pts[6] = {x,      y+bh-C}; pts[7] = {x,      y+C   };
            n_pts = 8;
            break;
    }

    // 1. Background fill
    dl->AddConvexPolyFilled(pts, n_pts, col_.background);

    // 2. Camera image or "No Signal" placeholder
    if (tex) {
        dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                     {x, y}, {x + bw, y + bh});
    } else {
        if (font_mono_) ImGui::PushFont(font_mono_);
        dl->AddText({x + bw * 0.5f - 36.f, y + bh * 0.5f - 7.f},
                    col_.text_dim, "No Signal");
        if (font_mono_) ImGui::PopFont();
    }

    // 3. Mask image overflow at each clipped corner
    constexpr ImU32 mask = IM_COL32(0, 0, 0, 255);
    switch (cfg.anchor) {
        case A::TOP_LEFT:
        case A::BOTTOM_RIGHT:
            dl->AddTriangleFilled({x,      y   }, {x+C,    y   }, {x,    y+C   }, mask);
            dl->AddTriangleFilled({x+bw,   y+bh}, {x+bw-C, y+bh}, {x+bw, y+bh-C}, mask);
            break;
        case A::TOP_RIGHT:
        case A::BOTTOM_LEFT:
            dl->AddTriangleFilled({x+bw,   y   }, {x+bw-C, y   }, {x+bw, y+C   }, mask);
            dl->AddTriangleFilled({x,      y+bh}, {x+C,    y+bh}, {x,    y+bh-C}, mask);
            break;
        default:
            dl->AddTriangleFilled({x,      y   }, {x+C,    y   }, {x,    y+C   }, mask);
            dl->AddTriangleFilled({x+bw,   y   }, {x+bw-C, y   }, {x+bw, y+C   }, mask);
            dl->AddTriangleFilled({x,      y+bh}, {x+C,    y+bh}, {x,    y+bh-C}, mask);
            dl->AddTriangleFilled({x+bw,   y+bh}, {x+bw-C, y+bh}, {x+bw, y+bh-C}, mask);
            break;
    }

    // 4. Label (top-left)
    if (font_mono_) ImGui::PushFont(font_mono_);
    dl->AddText({x + 4.f, y + 4.f}, col_.primary, label);
    if (font_mono_) ImGui::PopFont();

    // 5. Focus mode + NV status strip (bottom-left)
    {
        const char* focus_str =
            (focus.mode == CameraFocusState::Mode::MANUAL) ? "MAN" :
            (focus.mode == CameraFocusState::Mode::SLAVE)  ? "SLV" :
            focus.af_locked ? "LOCK" :
            focus.af_active ? "SCAN" : "AF";
        const bool  af_on   = (focus.mode == CameraFocusState::Mode::AUTO);
        if (font_mono_) ImGui::PushFont(font_mono_);
        const float lh      = ImGui::GetTextLineHeight();
        const float sy      = y + bh - lh - 5.f;
        hud_glow_text(dl, {x + 6.f, sy}, focus_str, af_on,
                      col_.glow_base, col_.text_fill);
        if (nv_active) {
            const float off = ImGui::CalcTextSize(focus_str).x + 8.f;
            hud_glow_text(dl, {x + 6.f + off, sy}, "NV", true,
                          col_.glow_base, col_.text_fill);
        }
        if (font_mono_) ImGui::PopFont();
    }

    // 6. Chamfered border outline on top
    dl->AddPolyline(pts, n_pts, col_.glow_base, ImDrawFlags_Closed, 2.f);

    ImGui::End();
}

// ── Android mirror overlay ────────────────────────────────────────────────────

void HudRenderer::draw_android_overlay(unsigned int tex, int w, int h,
                                        bool active, bool connecting,
                                        const OverlayConfig& cfg,
                                        float frame_aspect) {
    if (!active) return;

    ImGui::SetCurrentContext(ctx_);

    const float sw     = static_cast<float>(w);
    const float sh     = static_cast<float>(h);
    const float ov_h   = sh * cfg.size;
    const float ov_w   = ov_h * frame_aspect;
    const float margin = static_cast<float>(cfg_.compass_height);
    const auto  pos    = overlay_origin(cfg, sw, sh, ov_w, ov_h, margin);

    ImGui::SetNextWindowPos ({0.f, 0.f});
    ImGui::SetNextWindowSize({sw,  sh });
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin("##android", nullptr,
        ImGuiWindowFlags_NoDecoration         |
        ImGuiWindowFlags_NoInputs             |
        ImGuiWindowFlags_NoMove               |
        ImGuiWindowFlags_NoNav                |
        ImGuiWindowFlags_NoBringToFrontOnFocus|
        ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Border
    dl->AddRect({ pos.x - 2.f, pos.y - 2.f },
                { pos.x + ov_w + 2.f, pos.y + ov_h + 2.f },
                col_.accent, 3.f);

    // Background fill
    dl->AddRectFilled({ pos.x, pos.y }, { pos.x + ov_w, pos.y + ov_h },
                      IM_COL32(10, 15, 20, 220));

    if (tex != 0) {
        dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                     { pos.x, pos.y }, { pos.x + ov_w, pos.y + ov_h });
    } else {
        const char* msg = connecting ? "Connecting..." : "No signal";
        if (font_ui_) ImGui::PushFont(font_ui_);
        dl->AddText({ pos.x + 8.f, pos.y + ov_h * 0.5f - 8.f }, col_.text_dim, msg);
        if (font_ui_) ImGui::PopFont();
    }

    // Label (top-left corner of the box, drawn after image so it's always visible)
    if (font_mono_) ImGui::PushFont(font_mono_);
    dl->AddText({ pos.x + 4.f, pos.y + 4.f }, col_.accent, "ANDROID");
    if (font_mono_) ImGui::PopFont();

    ImGui::End();
}

// ── Panel preview (FacePanel HUB75 live feed) ─────────────────────────────────

void HudRenderer::draw_panel_preview(unsigned int tex, int screen_w, int screen_h,
                                     float scale) {
    if (tex == 0) return;

    ImGui::SetCurrentContext(ctx_);

    // Native LED canvas dimensions (128×64 for 4-panel 2×2 layout).
    const float PW = static_cast<float>(ShmFrameReader::W);
    const float PH = static_cast<float>(ShmFrameReader::H);
    const float pw = PW * scale;
    const float ph = PH * scale;

    // Padding around the image inside the window.
    const float pad = 6.f;
    const float win_w = pw + pad * 2.f;
    const float win_h = ph + pad * 2.f;

    // Anchor: top-right with a small margin so it doesn't clip the screen edge.
    const float margin = 12.f;
    const float wx = static_cast<float>(screen_w) - win_w - margin;
    const float wy = margin;

    ImGui::SetNextWindowPos ({wx, wy}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({win_w, win_h}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin("##panel_preview", nullptr,
        ImGuiWindowFlags_NoDecoration          |
        ImGuiWindowFlags_NoInputs              |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoNav                 |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetWindowPos();

    // Dark backing rectangle
    dl->AddRectFilled({p.x, p.y}, {p.x + win_w, p.y + win_h},
                      IM_COL32(8, 12, 18, 220), 4.f);

    // The LED panel image — GL_NEAREST is set at texture creation time so
    // ImGui renders it crisp.
    const ImVec2 img0 = {p.x + pad, p.y + pad};
    const ImVec2 img1 = {p.x + pad + pw, p.y + pad + ph};
    dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                 img0, img1);

    // Accent border
    dl->AddRect({p.x, p.y}, {p.x + win_w, p.y + win_h},
                col_.accent, 4.f, 0, 1.5f);

    // Small "LED" label in the corner
    if (font_mono_) ImGui::PushFont(font_mono_);
    dl->AddText({p.x + 4.f, p.y + 4.f}, col_.accent, "LED");
    if (font_mono_) ImGui::PopFont();

    ImGui::End();
}

// ── Top bar ───────────────────────────────────────────────────────────────────

void HudRenderer::draw_top_bar(ImDrawList* dl, const AppState& s, float w, float bar_y) {
    float th = static_cast<float>(cfg_.top_bar_height);

    dl->AddRectFilled({0, bar_y}, {w, bar_y + th}, col_.background);

    // Clock (centered in bar) — uses AppState::ClockConfig
    {
        ImGui::PushFont(nullptr);
        float saved_scale = ImGui::GetIO().FontGlobalScale;
        const float cscale = std::max(0.5f, s.clock_cfg.font_scale) * 0.8f;
        ImGui::GetIO().FontGlobalScale = saved_scale * cscale;

        std::string time_str = fmt_clock(s.clock_cfg.use_24h, s.clock_cfg.show_seconds);
        ImVec2 tsz = ImGui::CalcTextSize(time_str.c_str());
        float cx = w * 0.5f - tsz.x * 0.5f;
        float cy = bar_y + th * 0.5f - tsz.y * 0.5f - (s.clock_cfg.show_date ? tsz.y * 0.5f : 0.f);
        hud_glow_text(dl, {cx, cy}, time_str.c_str(), true, col_.glow_base, col_.text_fill);

        // Second line: countdown timer or date
        if (s.timer_alarm.timer_active) {
            std::string cd = fmt_countdown(s.timer_alarm.timer_end);
            ImGui::GetIO().FontGlobalScale = saved_scale * cscale * 0.75f;
            ImVec2 csz = ImGui::CalcTextSize(cd.c_str());
            hud_glow_text(dl, {w * 0.5f - csz.x * 0.5f, cy + tsz.y + 1.f},
                          cd.c_str(), true, col_.warn, col_.warn);
        } else if (s.clock_cfg.show_date) {
            std::string date_str = fmt_date();
            ImGui::GetIO().FontGlobalScale = saved_scale * cscale * 0.75f;
            ImVec2 dsz = ImGui::CalcTextSize(date_str.c_str());
            hud_glow_text(dl, {w * 0.5f - dsz.x * 0.5f, cy + tsz.y + 1.f},
                          date_str.c_str(), false, col_.glow_base, col_.text_fill);
        }

        ImGui::GetIO().FontGlobalScale = saved_scale;
        ImGui::PopFont();
    }

    // Unread messages badge (left-of-center to avoid clock overlap)
    int unread = s.unread_message_count();
    if (unread > 0) {
        char buf[32]; snprintf(buf, sizeof(buf), "MSG:%d", unread);
        dl->AddText({w * 0.25f, bar_y + 10.f}, col_.warn, buf);
    }

    // Audio strip (right side)
    draw_audio_strip(dl, s.audio, {w - 180.f, bar_y + 4.f}, 170.f);
}

// ── Health side indicators ────────────────────────────────────────────────────

void HudRenderer::draw_health_side(ImDrawList* dl, const SystemHealth& h,
                                    float fw, float fh, bool right_side,
                                    const CameraFocusState& focus_left,
                                    const CameraFocusState& focus_right,
                                    bool nv_enabled) {
    const float tape_w   = fw / 3.f;
    const float tape_x   = fw / 2.f - tape_w / 2.f;
    const float fade_w   = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const float ch       = static_cast<float>(cfg_.compass_height);
    const bool  flip     = cfg_.hud_flip_vertical;
    const float anchor_y = flip ? c_margin : fh - c_margin;
    const float anchor_x = right_side ? tape_x + tape_w + fade_w
                                       : tape_x - fade_w;

    const ImU32 COL_MAJ  = col_.glow_base;
    const ImU32 COL_GLW1 = with_alpha(col_.glow_base, 70);
    const ImU32 COL_GLW2 = with_alpha(col_.glow_base, 28);

    auto focus_suffix = [](const CameraFocusState& f) -> const char* {
        if (f.mode == CameraFocusState::Mode::MANUAL) return " MAN";
        if (f.mode == CameraFocusState::Mode::SLAVE)  return " SLV";
        if (f.af_locked) return " LOCK";
        if (f.af_active) return " SCAN";
        return " AUTO";
    };
    char lcam_lbl[24], rcam_lbl[24];
    snprintf(lcam_lbl, sizeof(lcam_lbl), "L.Cam%s", focus_suffix(focus_left));
    snprintf(rcam_lbl, sizeof(rcam_lbl), "R.Cam%s", focus_suffix(focus_right));

    struct Ind { const char* label; bool ok; bool inactive = false; };
    const Ind left_items[]  = {{"Proot",     h.teensy_ok},
                                {"LoRa",      h.lora_ok},
                                {"Interface", h.knob_ok},
                                {"Audio",     h.audio_ok}};
    const Ind right_items[] = {{lcam_lbl,    h.cam_owl_left,  false},
                                {rcam_lbl,    h.cam_owl_right, false},
                                {"Cam 1",     h.cam_usb1,      h.cam_usb1 && !h.cam_usb1_overlay},
                                {"Cam 2",     h.cam_usb2,      h.cam_usb2 && !h.cam_usb2_overlay}};
    const Ind* items   = right_side ? right_items : left_items;
    const int  n_items = 4;

    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;
    constexpr float H_LEN   = 150.f;  // health horiz line (SEG_W * 2; SEG_W=75)
    constexpr float BG_FULL = 440.f;  // H_LEN + ARM_EXT*2 + SEG_W*2; covers health + LoRa + clock

    const float dir_x    = std::cos(ANGLE) * (right_side ? 1.f : -1.f);
    const float dir_y    = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float diag_len = static_cast<float>(n_items + 1) * ROW_H;

    // Parallelogram background — 16 strips fading from opaque (inner) to transparent (outer).
    // Extends BG_FULL px outward to cover both health indicators and the adjacent arm area.
    if (cfg_.indicator_bg_enabled) {
        const uint8_t bg_a       = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        const float   outer_sign = right_side ? 1.f : -1.f;
        constexpr int N = 16;
        for (int i = 0; i < N; i++) {
            const float t0 = float(i)     / float(N);
            const float t1 = float(i + 1) / float(N);
            constexpr float FADE_START = 0.9f;
            const float fade_t = (t0 < FADE_START) ? 0.f
                                 : (t0 - FADE_START) / (1.f - FADE_START);
            const uint8_t a0 = static_cast<uint8_t>(bg_a * (1.f - fade_t));
            ImVec2 strip[4] = {
                {anchor_x + outer_sign * t0 * BG_FULL,                    anchor_y},
                {anchor_x + outer_sign * t0 * BG_FULL + dir_x * diag_len, anchor_y + dir_y * diag_len},
                {anchor_x + outer_sign * t1 * BG_FULL + dir_x * diag_len, anchor_y + dir_y * diag_len},
                {anchor_x + outer_sign * t1 * BG_FULL,                    anchor_y},
            };
            dl->AddConvexPolyFilled(strip, 4, with_alpha(col_.compass_bg_color, a0));
        }
    }

    // Diagonal glow line
    const ImVec2 diag_end = {anchor_x + dir_x * diag_len, anchor_y + dir_y * diag_len};
    dl->AddLine({anchor_x, anchor_y}, diag_end, COL_GLW2, 5.f);
    dl->AddLine({anchor_x, anchor_y}, diag_end, COL_GLW1, 2.5f);
    dl->AddLine({anchor_x, anchor_y}, diag_end, COL_MAJ,  1.f);

    // Horizontal glow line — 300px outward from anchor
    const float h_end_x = anchor_x + (right_side ? H_LEN : -H_LEN);
    dl->AddLine({anchor_x, anchor_y}, {h_end_x, anchor_y}, COL_GLW2, 5.f);
    dl->AddLine({anchor_x, anchor_y}, {h_end_x, anchor_y}, COL_GLW1, 2.5f);
    dl->AddLine({anchor_x, anchor_y}, {h_end_x, anchor_y}, COL_MAJ,  1.f);

    // NV badge at end of horizontal line when night vision is active
    if (nv_enabled) {
        const float nv_x = right_side ? h_end_x + 6.f : h_end_x - ImGui::CalcTextSize("NV").x - 6.f;
        if (font_mono_) ImGui::PushFont(font_mono_);
        hud_glow_text(dl, {nv_x, anchor_y - 7.f}, "NV", true, col_.glow_base, col_.text_fill);
        if (font_mono_) ImGui::PopFont();
    }

    // Indicators placed along the diagonal, lifted 1 item above anchor
    if (font_mono_) ImGui::PushFont(font_mono_);
    for (int i = 0; i < n_items; ++i) {
        const float t  = static_cast<float>(i + 1) * ROW_H;
        const float ix = anchor_x + dir_x * t;
        const float iy = anchor_y + dir_y * t;

        if (items[i].inactive) {
            dl->AddCircleFilled({ix, iy}, DOT_R, col_.ind_inactive);
        } else if (items[i].ok) {
            dl->AddCircleFilled({ix, iy}, DOT_R + 2.f, with_alpha(col_.ind_good, 28));
            dl->AddCircleFilled({ix, iy}, DOT_R,        col_.ind_good);
        } else {
            dl->AddCircleFilled({ix, iy}, DOT_R, col_.ind_fail);
        }

        const char* lbl = items[i].label;
        const bool  bold = !cfg_.indicator_bg_enabled;
        if (right_side) {
            const ImVec2 lp = {ix + DOT_R + 6.f, iy - 7.f};
            if (bold) dl->AddText({lp.x + 0.7f, lp.y}, items[i].ok ? col_.text_fill : with_alpha(col_.text_fill, 160), lbl);
            hud_glow_text(dl, lp, lbl, items[i].ok, col_.text_fill, col_.text_fill);
        } else {
            float tw = ImGui::CalcTextSize(lbl).x;
            const ImVec2 lp = {ix - DOT_R - 6.f - tw, iy - 7.f};
            if (bold) dl->AddText({lp.x + 0.7f, lp.y}, items[i].ok ? col_.text_fill : with_alpha(col_.text_fill, 160), lbl);
            hud_glow_text(dl, lp, lbl, items[i].ok, col_.text_fill, col_.text_fill);
        }
    }
    if (font_mono_) ImGui::PopFont();
}

// ── Audio strip ───────────────────────────────────────────────────────────────

void HudRenderer::draw_audio_strip(ImDrawList* dl, const AudioState& a,
                                    ImVec2 origin, float w) {
    if (!a.enabled) {
        hud_glow_text(dl, origin, "AUDIO OFF", false);
        return;
    }

    char buf[64];
    static const char* outputs[] = {"VITURE","JACK","HDMI"};
    const char* out_str = (a.output >= 0 && a.output < 3) ? outputs[a.output] : "?";
    snprintf(buf, sizeof(buf), "AU \xe2\x86\x92 %s  X:%d", out_str, a.xrun_count);
    hud_glow_text(dl, origin, buf, a.device_ok);

    // CPU load bar
    float bar_y = origin.y + 20.f;
    dl->AddRectFilled({origin.x, bar_y}, {origin.x + w, bar_y + 6.f},
                       IM_COL32(20, 20, 20, 180));
    float load_w = w * std::min(1.f, a.cpu_load);
    ImU32 load_col = (a.cpu_load > 0.8f) ? col_.danger :
                     (a.cpu_load > 0.5f) ? col_.warn : col_.orange;
    dl->AddRectFilled({origin.x, bar_y}, {origin.x + load_w, bar_y + 6.f}, load_col);
}

// ── Face indicator arm (left side) ───────────────────────────────────────────
// Two parallel diagonal arms at 130°: [proto arm]  [health indicators]
// The health indicator diagonal itself is the visual divider between sections.
// SEG_W=75 puts proto at anchor_x-150 (= end of health side's 150px horiz line).

void HudRenderer::draw_face_indicator(ImDrawList* dl, const FaceState& f,
                                       float fw, float fh) {
    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float SEG_W   = 75.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;
    constexpr int   N_ITEMS = 6;

    const float tape_w        = fw / 3.f;
    const float tape_x        = fw / 2.f - tape_w / 2.f;
    const float fade_w        = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin      = static_cast<float>(cfg_.compass_bottom_margin);
    const float ch            = static_cast<float>(cfg_.compass_height);
    const bool  flip          = cfg_.hud_flip_vertical;
    const float anchor_y      = flip ? c_margin : fh - c_margin;
    const float ind_anchor_x  = tape_x - fade_w;
    const float proto_anchor_x = ind_anchor_x - SEG_W * 2.f;

    const float dir_x    = std::cos(ANGLE) * -1.f;
    const float dir_y    = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float diag_len = static_cast<float>(N_ITEMS + 1) * ROW_H;

    const ImU32 COL_MAJ  = col_.glow_base;
    const ImU32 COL_GLW1 = with_alpha(col_.glow_base, 70);
    const ImU32 COL_GLW2 = with_alpha(col_.glow_base, 28);

    // Proto arm: short horizontal extension leftward
    const float h_ext_x = proto_anchor_x - ARM_EXT;
    dl->AddLine({proto_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW2, 5.f);
    dl->AddLine({proto_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW1, 2.5f);
    dl->AddLine({proto_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_MAJ,  1.f);

    // Proto arm diagonal
    const ImVec2 proto_end = {proto_anchor_x + dir_x * diag_len,
                               anchor_y       + dir_y * diag_len};
    dl->AddLine({proto_anchor_x, anchor_y}, proto_end, COL_GLW2, 5.f);
    dl->AddLine({proto_anchor_x, anchor_y}, proto_end, COL_GLW1, 2.5f);
    dl->AddLine({proto_anchor_x, anchor_y}, proto_end, COL_MAJ,  1.f);

    // Build items
    char effect_lbl[24], mode_lbl[24], rgb_lbl[24], brt_lbl[24];
    snprintf(effect_lbl, sizeof(effect_lbl), "%s", effect_name(f.effect_id));
    if (f.playing_gif)
        snprintf(mode_lbl, sizeof(mode_lbl), "GIF #%d", f.gif_id);
    else
        snprintf(mode_lbl, sizeof(mode_lbl), "Pal #%d", f.palette_id);
    snprintf(rgb_lbl, sizeof(rgb_lbl), "R%d G%d B%d", f.r, f.g, f.b);
    snprintf(brt_lbl, sizeof(brt_lbl), "Brt %d%%", (f.brightness * 100) / 255);
    const char* ctrl_lbl = f.hud_control ? "HUD" : "AUTO";

    struct Ind { const char* label; bool ok; };
    const Ind items[N_ITEMS] = {
        {"FACE",      f.connected},
        {effect_lbl,  f.connected},
        {mode_lbl,    f.connected},
        {rgb_lbl,     f.connected},
        {brt_lbl,     f.connected},
        {ctrl_lbl,    f.connected},
    };

    if (font_mono_) ImGui::PushFont(font_mono_);
    for (int i = 0; i < N_ITEMS; ++i) {
        const float t  = static_cast<float>(i + 1) * ROW_H;
        const float ix = proto_anchor_x + dir_x * t;
        const float iy = anchor_y       + dir_y * t;

        if (items[i].ok) {
            dl->AddCircleFilled({ix, iy}, DOT_R + 2.f, with_alpha(col_.ind_good, 28));
            dl->AddCircleFilled({ix, iy}, DOT_R,        col_.ind_good);
        } else {
            dl->AddCircleFilled({ix, iy}, DOT_R, col_.ind_fail);
        }

        const char* lbl = items[i].label;
        float lbl_w = ImGui::CalcTextSize(lbl).x;
        hud_glow_text(dl, {ix - DOT_R - 6.f - lbl_w, iy - 7.f}, lbl, items[i].ok,
                      col_.text_fill, col_.text_fill);
    }
    if (font_mono_) ImGui::PopFont();
}

// ── LoRa indicator arm (right side) ──────────────────────────────────────────
// Two parallel diagonal arms at 130°: [health indicators]  [lora arm]
// The health indicator diagonal itself is the visual divider between sections.
// SEG_W=75 puts lora at anchor_x+150 (= end of health side's 150px horiz line).

void HudRenderer::draw_lora_indicator(ImDrawList* dl, const AppState& s,
                                       float fw, float fh) {
    constexpr float ROW_H    = 18.f;
    constexpr float DOT_R    = 4.f;
    constexpr float SEG_W    = 75.f;
    constexpr float ARM_EXT  = 140.f;
    constexpr float ANGLE    = 130.f * 3.14159265f / 180.f;
    constexpr int   MAX_ROWS = 4;   // 1 header + up to 3 nodes

    const float tape_w       = fw / 3.f;
    const float tape_x       = fw / 2.f - tape_w / 2.f;
    const float fade_w       = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin     = static_cast<float>(cfg_.compass_bottom_margin);
    const float ch           = static_cast<float>(cfg_.compass_height);
    const bool  flip         = cfg_.hud_flip_vertical;
    const float anchor_y     = flip ? c_margin : fh - c_margin;
    const float ind_anchor_x = tape_x + tape_w + fade_w;
    const float lora_anchor_x = ind_anchor_x + SEG_W * 2.f;

    const float dir_x    = std::cos(ANGLE) * 1.f;
    const float dir_y    = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float diag_len = static_cast<float>(MAX_ROWS + 1) * ROW_H;

    const ImU32 COL_MAJ  = col_.glow_base;
    const ImU32 COL_GLW1 = with_alpha(col_.glow_base, 70);
    const ImU32 COL_GLW2 = with_alpha(col_.glow_base, 28);

    // LoRa arm: short horizontal extension rightward
    const float h_ext_x = lora_anchor_x + ARM_EXT;
    dl->AddLine({lora_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW2, 5.f);
    dl->AddLine({lora_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW1, 2.5f);
    dl->AddLine({lora_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_MAJ,  1.f);

    // LoRa arm diagonal
    const ImVec2 lora_end = {lora_anchor_x + dir_x * diag_len,
                              anchor_y      + dir_y * diag_len};
    dl->AddLine({lora_anchor_x, anchor_y}, lora_end, COL_GLW2, 5.f);
    dl->AddLine({lora_anchor_x, anchor_y}, lora_end, COL_GLW1, 2.5f);
    dl->AddLine({lora_anchor_x, anchor_y}, lora_end, COL_MAJ,  1.f);

    // Build items: header row + one row per tracked node (capped at MAX_ROWS-1)
    struct Ind { char label[32]; bool ok; };
    Ind items[MAX_ROWS] = {};
    int n_items = 0;

    snprintf(items[n_items].label, sizeof(items[n_items].label), "LORA");
    items[n_items].ok = s.health.lora_ok;
    n_items++;

    time_t now = std::time(nullptr);
    for (const auto& node : s.lora_nodes) {
        if (n_items >= MAX_ROWS) break;
        double age = difftime(now, node.last_seen);
        const char* nm = node.name.empty() ? "???" : node.name.c_str();
        snprintf(items[n_items].label, sizeof(items[n_items].label),
                 "%-6.6s %03.0f\xc2\xb0 %.1fk",
                 nm, node.heading_deg, node.distance_m / 1000.f);
        items[n_items].ok = (node.last_seen > 0 && age < 120.0);
        n_items++;
    }

    if (font_mono_) ImGui::PushFont(font_mono_);
    for (int i = 0; i < n_items; ++i) {
        const float t  = static_cast<float>(i + 1) * ROW_H;
        const float ix = lora_anchor_x + dir_x * t;
        const float iy = anchor_y      + dir_y * t;

        if (items[i].ok) {
            dl->AddCircleFilled({ix, iy}, DOT_R + 2.f, with_alpha(col_.ind_good, 28));
            dl->AddCircleFilled({ix, iy}, DOT_R,        col_.ind_good);
        } else {
            dl->AddCircleFilled({ix, iy}, DOT_R, col_.ind_fail);
        }

        hud_glow_text(dl, {ix + DOT_R + 6.f, iy - 7.f}, items[i].label, items[i].ok,
                      col_.text_fill, col_.text_fill);
    }
    if (font_mono_) ImGui::PopFont();
}

// ── Clock arm (right side, outboard of LoRa arm) ─────────────────────────────
// Parallel diagonal arm at 130°. No dots — time and date rendered at font_scale.
// clock_anchor_x = lora_anchor_x + SEG_W*2 (another 150px right of LoRa anchor).

void HudRenderer::draw_clock_indicator(ImDrawList* dl, const AppState& s,
                                        float fw, float fh) {
    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;   // x-offset reference kept consistent with other arms
    constexpr float SEG_W   = 75.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;

    const float tape_w         = fw / 3.f;
    const float tape_x         = fw / 2.f - tape_w / 2.f;
    const float fade_w         = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin       = static_cast<float>(cfg_.compass_bottom_margin);
    const float ch             = static_cast<float>(cfg_.compass_height);
    const bool  flip           = cfg_.hud_flip_vertical;
    const float anchor_y       = flip ? c_margin : fh - c_margin;
    const float ind_anchor_x   = tape_x + tape_w + fade_w;
    const float clock_anchor_x = ind_anchor_x   + SEG_W * 2.f;

    const float dir_x = std::cos(ANGLE);
    const float dir_y = flip ? std::sin(ANGLE) : -std::sin(ANGLE);

    const float scale     = std::max(0.5f, s.clock_cfg.font_scale);
    const float eff_row_h = ROW_H * scale;
    const int   n_rows    = s.clock_cfg.show_date ? 2 : 1;
    const float diag_len  = static_cast<float>(n_rows + 1) * eff_row_h;

    const ImU32 COL_MAJ  = col_.glow_base;
    const ImU32 COL_GLW1 = with_alpha(col_.glow_base, 70);
    const ImU32 COL_GLW2 = with_alpha(col_.glow_base, 28);

    // Horizontal extension rightward
    const float h_ext_x = clock_anchor_x + ARM_EXT;
    dl->AddLine({clock_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW2, 5.f);
    dl->AddLine({clock_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW1, 2.5f);
    dl->AddLine({clock_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_MAJ,  1.f);

    // Diagonal
    const ImVec2 clk_end = {clock_anchor_x + dir_x * diag_len,
                              anchor_y      + dir_y * diag_len};
    dl->AddLine({clock_anchor_x, anchor_y}, clk_end, COL_GLW2, 5.f);
    dl->AddLine({clock_anchor_x, anchor_y}, clk_end, COL_GLW1, 2.5f);
    dl->AddLine({clock_anchor_x, anchor_y}, clk_end, COL_MAJ,  1.f);

    // Build time / date strings
    time_t now = std::time(nullptr) + static_cast<time_t>(s.clock_cfg.manual_offset_s);
    struct tm tm_buf = {};
    localtime_r(&now, &tm_buf);

    char time_str[24], date_str[24];
    if (s.clock_cfg.use_24h) {
        if (s.clock_cfg.show_seconds)
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        else
            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                     tm_buf.tm_hour, tm_buf.tm_min);
    } else {
        int h12 = tm_buf.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(time_str, sizeof(time_str), "%d:%02d %s",
                 h12, tm_buf.tm_min, tm_buf.tm_hour < 12 ? "AM" : "PM");
    }

    static const char* dow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(date_str, sizeof(date_str), "%s %d %s",
             dow[tm_buf.tm_wday], tm_buf.tm_mday, mon[tm_buf.tm_mon]);

    // Render rows using explicit font size — respects global glow flag + palette
    const float font_size = font_mono_ ? font_mono_->FontSize * scale
                                       : ImGui::GetFontSize() * scale;
    const char* rows[2] = { time_str, date_str };
    for (int i = 0; i < n_rows; ++i) {
        const float t  = static_cast<float>(i + 1) * eff_row_h;
        const float ix = clock_anchor_x + dir_x * t;
        const float iy = anchor_y       + dir_y * t;
        hud_glow_text(dl, {ix + DOT_R + 6.f, iy - font_size * 0.5f}, rows[i],
                      font_mono_, font_size, col_.glow_base, col_.text_fill);
    }
}

// ── Timer / Alarm indicator arm (right side, outboard of clock arm) ──────────
// Visible only when a timer or alarm is active.
// Timer row: countdown to expiry (MM:SS).  Alarm row: set time (HH:MM).

void HudRenderer::draw_timer_alarm_indicator(ImDrawList* dl, const AppState& s,
                                              float fw, float fh) {
    const auto& ta = s.timer_alarm;
    const bool  show_timer = ta.timer_active;
    const bool  show_alarm = ta.alarm_active;
    if (!show_timer && !show_alarm) return;

    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float SEG_W   = 75.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;

    const float tape_w    = fw / 3.f;
    const float tape_x    = fw / 2.f - tape_w / 2.f;
    const float fade_w    = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin  = static_cast<float>(cfg_.compass_bottom_margin);
    const float ch        = static_cast<float>(cfg_.compass_height);
    const bool  flip      = cfg_.hud_flip_vertical;
    const float anchor_y  = flip ? c_margin : fh - c_margin;
    // Outboard of clock arm (which is now at ind + SEG_W*2)
    const float ind_anchor_x = tape_x + tape_w + fade_w;
    const float ta_anchor_x  = ind_anchor_x + SEG_W * 4.f;

    const float dir_x = std::cos(ANGLE);
    const float dir_y = flip ? std::sin(ANGLE) : -std::sin(ANGLE);

    // Build display rows
    struct Row { char text[24]; ImU32 accent; };
    Row rows[2];
    int n_rows = 0;

    if (show_timer) {
        int remaining = static_cast<int>(ta.timer_end - time(nullptr));
        if (remaining < 0) remaining = 0;
        const int mm = remaining / 60, ss = remaining % 60;
        snprintf(rows[n_rows].text, sizeof(rows[0].text), "TMR %02d:%02d", mm, ss);
        // Orange when nearly done (< 60 s), else normal
        rows[n_rows].accent = (remaining < 60) ? col_.warn : col_.glow_base;
        ++n_rows;
    }
    if (show_alarm) {
        if (s.clock_cfg.use_24h) {
            snprintf(rows[n_rows].text, sizeof(rows[0].text),
                     "ALM %02d:%02d", ta.alarm_hour, ta.alarm_minute);
        } else {
            int h = ta.alarm_hour % 12;
            if (h == 0) h = 12;
            snprintf(rows[n_rows].text, sizeof(rows[0].text),
                     "ALM %d:%02d%s", h, ta.alarm_minute,
                     ta.alarm_hour < 12 ? "A" : "P");
        }
        rows[n_rows].accent = col_.glow_base;
        ++n_rows;
    }

    const float scale     = std::max(0.5f, s.clock_cfg.font_scale);
    const float eff_row_h = ROW_H * scale;
    const float diag_len  = static_cast<float>(n_rows + 1) * eff_row_h;
    const float font_size = font_mono_ ? font_mono_->FontSize * scale
                                       : ImGui::GetFontSize() * scale;

    const ImU32 COL_MAJ  = col_.glow_base;
    const ImU32 COL_GLW1 = with_alpha(col_.glow_base, 70);
    const ImU32 COL_GLW2 = with_alpha(col_.glow_base, 28);

    // Horizontal extension rightward
    const float h_ext_x = ta_anchor_x + ARM_EXT;
    dl->AddLine({ta_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW2, 5.f);
    dl->AddLine({ta_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_GLW1, 2.5f);
    dl->AddLine({ta_anchor_x, anchor_y}, {h_ext_x, anchor_y}, COL_MAJ,  1.f);

    // Diagonal
    const ImVec2 arm_end = {ta_anchor_x + dir_x * diag_len,
                             anchor_y    + dir_y * diag_len};
    dl->AddLine({ta_anchor_x, anchor_y}, arm_end, COL_GLW2, 5.f);
    dl->AddLine({ta_anchor_x, anchor_y}, arm_end, COL_GLW1, 2.5f);
    dl->AddLine({ta_anchor_x, anchor_y}, arm_end, COL_MAJ,  1.f);

    // Text rows
    if (font_mono_) ImGui::PushFont(font_mono_);
    for (int i = 0; i < n_rows; ++i) {
        const float t  = static_cast<float>(i + 1) * eff_row_h;
        const float ix = ta_anchor_x + dir_x * t;
        const float iy = anchor_y    + dir_y * t;
        hud_glow_text(dl, {ix + DOT_R + 6.f, iy - font_size * 0.5f},
                      rows[i].text, font_mono_, font_size,
                      rows[i].accent, col_.text_fill);
    }
    if (font_mono_) ImGui::PopFont();
}

// ── LoRa messages panel ───────────────────────────────────────────────────────

void HudRenderer::draw_lora_messages(ImDrawList* dl, const AppState& s,
                                      ImVec2 origin, float pw, float ph) {
    dl->AddRectFilled(origin, {origin.x + pw, origin.y + ph},
                      IM_COL32(10, 15, 20, 230));
    dl->AddLine({origin.x + pw, origin.y},
                {origin.x + pw, origin.y + ph}, col_.orange);

    if (font_ui_) ImGui::PushFont(font_ui_);
    hud_glow_text(dl, {origin.x + 8.f, origin.y + 6.f}, "MESSAGES");

    float py = origin.y + 28.f;
    for (const auto& msg : s.lora_messages) {
        if (py + 38.f > origin.y + ph) break;

        std::string sender;
        for (const auto& n : s.lora_nodes)
            if (n.local_id == msg.local_id) { sender = n.name; break; }
        if (sender.empty()) {
            char id[12]; snprintf(id, sizeof(id), "ID:%02X", msg.local_id);
            sender = id;
        }

        char hdr[64];
        snprintf(hdr, sizeof(hdr), "[%s  %s]",
                 sender.substr(0, 10).c_str(),
                 fmt_time(msg.timestamp).c_str());
        hud_glow_text(dl, {origin.x + 8.f, py}, hdr);
        py += 16.f;

        hud_glow_text(dl, {origin.x + 8.f, py}, msg.text.c_str(), !msg.read);
        py += 22.f;
    }
    if (font_ui_) ImGui::PopFont();
}

// ── Compass tape ──────────────────────────────────────────────────────────────

void HudRenderer::draw_compass_tape(ImDrawList* dl, const AppState& s,
                                     ImVec2 origin, float tw, float th) {
    const float heading  = s.compass_heading;
    const float ppd      = tw / 120.f; // 120° visible across full tape width
    const float center_x = origin.x + tw / 2.f;

    const ImU32 col_major = col_.compass_tick;
    const ImU32 col_mid   = with_alpha(col_.compass_tick, 180);
    const ImU32 col_minor = with_alpha(col_.compass_tick, 110);
    const ImU32 col_glow1 = with_alpha(col_.compass_glow, 70);
    const ImU32 col_glow2 = with_alpha(col_.compass_glow, 28);

    const float fade_w = static_cast<float>(cfg_.compass_bg_side_fade);
    const bool  flip   = cfg_.hud_flip_vertical;

    if (s.compass_bg_enabled) {
        const uint8_t a = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        const ImU32   A = with_alpha(col_.compass_bg_color, a);

        constexpr float kIndAngle = 130.f * 3.14159265f / 180.f;
        const float inset = std::abs(std::cos(kIndAngle)) / std::sin(kIndAngle) * th;

        // Trapezoid: narrower on the side that faces the indicator arms.
        // Normal: narrower at top (origin.y), wider at bottom (origin.y+th).
        // Flipped: narrower at bottom (origin.y+th), wider at top (origin.y).
        ImVec2 bg[4];
        if (!flip) {
            bg[0] = {origin.x - fade_w + inset,      origin.y     };  // TL (inset)
            bg[1] = {origin.x + tw + fade_w - inset, origin.y     };  // TR (inset)
            bg[2] = {origin.x + tw + fade_w,         origin.y + th};  // BR
            bg[3] = {origin.x - fade_w,              origin.y + th};  // BL
        } else {
            bg[0] = {origin.x - fade_w,              origin.y     };  // TL
            bg[1] = {origin.x + tw + fade_w,         origin.y     };  // TR
            bg[2] = {origin.x + tw + fade_w - inset, origin.y + th};  // BR (inset)
            bg[3] = {origin.x - fade_w + inset,      origin.y + th};  // BL (inset)
        }
        dl->AddConvexPolyFilled(bg, 4, A);
    }

    // Glow line — at tape edge that meets the indicator arms.
    {
        const float lx0    = origin.x - fade_w, lx1 = origin.x + tw + fade_w;
        const float line_y = flip ? origin.y : origin.y + th;
        dl->AddLine({lx0, line_y}, {lx1, line_y}, with_alpha(col_.glow_base, 28), 5.f);
        dl->AddLine({lx0, line_y}, {lx1, line_y}, with_alpha(col_.glow_base, 70), 2.5f);
        dl->AddLine({lx0, line_y}, {lx1, line_y}, col_.glow_base, 1.f);
    }

    // Tick lengths — major is user-configurable; mid and minor scale proportionally.
    const float t_maj = static_cast<float>(cfg_.compass_tick_length);
    const float t_mid = t_maj * (16.f / 24.f);
    const float t_min = t_maj * (10.f / 24.f);

    // Tick zone: 20 px label strip on the arm-facing edge; ticks grow inward.
    // Normal: label strip at bottom, ticks grow upward.
    // Flipped: label strip at top, ticks grow downward.
    const float tick_base   = flip ? origin.y + 20.f : origin.y + th - 20.f;
    const float tick_sign   = flip ? 1.f : -1.f;   // +1 = down, -1 = up
    const float label_y     = flip ? origin.y + 3.f : tick_base + 3.f;

    if (font_mono_) ImGui::PushFont(font_mono_);

    for (int deg = 0; deg < 360; deg++) {
        float offset = deg - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;

        float px = center_x + offset * ppd;
        if (px < origin.x || px > origin.x + tw) continue;

        const bool tick_glow = cfg_.compass_tick_glow;
        if (deg % 45 == 0) {
            if (tick_glow) {
                dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_maj}, col_glow2, t_maj * 0.5f);
                dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_maj}, col_glow1, t_maj * 0.25f);
            }
            dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_maj}, col_major, 3.f);
            const char* card = cardinal_str(static_cast<float>(deg));
            ImVec2 csz = ImGui::CalcTextSize(card);
            hud_glow_text(dl, {px - csz.x * 0.5f, label_y},
                          card, true, col_.glow_base, col_.text_fill);
        } else if (deg % 10 == 0) {
            if (tick_glow)
                dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_mid}, col_glow2, t_mid * 0.375f);
            dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_mid}, col_mid, 2.f);
            char buf[8]; snprintf(buf, sizeof(buf), "%d", deg);
            ImVec2 bsz = ImGui::CalcTextSize(buf);
            hud_glow_text(dl, {px - bsz.x * 0.5f, label_y}, buf,
                          true, col_.glow_base, col_.text_fill);
        } else if (deg % 5 == 0) {
            if (tick_glow)
                dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_min}, col_glow2, t_min * 0.4f);
            dl->AddLine({px, tick_base}, {px, tick_base + tick_sign * t_min}, col_minor, 2.f);
        }
    }

    if (font_mono_) ImGui::PopFont();

    // LoRa node bearing markers — small triangles on the inner (non-label) side of the ticks.
    // Normal: triangles above the tick tops (pointing down into tape).
    // Flipped: triangles below the tick bottoms (pointing up into tape).
    for (const auto& node : s.lora_nodes) {
        if (node.distance_m <= 0.f) continue;
        float offset = node.heading_deg - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;
        float px = center_x + offset * ppd;
        if (px < origin.x || px > origin.x + tw) continue;

        ImU32 node_col = s.lora_node_colors[node.local_id % 8];
        float mx = px;
        float my, tri_tip_dy;
        if (!flip) {
            my = tick_base + tick_sign * t_maj - 6.f;  // above tick tops
            tri_tip_dy = 9.f;                            // tip points downward
        } else {
            my = tick_base + tick_sign * t_maj + 6.f;  // below tick bottoms
            tri_tip_dy = -9.f;                           // tip points upward
        }
        ImVec2 tri[3] = {{mx - 5.f, my}, {mx + 5.f, my}, {mx, my + tri_tip_dy}};
        dl->AddConvexPolyFilled(tri, 3, node_col);
        dl->AddPolyline(tri, 3, with_alpha(node_col, 200), ImDrawFlags_Closed, 1.f);
    }
}

// ── Alarm / Timer popups ──────────────────────────────────────────────────────

// Helper: draw an 8-point chamfered shape.  pts_out must be ImVec2[8].
static void chamfer_pts(ImVec2 mn, ImVec2 mx, float c, ImVec2 pts[8]) {
    pts[0] = {mn.x + c, mn.y}; pts[1] = {mx.x - c, mn.y};
    pts[2] = {mx.x, mn.y + c}; pts[3] = {mx.x, mx.y - c};
    pts[4] = {mx.x - c, mx.y}; pts[5] = {mn.x + c, mx.y};
    pts[6] = {mn.x, mx.y - c}; pts[7] = {mn.x, mn.y + c};
}

// Draw one selectable button. Returns true if this button's slot == cursor.
static bool popup_button(ImDrawList* dl, ImVec2 pos, const char* label,
                          bool selected,
                          ImU32 sel_bg, ImU32 sel_text,
                          ImU32 def_bg, ImU32 def_text,
                          float pad_x = 14.f, float pad_y = 7.f) {
    ImVec2 tsz = ImGui::CalcTextSize(label);
    float  bw  = tsz.x + pad_x * 2.f;
    float  bh  = tsz.y + pad_y * 2.f;
    ImVec2 bmax = {pos.x + bw, pos.y + bh};
    ImU32  bg  = selected ? sel_bg   : def_bg;
    ImU32  fg  = selected ? sel_text : def_text;
    dl->AddRectFilled(pos, bmax, bg, 5.f);
    if (selected)
        dl->AddRect(pos, bmax, sel_text, 5.f, 0, 1.5f);
    dl->AddText({pos.x + pad_x, pos.y + pad_y}, fg, label);
    return selected;
}

void HudRenderer::draw_alarm_popup(ImDrawList* dl, float fw, float fh) {
    constexpr float W   = 380.f, H = 180.f;
    constexpr float C   = 8.f,  GAP = 3.f;
    const float x = fw * 0.5f - W * 0.5f;
    const float y = fh * 0.5f - H * 0.5f;

    // Light grey background (chamfered, inset by GAP)
    ImVec2 bg_pts[8];
    chamfer_pts({x + GAP, y + GAP}, {x + W - GAP, y + H - GAP}, C, bg_pts);
    dl->AddConvexPolyFilled(bg_pts, 8, IM_COL32(220, 220, 220, 248));

    // Red chamfered border at window edge
    ImVec2 bdr_pts[8];
    chamfer_pts({x, y}, {x + W, y + H}, C, bdr_pts);
    dl->AddPolyline(bdr_pts, 8, IM_COL32(210, 30, 30, 255), ImDrawFlags_Closed, 2.5f);

    // Title
    const char* title = "!! ALARM !!";
    ImVec2 tsz = ImGui::CalcTextSize(title);
    dl->AddText({x + W * 0.5f - tsz.x * 0.5f, y + 32.f},
                IM_COL32(190, 20, 20, 255), title);

    // Dismiss button (centered; always selected — only one button)
    float btn_y = y + H - 60.f;
    ImVec2 dsz  = ImGui::CalcTextSize("DISMISS");
    float  bw   = dsz.x + 28.f;
    float  bx   = x + W * 0.5f - bw * 0.5f;
    popup_button(dl, {bx, btn_y}, "DISMISS", true,
                 IM_COL32(200, 30, 30, 230), IM_COL32(255, 255, 255, 255),
                 IM_COL32(180, 180, 180, 200), IM_COL32(40, 40, 40, 255));
}

void HudRenderer::draw_timer_popup(ImDrawList* dl, float fw, float fh,
                                    const TimerAlarmState& /*ta*/) {
    constexpr float W   = 460.f, H = 190.f;
    constexpr float C   = 8.f,  GAP = 3.f;
    const float x = fw * 0.5f - W * 0.5f;
    const float y = fh * 0.5f - H * 0.5f;

    // Golden-orange colors
    constexpr ImU32 ORANGE_BORDER = IM_COL32(220, 140, 20, 255);
    constexpr ImU32 ORANGE_SEL    = IM_COL32(210, 130, 15, 230);

    // Light grey background
    ImVec2 bg_pts[8];
    chamfer_pts({x + GAP, y + GAP}, {x + W - GAP, y + H - GAP}, C, bg_pts);
    dl->AddConvexPolyFilled(bg_pts, 8, IM_COL32(220, 220, 220, 248));

    // Golden-orange chamfered border
    ImVec2 bdr_pts[8];
    chamfer_pts({x, y}, {x + W, y + H}, C, bdr_pts);
    dl->AddPolyline(bdr_pts, 8, ORANGE_BORDER, ImDrawFlags_Closed, 2.5f);

    // Title
    const char* title = "TIMER EXPIRED";
    ImVec2 tsz = ImGui::CalcTextSize(title);
    dl->AddText({x + W * 0.5f - tsz.x * 0.5f, y + 28.f},
                IM_COL32(160, 100, 10, 255), title);

    // Four buttons: Dismiss | +2 Min | +5 Min | +10 Min
    static const char* labels[4] = {"DISMISS", "+2 MIN", "+5 MIN", "+10 MIN"};
    float btn_y     = y + H - 64.f;
    float total_w   = 0.f;
    float btn_ws[4];
    for (int i = 0; i < 4; i++) {
        btn_ws[i] = ImGui::CalcTextSize(labels[i]).x + 28.f;
        total_w  += btn_ws[i];
    }
    float gap = (W - 24.f - total_w) / 3.f;
    float bx  = x + 12.f;
    for (int i = 0; i < 4; i++) {
        bool sel = (popup_cursor_ == i);
        popup_button(dl, {bx, btn_y}, labels[i], sel,
                     ORANGE_SEL, IM_COL32(255, 255, 255, 255),
                     IM_COL32(185, 185, 185, 210), IM_COL32(50, 50, 50, 255));
        bx += btn_ws[i] + (i < 3 ? gap : 0.f);
    }
}

bool HudRenderer::popup_active() const {
    return popup_kind_ != PopupKind::None;
}

void HudRenderer::popup_navigate(int delta) {
    if (popup_kind_ == PopupKind::Timer) {
        popup_cursor_ = (popup_cursor_ + delta + 4) % 4;
    }
    // Alarm has only one button; navigation is a no-op.
}

void HudRenderer::popup_select() {
    if (popup_kind_ == PopupKind::Alarm) {
        popup_pending_ = PopupAction::AlarmDismiss;
    } else if (popup_kind_ == PopupKind::Timer) {
        switch (popup_cursor_) {
            case 0:  popup_pending_ = PopupAction::TimerDismiss; break;
            case 1:  popup_pending_ = PopupAction::TimerAdd2;    break;
            case 2:  popup_pending_ = PopupAction::TimerAdd5;    break;
            default: popup_pending_ = PopupAction::TimerAdd10;   break;
        }
    }
}

bool HudRenderer::draw_popups(AppState& state, int w, int h) {
    ImGui::SetCurrentContext(ctx_);

    // Determine current popup kind (alarm takes priority)
    PopupKind kind = PopupKind::None;
    if (state.timer_alarm.alarm_triggered)   kind = PopupKind::Alarm;
    else if (state.timer_alarm.timer_triggered) kind = PopupKind::Timer;

    // Reset cursor when a new popup type appears
    if (kind != popup_kind_) {
        popup_cursor_ = 0;
        popup_kind_   = kind;
    }

    // Execute any pending button action
    if (popup_pending_ != PopupAction::None) {
        auto& ta = state.timer_alarm;
        switch (popup_pending_) {
            case PopupAction::AlarmDismiss:
                ta.alarm_triggered = false;
                break;
            case PopupAction::TimerDismiss:
                ta.timer_triggered = false;
                break;
            case PopupAction::TimerAdd2:
                ta.timer_end = time(nullptr) + 120;
                ta.timer_active    = true;
                ta.timer_triggered = false;
                break;
            case PopupAction::TimerAdd5:
                ta.timer_end = time(nullptr) + 300;
                ta.timer_active    = true;
                ta.timer_triggered = false;
                break;
            case PopupAction::TimerAdd10:
                ta.timer_end = time(nullptr) + 600;
                ta.timer_active    = true;
                ta.timer_triggered = false;
                break;
            default: break;
        }
        popup_pending_ = PopupAction::None;
        // Update kind immediately after dismissal
        if (!state.timer_alarm.alarm_triggered && !state.timer_alarm.timer_triggered) {
            popup_kind_ = PopupKind::None;
            return false;
        }
    }

    if (popup_kind_ == PopupKind::None) return false;

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    // Use a full-screen transparent ImGui window so popup is on top of the HUD.
    ImGui::SetNextWindowPos ({0.f, 0.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({fw,  fh},  ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.f, 0.f));
    ImGui::Begin("##popup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoInputs);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (popup_kind_ == PopupKind::Alarm)
        draw_alarm_popup(dl, fw, fh);
    else
        draw_timer_popup(dl, fw, fh, state.timer_alarm);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return true;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

const char* HudRenderer::cardinal_str(float deg) {
    static const char* pts[] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = static_cast<int>((deg + 22.5f) / 45.f) % 8;
    return pts[idx];
}

// ── Particle effects ──────────────────────────────────────────────────────────

// Palette colors for each EffectPalette option.
// "Theme" reads from the live HudColors glow_base; the rest are fixed.
static ImU32 kPaletteColors[5] = {
    IM_COL32(  0,   0,   0, 255),  // 0 = Theme — overridden at runtime
    IM_COL32(255, 255, 255, 255),  // 1 = Halo   (white)
    IM_COL32(255, 140,  20, 255),  // 2 = Solar  (amber orange)
    IM_COL32(  0, 210,  50, 255),  // 3 = Fallout (radioactive green)
    IM_COL32( 80, 100, 255, 255),  // 4 = Space  (electric blue)
};

ImU32 HudRenderer::fx_palette_color(const AppState& s) const {
    const int idx = static_cast<int>(s.effects_cfg.palette);
    if (idx == 0) return col_.glow_base;  // Theme: match current HUD palette
    if (idx >= 1 && idx <= 4) return kPaletteColors[idx];
    return col_.glow_base;
}

void HudRenderer::fx_tick(float dt) {
    if (dt <= 0.f || n_particles_ == 0) return;
    int i = 0;
    while (i < n_particles_) {
        Particle& p = particles_[i];
        p.life -= dt;
        if (p.life <= 0.f) {
            // Remove by swap-with-last
            particles_[i] = particles_[--n_particles_];
        } else {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            ++i;
        }
    }
}

void HudRenderer::fx_emit(float x, float y, float vx, float vy,
                           float life, float size, ImU32 color) {
    if (n_particles_ >= kMaxParticles) {
        // Replace the oldest particle (index 0 after swap churn is approximately oldest)
        particles_[0] = { x, y, vx, vy, life, life, color, size };
        return;
    }
    particles_[n_particles_++] = { x, y, vx, vy, life, life, color, size };
}

void HudRenderer::fx_draw(ImDrawList* dl) const {
    for (int i = 0; i < n_particles_; ++i) {
        const Particle& p = particles_[i];
        const float frac  = p.life / p.life_total;
        const uint8_t a   = static_cast<uint8_t>(frac * 220.f);
        const ImU32  col  = with_alpha(p.color, a);
        dl->AddCircleFilled({p.x, p.y}, p.size, col, 6);
    }
}

// Emit a handful of sparks at random positions along an indicator arm.
void HudRenderer::fx_emit_arm_glint(float ax, float ay, float dx, float dy,
                                     float diag_len, ImU32 c, float dt) {
    // ~3 glints per second per arm
    const float rate = 3.0f;
    const float prob = rate * dt;
    // Simple deterministic-ish per-frame: if prob > random threshold, emit
    static unsigned rng = 0x12345678u;
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;  // xorshift32
    const float r0 = static_cast<float>(rng & 0xFFFF) / 65535.f;
    if (r0 > prob) return;

    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    const float t  = static_cast<float>(rng & 0xFFFF) / 65535.f;   // position along arm
    const float px = ax + dx * t * diag_len;
    const float py = ay + dy * t * diag_len;

    // Velocity mostly along arm direction, slight spread
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    const float spread = ((rng & 0xFF) / 255.f - 0.5f) * 30.f;
    const float speed  = 18.f + (rng & 0x3F);
    const float vx = dx * speed + dy * spread;
    const float vy = dy * speed - dx * spread;

    fx_emit(px, py, vx, vy, 0.45f + (rng & 0x3F) * 0.004f, 2.0f, c);
}

// Emit drifting particles from a corner of the compass tape.
void HudRenderer::fx_emit_corner_drift(float cx, float cy, ImU32 c, float dt) {
    const float rate = 2.5f;
    const float prob = rate * dt;
    static unsigned rng2 = 0xABCDEF01u;
    rng2 ^= rng2 << 13; rng2 ^= rng2 >> 17; rng2 ^= rng2 << 5;
    if (static_cast<float>(rng2 & 0xFFFF) / 65535.f > prob) return;

    rng2 ^= rng2 << 13; rng2 ^= rng2 >> 17; rng2 ^= rng2 << 5;
    const float angle = ((rng2 & 0xFFFF) / 65535.f) * 2.f * 3.14159f;
    const float speed = 12.f + (rng2 & 0x1F);
    fx_emit(cx, cy, std::cos(angle) * speed, std::sin(angle) * speed,
            0.6f + (rng2 & 0x3F) * 0.005f, 1.8f, c);
}

// One-shot ring of particles radiating outward from (cx, cy).
void HudRenderer::fx_emit_burst(float cx, float cy, int count, ImU32 c) {
    for (int i = 0; i < count; ++i) {
        const float angle = (static_cast<float>(i) / count) * 2.f * 3.14159f;
        const float speed = 40.f + (i % 3) * 15.f;
        fx_emit(cx, cy,
                std::cos(angle) * speed, std::sin(angle) * speed,
                0.7f + i * 0.01f, 2.5f, c);
    }
}

// Continuous random sparks within the compass tape bounding box.
void HudRenderer::fx_emit_turbulence(float tape_cx, float tape_y,
                                      float tw, float th_tape,
                                      ImU32 c, float dt) {
    const float rate = 12.f;
    const float prob = rate * dt;
    static unsigned rng3 = 0xDEADBEEFu;
    rng3 ^= rng3 << 13; rng3 ^= rng3 >> 17; rng3 ^= rng3 << 5;
    const float frac = static_cast<float>(rng3 & 0xFFFF) / 65535.f;
    const int n = static_cast<int>(prob) + (frac < (prob - std::floor(prob)) ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        rng3 ^= rng3 << 13; rng3 ^= rng3 >> 17; rng3 ^= rng3 << 5;
        const float rx = (rng3 & 0xFFFF) / 65535.f;
        rng3 ^= rng3 << 13; rng3 ^= rng3 >> 17; rng3 ^= rng3 << 5;
        const float ry = (rng3 & 0xFFFF) / 65535.f;
        const float px = tape_cx - tw * 0.5f + rx * tw;
        const float py = tape_y  + ry * th_tape;
        rng3 ^= rng3 << 13; rng3 ^= rng3 >> 17; rng3 ^= rng3 << 5;
        const float angle = (rng3 & 0xFFFF) / 65535.f * 2.f * 3.14159f;
        const float speed = 5.f + (rng3 & 0x1F);
        fx_emit(px, py, std::cos(angle) * speed, std::sin(angle) * speed,
                0.3f + (rng3 & 0x3F) * 0.004f, 1.5f, c);
    }
}

// Master dispatcher — derives arm geometry the same way the draw_* helpers do.
void HudRenderer::fx_update(ImDrawList* dl, const AppState& s,
                             float fw, float fh, float dt) {
    const EffectType effect = s.effects_cfg.effect;

    // Popup burst: event-driven, fires once when a popup opens (only if PopupBurst selected)
    PopupKind cur_popup = popup_kind_;
    if (effect == EffectType::PopupBurst &&
        cur_popup != fx_prev_popup_ && cur_popup != PopupKind::None) {
        const ImU32 bc = (cur_popup == PopupKind::Alarm)
                         ? IM_COL32(220, 50, 50, 255)
                         : IM_COL32(220, 140, 20, 255);
        fx_emit_burst(fw * 0.5f, fh * 0.5f, 24, bc);
    }
    fx_prev_popup_ = cur_popup;

    if (effect == EffectType::None) {
        fx_draw(dl);
        return;
    }

    const ImU32 c = fx_palette_color(s);

    const float ch       = static_cast<float>(cfg_.compass_height);
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip     = cfg_.hud_flip_vertical;

    // Compass tape geometry (same formula as draw_frame)
    const float cw       = fw / 3.f;
    const float compass_y = flip ? c_margin : fh - ch - c_margin;
    const float tape_cx   = fw * 0.5f;

    if (effect == EffectType::CompassTurbulence) {
        fx_emit_turbulence(tape_cx, compass_y, cw, ch, c, dt);
    }

    if (effect == EffectType::CornerDrift) {
        // Four corners of the compass tape
        const float x0 = tape_cx - cw * 0.5f;
        const float x1 = tape_cx + cw * 0.5f;
        const float y0 = compass_y;
        const float y1 = compass_y + ch;
        fx_emit_corner_drift(x0, y0, c, dt);
        fx_emit_corner_drift(x1, y0, c, dt);
        fx_emit_corner_drift(x0, y1, c, dt);
        fx_emit_corner_drift(x1, y1, c, dt);
    }

    if (effect == EffectType::ArmGlints) {
        // Arm geometry mirrors draw_face_indicator / draw_lora_indicator / draw_clock_indicator.
        // Arms are at fixed angles: face ~210°, lora ~270°, clock ~330° (left side)
        // and mirrored on right side for health.
        // Use the same anchor_y the draw helpers use.
        const float anchor_y = flip ? c_margin : fh - c_margin;

        // Each indicator arm: left-center is ~ fw/3 wide; right ~2fw/3
        struct ArmDef { float ax; float angle_deg; };
        const ArmDef arms[] = {
            { fw * 0.33f, 225.f },   // face (left)
            { fw * 0.33f, 270.f },   // lora (left)
            { fw * 0.33f, 315.f },   // clock (left)
            { fw * 0.67f, 315.f },   // health right arm 1
            { fw * 0.67f, 270.f },   // health right arm 2
        };
        const float diag_len = std::min(fw, fh) * 0.22f;
        for (const auto& arm : arms) {
            const float rad   = arm.angle_deg * 3.14159f / 180.f;
            const float dx    =  std::cos(rad);
            const float dy_n  = -std::sin(rad);  // normal: arm goes up (negative y)
            const float dy    = flip ? std::sin(rad) : dy_n;
            fx_emit_arm_glint(arm.ax, anchor_y, dx, dy, diag_len, c, dt);
        }
    }

    // PopupBurst is event-driven (handled above); no per-frame emission needed.

    fx_draw(dl);
}

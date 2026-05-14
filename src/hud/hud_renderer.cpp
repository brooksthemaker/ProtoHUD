#include "hud_renderer.h"
#include "../serial/shm_frame_reader.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>
#include <nanovg_gl.h>

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

// NVG color conversion: ImU32 (0-7=R,8-15=G,16-23=B,24-31=A) → NVGcolor
static inline NVGcolor nvg_col(ImU32 c) {
    return nvgRGBA(c & 0xFF, (c>>8) & 0xFF, (c>>16) & 0xFF, c>>24);
}
static inline NVGcolor nvg_col_a(ImU32 c, uint8_t a) {
    return nvgRGBA(c & 0xFF, (c>>8) & 0xFF, (c>>16) & 0xFF, a);
}

// NVG three-layer glow line helper (wide glow2 → narrow glow1 → sharp major)
static void nvg_glow_line(NVGcontext* vg,
                           float x0, float y0, float x1, float y1,
                           NVGcolor col_maj, NVGcolor col_g1, NVGcolor col_g2) {
    nvgBeginPath(vg); nvgMoveTo(vg,x0,y0); nvgLineTo(vg,x1,y1);
    nvgStrokeWidth(vg, 5.f); nvgStrokeColor(vg, col_g2); nvgStroke(vg);
    nvgBeginPath(vg); nvgMoveTo(vg,x0,y0); nvgLineTo(vg,x1,y1);
    nvgStrokeWidth(vg, 2.5f); nvgStrokeColor(vg, col_g1); nvgStroke(vg);
    nvgBeginPath(vg); nvgMoveTo(vg,x0,y0); nvgLineTo(vg,x1,y1);
    nvgStrokeWidth(vg, 1.f); nvgStrokeColor(vg, col_maj); nvgStroke(vg);
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

// NVG glow text — 2-pass (blur+sharp) replaces 9-call ImGui offset pattern
static void nvg_glow_text(NVGcontext* vg, float x, float y, const char* text,
                           bool selected = true) {
    const ImU32 fill_u = selected ? IM_COL32(255,255,255,255) : IM_COL32(255,255,255,160);
    if (s_glow && s_glow_intensity > 0.f) {
        const uint8_t ga = static_cast<uint8_t>((selected ? 72 : 22) * s_glow_intensity);
        nvgFontBlur(vg, 3.0f);
        nvgFillColor(vg, nvg_col_a(s_glow_color_base, ga));
        nvgText(vg, x, y, text, nullptr);
        nvgFontBlur(vg, 0.f);
    }
    nvgFillColor(vg, nvg_col(fill_u));
    nvgText(vg, x, y, text, nullptr);
}
static void nvg_glow_text(NVGcontext* vg, float x, float y, const char* text,
                           bool selected, ImU32 glow_col, ImU32 fill_col) {
    const ImU32 fill = selected ? fill_col : with_alpha(fill_col, 160);
    if (s_glow && s_glow_intensity > 0.f) {
        const uint8_t ga = static_cast<uint8_t>((selected ? 72 : 22) * s_glow_intensity);
        nvgFontBlur(vg, 3.0f);
        nvgFillColor(vg, nvg_col_a(glow_col, ga));
        nvgText(vg, x, y, text, nullptr);
        nvgFontBlur(vg, 0.f);
    }
    nvgFillColor(vg, nvg_col(fill));
    nvgText(vg, x, y, text, nullptr);
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

    nvg_ = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (nvg_) {
        nvg_font_ui_   = nvgCreateFont(nvg_, "sans",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        nvg_font_mono_ = nvgCreateFont(nvg_, "mono",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
    }
}

void HudRenderer::unload() {
    if (!ctx_) return;
    if (nvg_) { nvgDeleteGLES2(nvg_); nvg_ = nullptr; }
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(ctx_);
    ctx_ = nullptr;
}

// ── Per-frame ─────────────────────────────────────────────────────────────────

void HudRenderer::set_dt(float dt) {
    frame_dt_         = dt;
    fps_shown_in_hud_ = false;
    s_glow            = cfg_.glow_enabled;
    s_glow_intensity  = cfg_.glow_intensity;
    s_glow_color_base = col_.glow_color;
    fx_tick(dt);
    fx_tick_lines(dt);
}

void HudRenderer::begin_menu_frame() {
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().FontGlobalScale = cfg_.text_scale;
}

void HudRenderer::render_menu_overlay() {
    ImGui::SetCurrentContext(ctx_);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void HudRenderer::nvg_set_font_ui(float sz) {
    nvgFontFaceId(nvg_, nvg_font_ui_);
    nvgFontSize(nvg_, sz > 0.f ? sz : 16.f * cfg_.text_scale);
    nvgFontBlur(nvg_, 0.f);
    nvgTextAlign(nvg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
}

void HudRenderer::nvg_set_font_mono(float sz) {
    nvgFontFaceId(nvg_, nvg_font_mono_);
    nvgFontSize(nvg_, sz > 0.f ? sz : 14.f * cfg_.text_scale);
    nvgFontBlur(nvg_, 0.f);
    nvgTextAlign(nvg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
}

// ── NVG HUD frame ─────────────────────────────────────────────────────────────

void HudRenderer::draw_hud_frame(const AppState& s, int w, int h, bool show_fps) {
    if (!nvg_) return;
    const float fw       = static_cast<float>(w);
    const float fh       = static_cast<float>(h);
    const float ch       = static_cast<float>(cfg_.compass_height);
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip     = cfg_.hud_flip_vertical;

    nvgBeginFrame(nvg_, fw, fh, 1.0f);

    fx_update(nvg_, s, fw, fh, frame_dt_);

    if (!s.lora_messages.empty()) {
        float pw    = static_cast<float>(cfg_.panel_width);
        float msg_w = std::min(pw, fw / 3.f);
        float msg_y = flip ? (c_margin + ch) : 0.f;
        draw_lora_messages(nvg_, s, 0.f, msg_y, msg_w, fh);
    }

    const float cw        = fw / 3.f;
    const float compass_y = flip ? c_margin : fh - ch - c_margin;
    draw_compass_tape(nvg_, s, fw / 2.f - cw / 2.f, compass_y, cw, ch);
    draw_health_side(nvg_, s.health, fw, fh, false,
                     s.focus_left, s.focus_right, s.night_vision.nv_enabled);
    draw_health_side(nvg_, s.health, fw, fh, true,
                     s.focus_left, s.focus_right, s.night_vision.nv_enabled);
    draw_face_indicator        (nvg_, s.face, fw, fh);
    draw_clock_indicator       (nvg_, s,      fw, fh);
    draw_timer_alarm_indicator (nvg_, s,      fw, fh);
    fx_draw_alarm_pulse(nvg_, s, fw, fh);

    if (show_fps) {
        draw_fps_nvg(nvg_, s, fw, fh);
        fps_shown_in_hud_ = true;
    }

    nvgEndFrame(nvg_);
    // Restore GL state NanoVG leaves dirty (stencil test, cull face)
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glStencilMask(0xFF);
}

void HudRenderer::draw_toasts(NotificationQueue& live_q, int w, int h) {
    if (!nvg_) return;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    nvgBeginFrame(nvg_, fw, fh, 1.0f);
    toast_renderer_.draw(nvg_, live_q, fw, fh, frame_dt_, nvg_font_ui_, nvg_font_mono_);
    nvgEndFrame(nvg_);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glStencilMask(0xFF);
}

void HudRenderer::draw_fps_overlay(const AppState& snap, int w, int h, bool active) {
    if (!active || fps_shown_in_hud_ || !nvg_) return;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    nvgBeginFrame(nvg_, fw, fh, 1.0f);
    draw_fps_nvg(nvg_, snap, fw, fh);
    nvgEndFrame(nvg_);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

void HudRenderer::draw_fps_nvg(NVGcontext* vg, const AppState& snap, float fw, float fh) {
    const float fps = snap.sys_metrics.fps_avg;
    const float ft  = snap.sys_metrics.frame_time_ms;
    char buf[32];
    if (fps > 0.f)
        snprintf(buf, sizeof(buf), "%.0f FPS  %.1fms",
                 static_cast<double>(fps), static_cast<double>(ft));
    else
        snprintf(buf, sizeof(buf), "-- FPS");

    nvg_set_font_mono();
    const float margin = 8.f;
    const float eye_w  = fw * 0.5f;
    const float text_w = nvgTextBounds(vg, 0, 0, buf, nullptr, nullptr);
    for (int eye = 0; eye < 2; ++eye) {
        const float off = eye * eye_w;
        const float bx  = off + eye_w - text_w - margin * 2.f;
        const float by  = margin;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx - 4.f, by - 2.f, text_w + 8.f, 14.f * cfg_.text_scale + 4.f, 3.f);
        nvgFillColor(vg, nvgRGBA(6, 8, 10, 140));
        nvgFill(vg);
        nvgFillColor(vg, nvg_col_a(col_.text_fill, 160));
        nvgText(vg, bx, by, buf, nullptr);
    }
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

    // 2. Camera image or "No Signal" placeholder — rounded corners match chamfer shape
    if (tex) {
        ImDrawFlags corner_flags;
        switch (cfg.anchor) {
            case A::TOP_LEFT:
            case A::BOTTOM_RIGHT:
                corner_flags = ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomRight;
                break;
            case A::TOP_RIGHT:
            case A::BOTTOM_LEFT:
                corner_flags = ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomLeft;
                break;
            default:
                corner_flags = ImDrawFlags_RoundCornersAll;
                break;
        }
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                            {x, y}, {x + bw, y + bh},
                            {0.f, 0.f}, {1.f, 1.f},
                            IM_COL32_WHITE, C, corner_flags);
    } else {
        if (font_mono_) ImGui::PushFont(font_mono_);
        dl->AddText({x + bw * 0.5f - 36.f, y + bh * 0.5f - 7.f},
                    col_.text_dim, "No Signal");
        if (font_mono_) ImGui::PopFont();
    }

    // 3. Label (top-left)
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

void HudRenderer::draw_top_bar(NVGcontext* vg, const AppState& s, float w, float bar_y) {
    const float th = static_cast<float>(cfg_.top_bar_height);
    nvgBeginPath(vg);
    nvgRect(vg, 0.f, bar_y, w, th);
    nvgFillColor(vg, nvg_col(col_.background));
    nvgFill(vg);

    const float scale     = std::max(0.5f, s.clock_cfg.font_scale) * 0.8f;
    const float font_size = 16.f * cfg_.text_scale * scale;
    nvg_set_font_ui(font_size);
    std::string time_str = fmt_clock(s.clock_cfg.use_24h, s.clock_cfg.show_seconds);
    float tsz_w = nvgTextBounds(vg, 0, 0, time_str.c_str(), nullptr, nullptr);
    const bool show_second = s.clock_cfg.show_date || s.timer_alarm.timer_active;
    float cy = bar_y + th * 0.5f - font_size * 0.5f - (show_second ? font_size * 0.5f : 0.f);
    nvg_glow_text(vg, w * 0.5f - tsz_w * 0.5f, cy, time_str.c_str(), true,
                  col_.glow_base, col_.text_fill);

    const float small = font_size * 0.75f;
    nvg_set_font_ui(small);
    if (s.timer_alarm.timer_active) {
        std::string cd = fmt_countdown(s.timer_alarm.timer_end);
        float cw2 = nvgTextBounds(vg, 0, 0, cd.c_str(), nullptr, nullptr);
        nvg_glow_text(vg, w * 0.5f - cw2 * 0.5f, cy + font_size + 1.f,
                      cd.c_str(), true, col_.warn, col_.warn);
    } else if (s.clock_cfg.show_date) {
        std::string ds = fmt_date();
        float dw = nvgTextBounds(vg, 0, 0, ds.c_str(), nullptr, nullptr);
        nvg_glow_text(vg, w * 0.5f - dw * 0.5f, cy + font_size + 1.f,
                      ds.c_str(), false, col_.glow_base, col_.text_fill);
    }

    int unread = s.unread_message_count();
    if (unread > 0) {
        char buf[32]; snprintf(buf, sizeof(buf), "MSG:%d", unread);
        nvg_set_font_mono();
        nvgFillColor(vg, nvg_col(col_.warn));
        nvgText(vg, w * 0.25f, bar_y + 10.f, buf, nullptr);
    }

    draw_audio_strip(vg, s.audio, w - 180.f, bar_y + 4.f, 170.f);
}

// ── Health side indicators ────────────────────────────────────────────────────

void HudRenderer::draw_health_side(NVGcontext* vg, const SystemHealth& h,
                                    float fw, float fh, bool right_side,
                                    const CameraFocusState& focus_left,
                                    const CameraFocusState& focus_right,
                                    bool nv_enabled) {
    const float tape_w   = fw / 3.f;
    const float tape_x   = fw / 2.f - tape_w / 2.f;
    const float fade_w   = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip     = cfg_.hud_flip_vertical;
    const float anchor_y = flip ? c_margin : fh - c_margin;
    const float anchor_x = right_side ? tape_x + tape_w + fade_w : tape_x - fade_w;

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
                                {"WiFi",      h.wifi_ok}};
    const Ind right_items[] = {{lcam_lbl,    h.cam_owl_left,  false},
                                {rcam_lbl,    h.cam_owl_right, false},
                                {"Cam 1",     h.cam_usb1,      h.cam_usb1 && !h.cam_usb1_overlay},
                                {"Cam 2",     h.cam_usb2,      h.cam_usb2 && !h.cam_usb2_overlay}};
    const Ind* items   = right_side ? right_items : left_items;
    const int  n_items = 4;

    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;
    constexpr float H_LEN   = 150.f;
    constexpr float BG_FULL = 440.f;

    const float dir_x    = std::cos(ANGLE) * (right_side ? 1.f : -1.f);
    const float dir_y    = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float diag_len = static_cast<float>(n_items + 1) * ROW_H;

    // Parallelogram bg via linear gradient (replaces 16 strip loop)
    if (cfg_.indicator_bg_enabled) {
        const uint8_t bg_a     = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        const float outer_sign = right_side ? 1.f : -1.f;
        const float p1x = anchor_x + outer_sign * BG_FULL;
        NVGpaint grad = nvgLinearGradient(vg,
            anchor_x, anchor_y, p1x, anchor_y,
            nvg_col_a(col_.compass_bg_color, bg_a),
            nvg_col_a(col_.compass_bg_color, 0));
        nvgBeginPath(vg);
        nvgMoveTo(vg, anchor_x, anchor_y);
        nvgLineTo(vg, p1x, anchor_y);
        nvgLineTo(vg, p1x + dir_x * diag_len, anchor_y + dir_y * diag_len);
        nvgLineTo(vg, anchor_x + dir_x * diag_len, anchor_y + dir_y * diag_len);
        nvgClosePath(vg);
        nvgFillPaint(vg, grad); nvgFill(vg);
    }

    NVGcolor cm = nvg_col(col_.glow_base);
    NVGcolor g1 = nvg_col_a(col_.glow_base, 70);
    NVGcolor g2 = nvg_col_a(col_.glow_base, 28);
    const float diag_ex = anchor_x + dir_x * diag_len;
    const float diag_ey = anchor_y + dir_y * diag_len;
    nvg_glow_line(vg, anchor_x, anchor_y, diag_ex, diag_ey, cm, g1, g2);
    const float h_end_x = anchor_x + (right_side ? H_LEN : -H_LEN);
    nvg_glow_line(vg, anchor_x, anchor_y, h_end_x, anchor_y, cm, g1, g2);

    if (nv_enabled) {
        nvg_set_font_mono();
        const float nv_tw = nvgTextBounds(vg, 0, 0, "NV", nullptr, nullptr);
        const float nv_x  = right_side ? h_end_x + 6.f : h_end_x - nv_tw - 6.f;
        nvg_glow_text(vg, nv_x, anchor_y - 7.f, "NV", true, col_.glow_base, col_.text_fill);
    }

    nvg_set_font_mono();
    for (int i = 0; i < n_items; ++i) {
        const float t  = static_cast<float>(i + 1) * ROW_H;
        const float ix = anchor_x + dir_x * t;
        const float iy = anchor_y + dir_y * t;
        if (items[i].inactive) {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_inactive)); nvgFill(vg);
        } else if (items[i].ok) {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R + 2.f);
            nvgFillColor(vg, nvg_col_a(col_.ind_good, 28)); nvgFill(vg);
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_good)); nvgFill(vg);
        } else {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_fail)); nvgFill(vg);
        }
        const char* lbl = items[i].label;
        if (right_side) {
            nvg_glow_text(vg, ix + DOT_R + 6.f, iy - 7.f, lbl, items[i].ok,
                          col_.text_fill, col_.text_fill);
        } else {
            float lw = nvgTextBounds(vg, 0, 0, lbl, nullptr, nullptr);
            nvg_glow_text(vg, ix - DOT_R - 6.f - lw, iy - 7.f, lbl, items[i].ok,
                          col_.text_fill, col_.text_fill);
        }
    }
}

// ── Audio strip ───────────────────────────────────────────────────────────────

void HudRenderer::draw_audio_strip(NVGcontext* vg, const AudioState& a,
                                    float ox, float oy, float w) {
    nvg_set_font_mono();
    if (!a.enabled) {
        nvg_glow_text(vg, ox, oy, "AUDIO OFF", false);
        return;
    }
    char buf[64];
    static const char* outputs[] = {"VITURE","JACK","HDMI"};
    const char* out_str = (a.output >= 0 && a.output < 3) ? outputs[a.output] : "?";
    snprintf(buf, sizeof(buf), "AU \xe2\x86\x92 %s  X:%d", out_str, a.xrun_count);
    nvg_glow_text(vg, ox, oy, buf, a.device_ok);

    const float bar_y = oy + 20.f;
    nvgBeginPath(vg); nvgRect(vg, ox, bar_y, w, 6.f);
    nvgFillColor(vg, nvgRGBA(20, 20, 20, 180)); nvgFill(vg);
    const float load_w = w * std::min(1.f, a.cpu_load);
    const ImU32 load_u = (a.cpu_load > 0.8f) ? col_.danger :
                         (a.cpu_load > 0.5f) ? col_.warn : col_.orange;
    nvgBeginPath(vg); nvgRect(vg, ox, bar_y, load_w, 6.f);
    nvgFillColor(vg, nvg_col(load_u)); nvgFill(vg);
}

// ── Face indicator arm (left side) ───────────────────────────────────────────
// Two parallel diagonal arms at 130°: [proto arm]  [health indicators]
// The health indicator diagonal itself is the visual divider between sections.
// SEG_W=75 puts proto at anchor_x-150 (= end of health side's 150px horiz line).

void HudRenderer::draw_face_indicator(NVGcontext* vg, const FaceState& f,
                                       float fw, float fh) {
    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float SEG_W   = 75.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;
    constexpr int   N_ITEMS = 4;

    const float tape_w         = fw / 3.f;
    const float tape_x         = fw / 2.f - tape_w / 2.f;
    const float fade_w         = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin       = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip           = cfg_.hud_flip_vertical;
    const float anchor_y       = flip ? c_margin : fh - c_margin;
    const float proto_anchor_x = tape_x - fade_w - SEG_W * 2.f;

    const float dir_x    = std::cos(ANGLE) * -1.f;
    const float dir_y    = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float diag_len = static_cast<float>(N_ITEMS + 1) * ROW_H;

    NVGcolor cm = nvg_col(col_.glow_base);
    NVGcolor g1 = nvg_col_a(col_.glow_base, 70);
    NVGcolor g2 = nvg_col_a(col_.glow_base, 28);
    nvg_glow_line(vg, proto_anchor_x, anchor_y, proto_anchor_x - ARM_EXT, anchor_y, cm, g1, g2);
    nvg_glow_line(vg, proto_anchor_x, anchor_y,
                  proto_anchor_x + dir_x * diag_len, anchor_y + dir_y * diag_len, cm, g1, g2);

    char effect_lbl[24], mode_lbl[24], brt_lbl[24];
    snprintf(effect_lbl, sizeof(effect_lbl), "%s", effect_name(f.effect_id));
    if (f.playing_gif) snprintf(mode_lbl, sizeof(mode_lbl), "GIF #%d", f.gif_id);
    else               snprintf(mode_lbl, sizeof(mode_lbl), "Pal #%d", f.palette_id);
    snprintf(brt_lbl, sizeof(brt_lbl), "Brt %d%%", (f.brightness * 100) / 255);
    const char* ctrl_lbl = f.hud_control ? "HUD" : "AUTO";

    struct Ind { const char* label; bool ok; };
    const Ind items[N_ITEMS] = {
        {effect_lbl, f.connected}, {mode_lbl, f.connected},
        {brt_lbl, f.connected},    {ctrl_lbl, f.connected},
    };

    nvg_set_font_mono();
    for (int i = 0; i < N_ITEMS; ++i) {
        const float t  = static_cast<float>(i + 1) * ROW_H;
        const float ix = proto_anchor_x + dir_x * t;
        const float iy = anchor_y       + dir_y * t;
        if (items[i].ok) {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R + 2.f);
            nvgFillColor(vg, nvg_col_a(col_.ind_good, 28)); nvgFill(vg);
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_good)); nvgFill(vg);
        } else {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_fail)); nvgFill(vg);
        }
        const char* lbl = items[i].label;
        float lw = nvgTextBounds(vg, 0, 0, lbl, nullptr, nullptr);
        nvg_glow_text(vg, ix - DOT_R - 6.f - lw, iy - 7.f, lbl, items[i].ok,
                      col_.text_fill, col_.text_fill);
    }
}

// ── LoRa indicator arm (right side) ──────────────────────────────────────────
// Two parallel diagonal arms at 130°: [health indicators]  [lora arm]
// The health indicator diagonal itself is the visual divider between sections.
// SEG_W=75 puts lora at anchor_x+150 (= end of health side's 150px horiz line).

void HudRenderer::draw_lora_indicator(NVGcontext* vg, const AppState& s,
                                       float fw, float fh) {
    constexpr float ROW_H    = 18.f;
    constexpr float DOT_R    = 4.f;
    constexpr float SEG_W    = 75.f;
    constexpr float ARM_EXT  = 140.f;
    constexpr float ANGLE    = 130.f * 3.14159265f / 180.f;
    constexpr int   MAX_ROWS = 4;

    const float tape_w        = fw / 3.f;
    const float tape_x        = fw / 2.f - tape_w / 2.f;
    const float fade_w        = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin      = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip          = cfg_.hud_flip_vertical;
    const float anchor_y      = flip ? c_margin : fh - c_margin;
    const float lora_anchor_x = tape_x + tape_w + fade_w + SEG_W * 2.f;

    const float dir_x    = std::cos(ANGLE);
    const float dir_y    = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float diag_len = static_cast<float>(MAX_ROWS + 1) * ROW_H;

    NVGcolor cm = nvg_col(col_.glow_base);
    NVGcolor g1 = nvg_col_a(col_.glow_base, 70);
    NVGcolor g2 = nvg_col_a(col_.glow_base, 28);
    nvg_glow_line(vg, lora_anchor_x, anchor_y, lora_anchor_x + ARM_EXT, anchor_y, cm, g1, g2);
    nvg_glow_line(vg, lora_anchor_x, anchor_y,
                  lora_anchor_x + dir_x * diag_len, anchor_y + dir_y * diag_len, cm, g1, g2);

    struct Ind { char label[32]; bool ok; };
    Ind items[MAX_ROWS] = {};
    int n_items = 0;
    snprintf(items[n_items].label, sizeof(items[n_items].label), "LORA");
    items[n_items].ok = s.health.lora_ok; n_items++;

    time_t now = std::time(nullptr);
    for (const auto& node : s.lora_nodes) {
        if (n_items >= MAX_ROWS) break;
        double age = difftime(now, node.last_seen);
        const char* nm = node.name.empty() ? "???" : node.name.c_str();
        snprintf(items[n_items].label, sizeof(items[n_items].label),
                 "%-6.6s %03.0f\xc2\xb0 %.1fk", nm, node.heading_deg, node.distance_m/1000.f);
        items[n_items].ok = (node.last_seen > 0 && age < 120.0);
        n_items++;
    }

    nvg_set_font_mono();
    for (int i = 0; i < n_items; ++i) {
        const float t  = static_cast<float>(i + 1) * ROW_H;
        const float ix = lora_anchor_x + dir_x * t;
        const float iy = anchor_y      + dir_y * t;
        if (items[i].ok) {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R+2.f);
            nvgFillColor(vg, nvg_col_a(col_.ind_good,28)); nvgFill(vg);
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_good)); nvgFill(vg);
        } else {
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, DOT_R);
            nvgFillColor(vg, nvg_col(col_.ind_fail)); nvgFill(vg);
        }
        nvg_glow_text(vg, ix+DOT_R+6.f, iy-7.f, items[i].label, items[i].ok,
                      col_.text_fill, col_.text_fill);
    }
}

// ── Clock arm (right side, outboard of LoRa arm) ─────────────────────────────
// Parallel diagonal arm at 130°. No dots — time and date rendered at font_scale.
// clock_anchor_x = lora_anchor_x + SEG_W*2 (another 150px right of LoRa anchor).

void HudRenderer::draw_clock_indicator(NVGcontext* vg, const AppState& s,
                                        float fw, float fh) {
    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float SEG_W   = 75.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;

    const float tape_w         = fw / 3.f;
    const float tape_x         = fw / 2.f - tape_w / 2.f;
    const float fade_w         = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin       = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip           = cfg_.hud_flip_vertical;
    const float anchor_y       = flip ? c_margin : fh - c_margin;
    const float clock_anchor_x = tape_x + tape_w + fade_w + SEG_W * 2.f;

    const float dir_x     = std::cos(ANGLE);
    const float dir_y     = flip ? std::sin(ANGLE) : -std::sin(ANGLE);
    const float scale     = std::max(0.5f, s.clock_cfg.font_scale);
    const float eff_row_h = ROW_H * scale;
    const int   n_rows    = s.clock_cfg.show_date ? 2 : 1;
    const float diag_len  = static_cast<float>(n_rows + 1) * eff_row_h;

    NVGcolor cm = nvg_col(col_.glow_base);
    NVGcolor g1 = nvg_col_a(col_.glow_base, 70);
    NVGcolor g2 = nvg_col_a(col_.glow_base, 28);
    nvg_glow_line(vg, clock_anchor_x, anchor_y, clock_anchor_x + ARM_EXT, anchor_y, cm, g1, g2);
    nvg_glow_line(vg, clock_anchor_x, anchor_y,
                  clock_anchor_x + dir_x * diag_len, anchor_y + dir_y * diag_len, cm, g1, g2);

    time_t now = std::time(nullptr) + static_cast<time_t>(s.clock_cfg.manual_offset_s);
    struct tm tm_buf = {};
    localtime_r(&now, &tm_buf);
    char time_str[24], date_str[24];
    if (s.clock_cfg.use_24h) {
        if (s.clock_cfg.show_seconds)
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        else
            snprintf(time_str, sizeof(time_str), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
    } else {
        int h12 = tm_buf.tm_hour % 12; if (h12 == 0) h12 = 12;
        snprintf(time_str, sizeof(time_str), "%d:%02d %s",
                 h12, tm_buf.tm_min, tm_buf.tm_hour < 12 ? "AM" : "PM");
    }
    static const char* dow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(date_str, sizeof(date_str), "%s %d %s",
             dow[tm_buf.tm_wday], tm_buf.tm_mday, mon[tm_buf.tm_mon]);

    const float font_size = 14.f * cfg_.text_scale * scale;
    nvg_set_font_mono(font_size);
    const char* rows[2] = { time_str, date_str };
    for (int i = 0; i < n_rows; ++i) {
        const float t  = static_cast<float>(i + 1) * eff_row_h;
        const float ix = clock_anchor_x + dir_x * t;
        const float iy = anchor_y       + dir_y * t;
        nvg_glow_text(vg, ix + 6.f, iy - font_size * 0.5f,
                      rows[i], true, col_.glow_base, col_.text_fill);
    }
}

// ── Timer / Alarm indicator arm (right side, outboard of clock arm) ──────────
// Visible only when a timer or alarm is active.
// Timer row: countdown to expiry (MM:SS).  Alarm row: set time (HH:MM).

void HudRenderer::draw_timer_alarm_indicator(NVGcontext* vg, const AppState& s,
                                              float fw, float fh) {
    const auto& ta = s.timer_alarm;
    if (!ta.timer_active && !ta.alarm_active) return;

    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float SEG_W   = 75.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;

    const float tape_w      = fw / 3.f;
    const float tape_x      = fw / 2.f - tape_w / 2.f;
    const float fade_w      = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin    = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip        = cfg_.hud_flip_vertical;
    const float anchor_y    = flip ? c_margin : fh - c_margin;
    const float ta_anchor_x = tape_x + tape_w + fade_w + SEG_W * 4.f;

    const float dir_x = std::cos(ANGLE);
    const float dir_y = flip ? std::sin(ANGLE) : -std::sin(ANGLE);

    struct Row { char text[24]; ImU32 accent; };
    Row rows[2]; int n_rows = 0;
    if (ta.timer_active) {
        int rem = static_cast<int>(ta.timer_end - time(nullptr));
        if (rem < 0) rem = 0;
        snprintf(rows[n_rows].text, sizeof(rows[0].text), "TMR %02d:%02d", rem/60, rem%60);
        rows[n_rows].accent = (rem < 60) ? col_.warn : col_.glow_base; ++n_rows;
    }
    if (ta.alarm_active) {
        if (s.clock_cfg.use_24h)
            snprintf(rows[n_rows].text, sizeof(rows[0].text),
                     "ALM %02d:%02d", ta.alarm_hour, ta.alarm_minute);
        else {
            int h = ta.alarm_hour % 12; if (h==0) h=12;
            snprintf(rows[n_rows].text, sizeof(rows[0].text),
                     "ALM %d:%02d%s", h, ta.alarm_minute, ta.alarm_hour<12?"A":"P");
        }
        rows[n_rows].accent = col_.glow_base; ++n_rows;
    }

    const float scale     = std::max(0.5f, s.clock_cfg.font_scale);
    const float eff_row_h = ROW_H * scale;
    const float diag_len  = static_cast<float>(n_rows + 1) * eff_row_h;
    const float font_size = 14.f * cfg_.text_scale * scale;

    NVGcolor cm = nvg_col(col_.glow_base);
    NVGcolor g1 = nvg_col_a(col_.glow_base, 70);
    NVGcolor g2 = nvg_col_a(col_.glow_base, 28);
    nvg_glow_line(vg, ta_anchor_x, anchor_y, ta_anchor_x + ARM_EXT, anchor_y, cm, g1, g2);
    nvg_glow_line(vg, ta_anchor_x, anchor_y,
                  ta_anchor_x + dir_x * diag_len, anchor_y + dir_y * diag_len, cm, g1, g2);

    nvg_set_font_mono(font_size);
    for (int i = 0; i < n_rows; ++i) {
        const float t  = static_cast<float>(i + 1) * eff_row_h;
        const float ix = ta_anchor_x + dir_x * t;
        const float iy = anchor_y    + dir_y * t;
        nvg_glow_text(vg, ix + 6.f, iy - font_size * 0.5f,
                      rows[i].text, true, rows[i].accent, col_.text_fill);
    }
}

// ── LoRa messages panel ───────────────────────────────────────────────────────

void HudRenderer::draw_lora_messages(NVGcontext* vg, const AppState& s,
                                      float ox, float oy, float pw, float ph) {
    nvgBeginPath(vg); nvgRect(vg, ox, oy, pw, ph);
    nvgFillColor(vg, nvgRGBA(10, 15, 20, 230)); nvgFill(vg);
    nvgBeginPath(vg); nvgMoveTo(vg, ox+pw, oy); nvgLineTo(vg, ox+pw, oy+ph);
    nvgStrokeWidth(vg, 1.5f); nvgStrokeColor(vg, nvg_col(col_.orange)); nvgStroke(vg);

    nvg_set_font_ui();
    nvg_glow_text(vg, ox + 8.f, oy + 6.f, "MESSAGES");

    nvg_set_font_mono();
    float py = oy + 28.f;
    for (const auto& msg : s.lora_messages) {
        if (py + 38.f > oy + ph) break;
        std::string sender;
        for (const auto& n : s.lora_nodes)
            if (n.local_id == msg.local_id) { sender = n.name; break; }
        if (sender.empty()) {
            char id[12]; snprintf(id, sizeof(id), "ID:%02X", msg.local_id);
            sender = id;
        }
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "[%s  %s]",
                 sender.substr(0, 10).c_str(), fmt_time(msg.timestamp).c_str());
        nvg_glow_text(vg, ox + 8.f, py, hdr);
        py += 16.f;
        nvg_glow_text(vg, ox + 8.f, py, msg.text.c_str(), !msg.read);
        py += 22.f;
    }
}

// ── Compass tape ──────────────────────────────────────────────────────────────

void HudRenderer::draw_compass_tape(NVGcontext* vg, const AppState& s,
                                     float ox, float oy, float tw, float th) {
    const float heading  = s.compass_heading;
    const float ppd      = tw / 120.f;
    const float center_x = ox + tw / 2.f;

    const ImU32 col_major = col_.compass_tick;
    const ImU32 col_mid   = with_alpha(col_.compass_tick, 180);
    const ImU32 col_minor = with_alpha(col_.compass_tick, 110);
    const ImU32 col_glow1 = with_alpha(col_.compass_glow, 70);
    const ImU32 col_glow2 = with_alpha(col_.compass_glow, 28);

    const float fade_w = static_cast<float>(cfg_.compass_bg_side_fade);
    const bool  flip   = cfg_.hud_flip_vertical;

    if (s.compass_bg_enabled) {
        const uint8_t a = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        constexpr float kIndAngle = 130.f * 3.14159265f / 180.f;
        const float inset = std::abs(std::cos(kIndAngle)) / std::sin(kIndAngle) * th;

        nvgBeginPath(vg);
        if (!flip) {
            nvgMoveTo(vg, ox - fade_w + inset,      oy);
            nvgLineTo(vg, ox + tw + fade_w - inset, oy);
            nvgLineTo(vg, ox + tw + fade_w,         oy + th);
            nvgLineTo(vg, ox - fade_w,              oy + th);
        } else {
            nvgMoveTo(vg, ox - fade_w,              oy);
            nvgLineTo(vg, ox + tw + fade_w,         oy);
            nvgLineTo(vg, ox + tw + fade_w - inset, oy + th);
            nvgLineTo(vg, ox - fade_w + inset,      oy + th);
        }
        nvgClosePath(vg);
        nvgFillColor(vg, nvg_col_a(col_.compass_bg_color, a));
        nvgFill(vg);
    }

    // Glow line at tape edge that meets the indicator arms
    {
        const float lx0    = ox - fade_w, lx1 = ox + tw + fade_w;
        const float line_y = flip ? oy : oy + th;
        nvg_glow_line(vg, lx0, line_y, lx1, line_y,
                      nvg_col(col_.glow_base),
                      nvg_col_a(col_.glow_base, 70),
                      nvg_col_a(col_.glow_base, 28));
    }

    const float t_maj = static_cast<float>(cfg_.compass_tick_length);
    const float t_mid = t_maj * (16.f / 24.f);
    const float t_min = t_maj * (10.f / 24.f);

    // Align degree labels with the first dot row of the indicator arms.
    // arm anchor sits at oy+th; first dot row = anchor - sin(130°)*ROW_H
    constexpr float kArmAngle = 130.f * 3.14159265f / 180.f;
    constexpr float kRowH     = 18.f;
    const float label_y   = flip ? oy + 3.f : (oy + th) - std::sin(kArmAngle) * kRowH;
    const float tick_base = flip ? oy + 20.f : label_y - 3.f;
    const float tick_sign = flip ? 1.f : -1.f;
    const bool  tick_glow = cfg_.compass_tick_glow;

    // Collect tick positions per tier for batched drawing
    float maj_px[8];  int n_maj = 0;
    float mid_px[36]; int n_mid = 0;
    float min_px[24]; int n_min = 0;

    struct LabelEntry { float px; char buf[8]; };
    LabelEntry labels[44]; int n_labels = 0;

    for (int deg = 0; deg < 360; deg++) {
        float offset = static_cast<float>(deg) - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;

        float px = center_x + offset * ppd;
        if (px < ox || px > ox + tw) continue;

        if (deg % 45 == 0) {
            if (n_maj < 8) maj_px[n_maj++] = px;
            if (n_labels < 44) {
                labels[n_labels].px = px;
                strncpy(labels[n_labels].buf, cardinal_str(static_cast<float>(deg)), 7);
                labels[n_labels].buf[7] = '\0';
                ++n_labels;
            }
        } else if (deg % 10 == 0) {
            if (n_mid < 36) mid_px[n_mid++] = px;
            if (n_labels < 44) {
                labels[n_labels].px = px;
                snprintf(labels[n_labels].buf, 8, "%d", deg);
                ++n_labels;
            }
        } else if (deg % 5 == 0) {
            if (n_min < 24) min_px[n_min++] = px;
        }
    }

    // Batched glow passes (one path per tier per glow layer)
    if (tick_glow) {
        auto draw_tick_batch = [&](float* px_arr, int n, float len, float w, NVGcolor col) {
            nvgBeginPath(vg);
            for (int i = 0; i < n; ++i) {
                nvgMoveTo(vg, px_arr[i], tick_base);
                nvgLineTo(vg, px_arr[i], tick_base + tick_sign * len);
            }
            nvgStrokeWidth(vg, w); nvgStrokeColor(vg, col); nvgStroke(vg);
        };
        draw_tick_batch(maj_px, n_maj, t_maj, t_maj * 0.5f,  nvg_col(col_glow2));
        draw_tick_batch(maj_px, n_maj, t_maj, t_maj * 0.25f, nvg_col(col_glow1));
        draw_tick_batch(mid_px, n_mid, t_mid, t_mid * 0.375f, nvg_col(col_glow2));
        draw_tick_batch(min_px, n_min, t_min, t_min * 0.4f,  nvg_col(col_glow2));
    }

    // Batched solid tick draws
    {
        auto draw_solid = [&](float* px_arr, int n, float len, float w, NVGcolor col) {
            nvgBeginPath(vg);
            for (int i = 0; i < n; ++i) {
                nvgMoveTo(vg, px_arr[i], tick_base);
                nvgLineTo(vg, px_arr[i], tick_base + tick_sign * len);
            }
            nvgStrokeWidth(vg, w); nvgStrokeColor(vg, col); nvgStroke(vg);
        };
        draw_solid(maj_px, n_maj, t_maj, 3.f, nvg_col(col_major));
        draw_solid(mid_px, n_mid, t_mid, 2.f, nvg_col(col_mid));
        draw_solid(min_px, n_min, t_min, 2.f, nvg_col(col_minor));
    }

    // Labels
    nvg_set_font_mono(0.f);
    for (int i = 0; i < n_labels; ++i) {
        float bounds[4];
        nvgTextBounds(vg, 0, 0, labels[i].buf, nullptr, bounds);
        float lw = bounds[2] - bounds[0];
        nvg_glow_text(vg, labels[i].px - lw * 0.5f, label_y, labels[i].buf,
                      true, col_.glow_base, col_.text_fill);
    }

    // LoRa node bearing markers — triangles on the inner (non-label) side of ticks
    for (const auto& node : s.lora_nodes) {
        if (node.distance_m <= 0.f) continue;
        float offset = node.heading_deg - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;
        float px = center_x + offset * ppd;
        if (px < ox || px > ox + tw) continue;

        float my = tick_base + tick_sign * t_maj + (flip ? 6.f : -6.f);
        float tri_tip_dy = flip ? -9.f : 9.f;

        nvgBeginPath(vg);
        nvgMoveTo(vg, px - 5.f, my);
        nvgLineTo(vg, px + 5.f, my);
        nvgLineTo(vg, px, my + tri_tip_dy);
        nvgClosePath(vg);
        nvgFillColor(vg, nvg_col(s.lora_node_colors[node.local_id % 8]));
        nvgFill(vg);
        nvgStrokeColor(vg, nvg_col_a(s.lora_node_colors[node.local_id % 8], 200));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);
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

void HudRenderer::fx_draw(NVGcontext* vg) const {
    if (n_particles_ == 0) return;
    // Batch by base color (strip alpha) — reduces 256 particles × 2 GL calls to ≤ 8 × 2
    ImU32 unique_cols[32]; int n_unique = 0;
    for (int i = 0; i < n_particles_; ++i) {
        ImU32 base = particles_[i].color & 0x00FFFFFFu;
        bool found = false;
        for (int j = 0; j < n_unique; ++j) if (unique_cols[j] == base) { found = true; break; }
        if (!found && n_unique < 32) unique_cols[n_unique++] = base;
    }
    for (int ci = 0; ci < n_unique; ++ci) {
        ImU32 base = unique_cols[ci];
        nvgBeginPath(vg);
        uint8_t max_a = 0;
        for (int i = 0; i < n_particles_; ++i) {
            if ((particles_[i].color & 0x00FFFFFFu) != base) continue;
            const Particle& p = particles_[i];
            uint8_t a = static_cast<uint8_t>((p.life / p.life_total) * 220.f);
            if (a > max_a) max_a = a;
            nvgCircle(vg, p.x, p.y, p.size);
        }
        nvgFillColor(vg, nvgRGBA(base & 0xFF, (base>>8) & 0xFF, (base>>16) & 0xFF, max_a));
        nvgFill(vg);
    }
}

void HudRenderer::fx_tick_lines(float dt) {
    if (dt <= 0.f || n_line_particles_ == 0) return;
    int i = 0;
    while (i < n_line_particles_) {
        LineParticle& p = line_particles_[i];
        p.life -= dt;
        if (p.life <= 0.f) {
            line_particles_[i] = line_particles_[--n_line_particles_];
        } else {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            ++i;
        }
    }
}

void HudRenderer::fx_emit_line(float x, float y, float vx, float vy,
                                float life, float len, ImU32 color) {
    if (n_line_particles_ >= kMaxLineParticles) {
        line_particles_[0] = { x, y, vx, vy, life, life, len, color };
        return;
    }
    line_particles_[n_line_particles_++] = { x, y, vx, vy, life, life, len, color };
}

// Draw comets: bright head circle + three-segment tail that fades to transparent.
void HudRenderer::fx_draw_lines(NVGcontext* vg) const {
    for (int i = 0; i < n_line_particles_; ++i) {
        const LineParticle& p = line_particles_[i];
        const float frac = p.life / p.life_total;
        float af;
        if      (frac > 0.85f) af = (1.f - frac) / 0.15f;
        else if (frac < 0.25f) af = frac / 0.25f;
        else                   af = 1.f;

        const float spd = std::sqrt(p.vx * p.vx + p.vy * p.vy);
        float dx = 1.f, dy = 0.f;
        if (spd > 0.01f) { dx = p.vx / spd; dy = p.vy / spd; }

        const float hx   = p.x + dx * 3.f,           hy   = p.y + dy * 3.f;
        const float q1x  = p.x - dx * p.len * 0.25f, q1y  = p.y - dy * p.len * 0.25f;
        const float q2x  = p.x - dx * p.len * 0.60f, q2y  = p.y - dy * p.len * 0.60f;
        const float tx   = p.x - dx * p.len,          ty   = p.y - dy * p.len;

        const NVGcolor c0 = nvg_col_a(p.color, static_cast<uint8_t>(af * 230.f));
        const NVGcolor c1 = nvg_col_a(p.color, static_cast<uint8_t>(af * 110.f));
        const NVGcolor c2 = nvg_col_a(p.color, static_cast<uint8_t>(af *  38.f));

        // Head
        nvgBeginPath(vg); nvgCircle(vg, hx, hy, 1.7f);
        nvgFillColor(vg, c0); nvgFill(vg);
        // Tail segments
        nvgBeginPath(vg); nvgMoveTo(vg, hx, hy); nvgLineTo(vg, q1x, q1y);
        nvgStrokeWidth(vg, 1.5f); nvgStrokeColor(vg, c0); nvgStroke(vg);
        nvgBeginPath(vg); nvgMoveTo(vg, q1x, q1y); nvgLineTo(vg, q2x, q2y);
        nvgStrokeWidth(vg, 1.1f); nvgStrokeColor(vg, c1); nvgStroke(vg);
        nvgBeginPath(vg); nvgMoveTo(vg, q2x, q2y); nvgLineTo(vg, tx, ty);
        nvgStrokeWidth(vg, 0.8f); nvgStrokeColor(vg, c2); nvgStroke(vg);
    }
}

// Layered dark-blue/violet gradient vignette along all four edges to evoke a nebula cloud.
void HudRenderer::fx_draw_nebula_cloud(NVGcontext* vg, float fw, float fh) const {
    // Each layer uses nvgBoxGradient over a fullscreen rect: one draw call covers
    // all four edges and corners simultaneously instead of four separate strips.
    // The inner box sits `depth` pixels from each screen edge; the feather of
    // `depth` pixels then carries the gradient outward to the screen boundary.
    struct CloudLayer { float depth; uint8_t r, g, b, a; };
    static const CloudLayer layers[] = {
        {  80.f,  3,  2, 18, 215 },
        { 130.f,  8,  4, 38, 130 },
        { 195.f, 20,  8, 65,  55 },
    };
    for (const auto& l : layers) {
        const float d    = l.depth;
        const NVGcolor clear = nvgRGBA(l.r, l.g, l.b,    0);
        const NVGcolor edge  = nvgRGBA(l.r, l.g, l.b, l.a);
        NVGpaint p = nvgBoxGradient(vg, d, d, fw - 2.f*d, fh - 2.f*d,
                                    0.f, d, clear, edge);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, fw, fh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
}

void HudRenderer::fx_draw_vignette(NVGcontext* vg, float fw, float fh) const {
    // Soft outer band: wide, gentle dark falloff from edges
    {
        constexpr float d = 200.f;
        NVGpaint p = nvgBoxGradient(vg, d, d, fw - 2.f*d, fh - 2.f*d,
                                    0.f, d, nvgRGBA(0,0,0,0), nvgRGBA(0,0,0,160));
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, fw, fh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
    // Hard inner ring: thin strip at the very edge for a clean border line
    {
        constexpr float d = 55.f;
        NVGpaint p = nvgBoxGradient(vg, d, d, fw - 2.f*d, fh - 2.f*d,
                                    0.f, d, nvgRGBA(0,0,0,0), nvgRGBA(0,0,0,220));
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, fw, fh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
}

// ── Alarm / timer heartbeat pulse ────────────────────────────────────────────

void HudRenderer::fx_draw_alarm_pulse(NVGcontext* vg, const AppState& s,
                                       float fw, float fh) {
    const bool alarm_on = s.timer_alarm.alarm_triggered;
    const bool timer_on = s.timer_alarm.timer_triggered;

    if (!alarm_on && !timer_on) {
        fx_pulse_phase_ = 1.f;   // park at cycle-end so next trigger starts fresh
        return;
    }

    fx_pulse_phase_ += frame_dt_ / 1.5f;
    if (fx_pulse_phase_ >= 1.f) fx_pulse_phase_ -= 1.f;

    const float phase = fx_pulse_phase_;

    // Double heartbeat envelope: two quick beats in the first 35% of the 1.5 s cycle,
    // then silence until the next cycle.
    // kTau  — per-beat decay time constant in phase units (~0.09 s real time at 1.5 s cycle)
    // kBeat2 — onset of the weaker second beat (lub-DUB gap ~0.22 s real time)
    static constexpr float kTau   = 0.06f;
    static constexpr float kBeat2 = 0.15f;

    float alpha_frac = 0.f;
    if (phase < 0.35f) {
        const float beat1 = std::exp(-phase / kTau);
        const float beat2 = (phase >= kBeat2)
                            ? 0.70f * std::exp(-(phase - kBeat2) / kTau)
                            : 0.f;
        alpha_frac = std::max(beat1, beat2);
    }

    const uint8_t a = static_cast<uint8_t>(alpha_frac * 210.f);
    if (a == 0) return;

    // Depth uses sqrt so the glow lingers spatially a beat longer than the alpha
    const float max_depth = std::min(fw, fh) * 0.32f;
    const float depth     = max_depth * std::sqrt(alpha_frac);
    if (depth < 1.f) return;

    const ImU32 base = alarm_on ? col_.danger : col_.warn;
    const NVGcolor edge  = nvg_col_a(base, a);
    const NVGcolor clear = nvg_col_a(base, 0);

    NVGpaint p;
    // Left edge
    p = nvgLinearGradient(vg, 0, 0, depth, 0, edge, clear);
    nvgBeginPath(vg); nvgRect(vg, 0, 0, depth, fh);
    nvgFillPaint(vg, p); nvgFill(vg);
    // Right edge
    p = nvgLinearGradient(vg, fw - depth, 0, fw, 0, clear, edge);
    nvgBeginPath(vg); nvgRect(vg, fw - depth, 0, depth, fh);
    nvgFillPaint(vg, p); nvgFill(vg);
    // Top edge
    p = nvgLinearGradient(vg, 0, 0, 0, depth, edge, clear);
    nvgBeginPath(vg); nvgRect(vg, 0, 0, fw, depth);
    nvgFillPaint(vg, p); nvgFill(vg);
    // Bottom edge
    p = nvgLinearGradient(vg, 0, fh - depth, 0, fh, clear, edge);
    nvgBeginPath(vg); nvgRect(vg, 0, fh - depth, fw, depth);
    nvgFillPaint(vg, p); nvgFill(vg);
}

// Nebula color palette — blue/violet/purple spectrum with occasional pale star
static const ImU32 kNebulaColors[] = {
    IM_COL32( 30,  10,  90, 255),   // deep indigo
    IM_COL32( 60,  30, 180, 255),   // blue-violet
    IM_COL32( 30, 100, 200, 255),   // steel blue
    IM_COL32(150, 130, 240, 255),   // pale lavender
    IM_COL32(200, 200, 255, 255),   // near-white blue
    IM_COL32(120,  30, 200, 255),   // hot purple
    IM_COL32( 80,  60, 220, 255),   // periwinkle
    IM_COL32(  0,  80, 160, 255),   // deep ocean blue
};
static constexpr int kNebCount = static_cast<int>(sizeof(kNebulaColors) / sizeof(kNebulaColors[0]));

// Emit nebula-style dot and line particles along all four screen edges.
void HudRenderer::fx_emit_nebula_edge(float fw, float fh, float dt) {
    static unsigned rngN = 0x7F3A9B1Cu;
    auto step  = [&]() -> unsigned {
        rngN ^= rngN << 13; rngN ^= rngN >> 17; rngN ^= rngN << 5;
        return rngN;
    };
    auto rf    = [&]() { return static_cast<float>(step() & 0xFFFF) / 65535.f; };
    auto ncol  = [&]() { return kNebulaColors[step() % kNebCount]; };

    const float dot_rate  = 9.0f;   // per edge per second
    const float line_rate = 3.5f;

    // Each edge described by: spawn origin, along-edge unit vector, inward unit vector,
    // edge length, and base line-segment angle (parallel to edge).
    struct EdgeDef {
        float ox, oy;   // start corner
        float ax, ay;   // along-edge unit vector
        float ix, iy;   // inward-normal unit vector
        float edge_len;
    };
    const EdgeDef edges[] = {
        { 0.f, 0.f,  1.f, 0.f,  0.f,  1.f, fw },  // top    → drift down
        { 0.f, fh,   1.f, 0.f,  0.f, -1.f, fw },  // bottom → drift up
        { 0.f, 0.f,  0.f, 1.f,  1.f,  0.f, fh },  // left   → drift right
        { fw,  0.f,  0.f, 1.f, -1.f,  0.f, fh },  // right  → drift left
    };

    for (const auto& e : edges) {
        // Dot particle
        if (rf() < dot_rate * dt) {
            const float t   = rf() * e.edge_len;
            const float px  = e.ox + e.ax * t;
            const float py  = e.oy + e.ay * t;
            const float spd = 5.f + rf() * 18.f;
            const float vx  = e.ix * spd + e.ax * (rf() - 0.5f) * 8.f;
            const float vy  = e.iy * spd + e.ay * (rf() - 0.5f) * 8.f;
            const float sz  = 0.8f + rf() * 2.4f;
            fx_emit(px, py, vx, vy, 1.6f + rf() * 2.2f, sz, ncol());
        }

        // Comet streak particle — tail length drives visual length
        if (rf() < line_rate * dt) {
            const float t   = rf() * e.edge_len;
            const float px  = e.ox + e.ax * t;
            const float py  = e.oy + e.ay * t;
            const float spd = 4.f + rf() * 14.f;
            const float vx  = e.ix * spd + e.ax * (rf() - 0.5f) * 6.f;
            const float vy  = e.iy * spd + e.ay * (rf() - 0.5f) * 6.f;
            const float len = 14.f + rf() * 38.f;   // 14–52 px tail
            fx_emit_line(px, py, vx, vy, 2.2f + rf() * 2.5f, len, ncol());
        }
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
void HudRenderer::fx_update(NVGcontext* vg, const AppState& s,
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
        fx_draw(vg);
        return;
    }

    const ImU32 c = fx_palette_color(s);

    const float ch       = static_cast<float>(cfg_.compass_height);
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip     = cfg_.hud_flip_vertical;

    const float cw        = fw / 3.f;
    const float compass_y = flip ? c_margin : fh - ch - c_margin;
    const float tape_cx   = fw * 0.5f;

    if (effect == EffectType::CompassTurbulence) {
        fx_emit_turbulence(tape_cx, compass_y, cw, ch, c, dt);
    }

    if (effect == EffectType::CornerDrift) {
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
        const float anchor_y = flip ? c_margin : fh - c_margin;
        struct ArmDef { float ax; float angle_deg; };
        const ArmDef arms[] = {
            { fw * 0.33f, 225.f },
            { fw * 0.33f, 270.f },
            { fw * 0.33f, 315.f },
            { fw * 0.67f, 315.f },
            { fw * 0.67f, 270.f },
        };
        const float diag_len = std::min(fw, fh) * 0.22f;
        for (const auto& arm : arms) {
            const float rad  = arm.angle_deg * 3.14159f / 180.f;
            const float dx   =  std::cos(rad);
            const float dy_n = -std::sin(rad);
            const float dy   = flip ? std::sin(rad) : dy_n;
            fx_emit_arm_glint(arm.ax, anchor_y, dx, dy, diag_len, c, dt);
        }
    }

    if (effect == EffectType::NebulaEdge) {
        fx_draw_nebula_cloud(vg, fw, fh);
        fx_emit_nebula_edge(fw, fh, dt);
    }

    if (effect == EffectType::DarkVignette)
        fx_draw_vignette(vg, fw, fh);

    fx_draw(vg);
    fx_draw_lines(vg);
}

// ── System status panel ───────────────────────────────────────────────────────

void HudRenderer::draw_sys_panel(const AppState& snap, int w, int h, bool active) {
    if (!active) return;

    ImGui::SetCurrentContext(ctx_);

    const float sw = static_cast<float>(w);
    const float sh = static_cast<float>(h);

    ImGui::SetNextWindowPos ({0.f, 0.f});
    ImGui::SetNextWindowSize({sw,  sh });
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin("##sys_panel", nullptr,
        ImGuiWindowFlags_NoDecoration          |
        ImGuiWindowFlags_NoInputs              |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoNav                 |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Panel geometry — top-left, fixed size
    constexpr float PW    = 340.f;
    constexpr float PH    = 580.f;
    constexpr float PAD   = 10.f;
    constexpr float MRG   = 14.f;   // left margin from screen edge
    const float px = MRG;
    const float py = MRG;

    // Background + border
    dl->AddRectFilled({px, py}, {px + PW, py + PH}, col_.panel_bg, 6.f);
    dl->AddRect      ({px, py}, {px + PW, py + PH}, col_.primary,  6.f, 0, 1.5f);

    if (font_mono_) ImGui::PushFont(font_mono_);
    const float lh = ImGui::GetTextLineHeight();

    float cy = py + PAD;   // current y cursor

    // Helper: draw a section header + separator
    auto section = [&](const char* title) {
        hud_glow_text(dl, {px + PAD, cy}, title, true, col_.primary, col_.primary);
        cy += lh + 2.f;
        dl->AddLine({px + PAD, cy}, {px + PW - PAD, cy}, with_alpha(col_.primary, 80), 1.f);
        cy += 5.f;
    };

    // Sparkline helper: draws oldest→newest from circular ring buffer
    auto sparkline = [&](const float* buf, int n, int head,
                         ImVec2 origin, float spw, float sph,
                         float scale_max, ImU32 col) {
        if (scale_max <= 0.f) return;
        const float step = spw / float(n - 1);
        for (int i = 0; i < n - 1; ++i) {
            const int   idx0 = (head + 1 + i)     % n;
            const int   idx1 = (head + 1 + i + 1) % n;
            const float v0   = std::min(buf[idx0], scale_max) / scale_max;
            const float v1   = std::min(buf[idx1], scale_max) / scale_max;
            dl->AddLine({origin.x + i * step,       origin.y + sph * (1.f - v0)},
                        {origin.x + (i+1) * step,   origin.y + sph * (1.f - v1)},
                        col, 1.5f);
        }
        // Frame
        dl->AddRect({origin.x, origin.y}, {origin.x + spw, origin.y + sph},
                    with_alpha(col, 60), 0.f, 0, 1.f);
    };

    // ── SYSTEM ────────────────────────────────────────────────────────────────
    section("SYSTEM");

    // Uptime
    {
        const uint64_t s = snap.sys_metrics.uptime_s;
        const uint32_t d = static_cast<uint32_t>(s / 86400);
        const uint32_t hh = static_cast<uint32_t>((s % 86400) / 3600);
        const uint32_t mm = static_cast<uint32_t>((s % 3600) / 60);
        char buf[48];
        snprintf(buf, sizeof(buf), "Up: %ud %02uh %02um", d, hh, mm);
        hud_glow_text(dl, {px + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
        cy += lh + 4.f;
    }

    // CPU sparkline row
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "CPU  %3.0f%%", static_cast<double>(snap.sys_metrics.cpu_pct));
        hud_glow_text(dl, {px + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
        const float spw = PW - PAD * 2.f - 80.f;
        sparkline(snap.sys_metrics.cpu_history, kSysHistLen, snap.sys_metrics.history_head,
                  {px + PAD + 78.f, cy}, spw, lh,
                  100.f, col_.accent);
        cy += lh + 6.f;
    }

    // RAM sparkline row
    {
        const float used  = snap.sys_metrics.ram_used_mb;
        const float total = snap.sys_metrics.ram_total_mb;
        char buf[40];
        snprintf(buf, sizeof(buf), "RAM  %.0f/%.0f", static_cast<double>(used),
                                                      static_cast<double>(total));
        hud_glow_text(dl, {px + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
        const float spw = PW - PAD * 2.f - 80.f;
        sparkline(snap.sys_metrics.ram_history, kSysHistLen, snap.sys_metrics.history_head,
                  {px + PAD + 78.f, cy}, spw, lh,
                  total > 0.f ? total : 4096.f, col_.warn);
        cy += lh + 8.f;
    }

    // ── NETWORK ───────────────────────────────────────────────────────────────
    section("NETWORK");

    // Wi-Fi
    {
        char buf[80];
        if (snap.wifi.connected) {
            snprintf(buf, sizeof(buf), "WiFi: %s", snap.wifi.ssid.c_str());
        } else {
            snprintf(buf, sizeof(buf), "WiFi: --");
        }
        hud_glow_text(dl, {px + PAD, cy}, buf, snap.wifi.connected,
                      col_.glow_base, col_.text_fill);
        cy += lh + 2.f;

        if (snap.wifi.connected) {
            // IP + signal bars
            const int dbm   = snap.wifi.signal_dbm;
            const int bars  = (dbm >= -50) ? 4 : (dbm >= -65) ? 3 : (dbm >= -75) ? 2 : 1;
            snprintf(buf, sizeof(buf), "  %s  %d dBm", snap.wifi.ip.c_str(), dbm);
            hud_glow_text(dl, {px + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
            // Draw signal bars (4 rectangles of increasing height)
            const float bx = px + PW - PAD - 28.f;
            const float by = cy + lh;
            for (int i = 0; i < 4; ++i) {
                const float bh2  = (i + 1) * 4.f;
                const ImU32 bcol = (i < bars) ? col_.ind_good : with_alpha(col_.ind_inactive, 100);
                dl->AddRectFilled({bx + i * 7.f, by - bh2}, {bx + i * 7.f + 5.f, by}, bcol);
            }
            cy += lh + 4.f;
        } else {
            cy += 4.f;
        }
    }

    // Ping
    {
        char buf[80];
        if (snap.ping.reachable) {
            snprintf(buf, sizeof(buf), "Ping  %.0fms  %s",
                     static_cast<double>(snap.ping.latency_ms),
                     snap.ping.host.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Ping  --  %s", snap.ping.host.c_str());
        }
        hud_glow_text(dl, {px + PAD, cy}, buf, snap.ping.reachable,
                      col_.glow_base, col_.text_fill);
        const float spw = PW - PAD * 2.f - 80.f;
        // Scale: 500 ms max
        sparkline(snap.ping.history, kPingHistLen, snap.ping.history_head,
                  {px + PAD + 78.f, cy}, spw, lh,
                  500.f, snap.ping.reachable ? col_.primary : with_alpha(col_.primary, 80));
        cy += lh + 8.f;
    }

    // ── BLUETOOTH ─────────────────────────────────────────────────────────────
    section("BLUETOOTH");

    if (snap.bt_devices.empty()) {
        hud_glow_text(dl, {px + PAD, cy}, "No devices", false, col_.glow_base, col_.text_dim);
        cy += lh + 4.f;
    } else {
        constexpr int MAX_BT = 4;
        int shown = 0;
        for (const auto& dev : snap.bt_devices) {
            if (shown >= MAX_BT) break;
            const ImU32 dot_col = dev.connected ? col_.ind_good : col_.ind_inactive;
            dl->AddCircleFilled({px + PAD + 5.f, cy + lh * 0.5f}, 4.f, dot_col);
            char buf[64];
            snprintf(buf, sizeof(buf), "  %s", dev.name.c_str());
            hud_glow_text(dl, {px + PAD + 4.f, cy}, buf, dev.connected,
                          col_.glow_base, col_.text_fill);
            cy += lh + 2.f;
            ++shown;
        }
        cy += 4.f;
    }

    // ── PERFORMANCE ───────────────────────────────────────────────────────────
    section("PERFORMANCE");

    // FPS + frame time sparkline
    {
        const float fps = snap.sys_metrics.fps_avg;
        const float ft  = snap.sys_metrics.frame_time_ms;
        char buf[48];
        if (fps > 0.f)
            snprintf(buf, sizeof(buf), "FPS  %4.1f", static_cast<double>(fps));
        else
            snprintf(buf, sizeof(buf), "FPS  --");
        hud_glow_text(dl, {px + PAD, cy}, buf, fps > 0.f, col_.glow_base, col_.text_fill);
        const float spw = PW - PAD * 2.f - 80.f;
        // Sparkline: frame time in ms, capped at 50ms (20 FPS floor)
        sparkline(snap.sys_metrics.ft_history, kSysHistLen, snap.sys_metrics.ft_history_head,
                  {px + PAD + 78.f, cy}, spw, lh, 50.f, col_.primary);
        cy += lh + 4.f;

        if (ft > 0.f)
            snprintf(buf, sizeof(buf), "Frame  %.1fms", static_cast<double>(ft));
        else
            snprintf(buf, sizeof(buf), "Frame  --");
        hud_glow_text(dl, {px + PAD, cy}, buf, false, col_.glow_base, col_.text_dim);
        cy += lh + 8.f;
    }

    // ── SERIAL ────────────────────────────────────────────────────────────────
    section("SERIAL LATENCY");

    // Teensy RTT
    {
        const float rtt = snap.serial_metrics.teensy_rtt_ms;
        const bool ok   = snap.health.teensy_ok;
        char buf[40];
        if (rtt >= 0.f)
            snprintf(buf, sizeof(buf), "  Teensy   %.0fms", static_cast<double>(rtt));
        else
            snprintf(buf, sizeof(buf), "  Teensy   --");
        dl->AddCircleFilled({px + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            ok ? col_.ind_good : col_.ind_fail);
        hud_glow_text(dl, {px + PAD + 4.f, cy}, buf, ok, col_.glow_base, col_.text_fill);
        cy += lh + 4.f;
    }

    // SmartKnob event age
    {
        const float age = snap.serial_metrics.knob_event_age_ms;
        const bool  ok  = snap.health.knob_ok;
        char buf[40];
        if (age >= 0.f)
            snprintf(buf, sizeof(buf), "  Knob   <%.0fms ago", static_cast<double>(age));
        else
            snprintf(buf, sizeof(buf), "  Knob   --");
        dl->AddCircleFilled({px + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            ok ? col_.ind_good : col_.ind_inactive);
        hud_glow_text(dl, {px + PAD + 4.f, cy}, buf, ok, col_.glow_base, col_.text_fill);
        cy += lh + 4.f;
    }

    // LoRa RSSI / SNR
    {
        const bool ok = snap.health.lora_ok;
        char buf[48];
        if (ok)
            snprintf(buf, sizeof(buf), "  LoRa   %ddBm  %.1fdB",
                     static_cast<int>(snap.serial_metrics.lora_rssi),
                     static_cast<double>(snap.serial_metrics.lora_snr));
        else
            snprintf(buf, sizeof(buf), "  LoRa   --");
        dl->AddCircleFilled({px + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            ok ? col_.ind_good : col_.ind_inactive);
        hud_glow_text(dl, {px + PAD + 4.f, cy}, buf, ok, col_.glow_base, col_.text_fill);
        cy += lh + 8.f;
    }

    // ── SSH ───────────────────────────────────────────────────────────────────
    {
        dl->AddLine({px + PAD, cy}, {px + PW - PAD, cy}, with_alpha(col_.primary, 80), 1.f);
        cy += 5.f;
        char buf[32];
        if (snap.ssh.active) {
            snprintf(buf, sizeof(buf), "SSH  Active :%d", snap.ssh.port);
            dl->AddCircleFilled({px + PAD + 5.f, cy + lh * 0.5f}, 4.f, col_.ind_good);
        } else {
            snprintf(buf, sizeof(buf), "SSH  Inactive");
            dl->AddCircleFilled({px + PAD + 5.f, cy + lh * 0.5f}, 4.f, col_.ind_fail);
        }
        hud_glow_text(dl, {px + PAD + 4.f, cy}, buf, snap.ssh.active,
                      col_.glow_base, col_.text_fill);
    }

    if (font_mono_) ImGui::PopFont();

    ImGui::End();
}


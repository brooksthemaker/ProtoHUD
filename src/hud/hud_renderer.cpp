#include "hud_renderer.h"

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

// Replace the alpha byte of an ImU32 color (format: ABGR, alpha in high byte).
static ImU32 with_alpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24u);
}

// Frame-scope glow flag — set each frame in begin_frame() from cfg_.glow_enabled.
static bool s_glow = true;

// Draw text with an orange glow outline matching the compass tick style.
// selected=true → full white + bright glow; false → dim white + faint glow.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           bool selected = true) {
    constexpr ImU32 GLOW_ON  = IM_COL32(255, 160, 32,  72);
    constexpr ImU32 GLOW_OFF = IM_COL32(255, 160, 32,  22);
    constexpr ImU32 FILL_ON  = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 FILL_OFF = IM_COL32(255, 255, 255, 160);
    const ImU32 fill = selected ? FILL_ON  : FILL_OFF;
    if (s_glow) {
        const ImU32 glow = selected ? GLOW_ON : GLOW_OFF;
        constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
        for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    }
    dl->AddText(pos, fill, text);
}

// Color-parameterized variant: glow_col and fill_col are full-brightness (alpha=255);
// glow alphas are derived internally.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           bool selected, ImU32 glow_col, ImU32 fill_col) {
    const ImU32 fill = selected ? fill_col : with_alpha(fill_col, 160);
    if (s_glow) {
        const ImU32 glow = selected ? with_alpha(glow_col, 72) : with_alpha(glow_col, 22);
        constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
        for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    }
    dl->AddText(pos, fill, text);
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

void HudRenderer::begin_frame(float /*dt*/) {
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().FontGlobalScale = cfg_.text_scale;
    s_glow = cfg_.glow_enabled;
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

    const float fw    = static_cast<float>(w);
    const float fh    = static_cast<float>(h);
    const float th    = static_cast<float>(cfg_.top_bar_height);
    const float ch    = static_cast<float>(cfg_.compass_height);
    const float mid_h = fh - th;

    draw_top_bar(dl, s, fw);

    if (!s.lora_messages.empty()) {
        float pw    = static_cast<float>(cfg_.panel_width);
        float msg_w = std::min(pw, fw / 3.f);
        draw_lora_messages(dl, s, { 0.f, th }, msg_w, mid_h);
    }

    const float cw       = fw / 3.f;
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    // Compass drawn first; indicator arms and health sides render on top.
    draw_compass_tape    (dl, s, {fw / 2.f - cw / 2.f, fh - ch - c_margin}, cw, ch);
    draw_face_indicator  (dl, s.face, fw, fh);
    draw_lora_indicator  (dl, s,      fw, fh);
    draw_health_side(dl, s.health, fw, fh, false,
                     s.focus_left, s.focus_right, s.night_vision.nv_enabled);
    draw_health_side(dl, s.health, fw, fh, true,
                     s.focus_left, s.focus_right, s.night_vision.nv_enabled);
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
        case OverlayConfig::Anchor::TOP_CENTER:    return { (sw - ov_w) * 0.5f,     kEdge    };
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
    dl->AddPolyline(pts, n_pts, col_.primary, ImDrawFlags_Closed, 2.f);

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

// ── Top bar ───────────────────────────────────────────────────────────────────

void HudRenderer::draw_top_bar(ImDrawList* dl, const AppState& s, float w) {
    float th = static_cast<float>(cfg_.top_bar_height);

    dl->AddRectFilled({0, 0}, {w, th}, col_.background);

    // Unread messages badge
    int unread = s.unread_message_count();
    if (unread > 0) {
        char buf[32]; snprintf(buf, sizeof(buf), "MSG:%d", unread);
        dl->AddText({w / 2.f + 10.f, 10.f}, col_.warn, buf);
    }

    // Audio strip (right side)
    draw_audio_strip(dl, s.audio, {w - 180.f, 4.f}, 170.f);
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
    const float anchor_y = fh - c_margin;
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

    struct Ind { const char* label; bool ok; };
    const Ind left_items[]  = {{"Proot",     h.teensy_ok},
                                {"LoRa",      h.lora_ok},
                                {"Interface", h.knob_ok},
                                {"Audio",     h.audio_ok}};
    const Ind right_items[] = {{lcam_lbl,    h.cam_owl_left},
                                {rcam_lbl,    h.cam_owl_right},
                                {"Cam 1",     h.cam_usb1},
                                {"Cam 2",     h.cam_usb2}};
    const Ind* items   = right_side ? right_items : left_items;
    const int  n_items = 4;

    constexpr float ROW_H  = 18.f;
    constexpr float DOT_R  = 4.f;
    constexpr float ANGLE  = 130.f * 3.14159265f / 180.f;
    constexpr float H_LEN  = 300.f;

    const float dir_x    = std::cos(ANGLE) * (right_side ? 1.f : -1.f);
    const float dir_y    = -std::sin(ANGLE);
    const float diag_len = static_cast<float>(n_items + 1) * ROW_H;

    // Parallelogram background — 16 strips fading from opaque (inner) to transparent (outer).
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
                {anchor_x + outer_sign * t0 * H_LEN,                    anchor_y},
                {anchor_x + outer_sign * t0 * H_LEN + dir_x * diag_len, anchor_y + dir_y * diag_len},
                {anchor_x + outer_sign * t1 * H_LEN + dir_x * diag_len, anchor_y + dir_y * diag_len},
                {anchor_x + outer_sign * t1 * H_LEN,                    anchor_y},
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

        if (items[i].ok) {
            dl->AddCircleFilled({ix, iy}, DOT_R + 2.f, with_alpha(col_.ind_good, 28));
            dl->AddCircleFilled({ix, iy}, DOT_R,        col_.ind_good);
        } else {
            dl->AddCircleFilled({ix, iy}, DOT_R, col_.ind_fail);
        }

        const char* lbl = items[i].label;
        if (right_side) {
            hud_glow_text(dl, {ix + DOT_R + 6.f, iy - 7.f}, lbl, items[i].ok,
                          col_.text_fill, col_.text_fill);
        } else {
            float tw = ImGui::CalcTextSize(lbl).x;
            hud_glow_text(dl, {ix - DOT_R - 6.f - tw, iy - 7.f}, lbl, items[i].ok,
                          col_.text_fill, col_.text_fill);
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
// Three parallel diagonal arms at 130°: [proto arm] [separator] [health indicators]
// SEG_W=150 puts proto at anchor_x-300 (= end of health side's 300px horiz line).

void HudRenderer::draw_face_indicator(ImDrawList* dl, const FaceState& f,
                                       float fw, float fh) {
    constexpr float ROW_H   = 18.f;
    constexpr float DOT_R   = 4.f;
    constexpr float SEG_W   = 150.f;
    constexpr float ARM_EXT = 140.f;
    constexpr float ANGLE   = 130.f * 3.14159265f / 180.f;
    constexpr int   N_ITEMS = 5;

    const float tape_w        = fw / 3.f;
    const float tape_x        = fw / 2.f - tape_w / 2.f;
    const float fade_w        = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin      = static_cast<float>(cfg_.compass_bottom_margin);
    const float anchor_y      = fh - c_margin;
    const float ind_anchor_x  = tape_x - fade_w;
    const float sep_anchor_x  = ind_anchor_x - SEG_W;
    const float proto_anchor_x = ind_anchor_x - SEG_W * 2.f;

    const float dir_x    = std::cos(ANGLE) * -1.f;   // left side: goes right
    const float dir_y    = -std::sin(ANGLE);           // goes up
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

    // Master separator diagonal (glow only, no dots, no horizontal)
    const ImVec2 sep_end = {sep_anchor_x + dir_x * diag_len,
                             anchor_y     + dir_y * diag_len};
    dl->AddLine({sep_anchor_x, anchor_y}, sep_end, COL_GLW2, 5.f);
    dl->AddLine({sep_anchor_x, anchor_y}, sep_end, COL_GLW1, 2.5f);
    dl->AddLine({sep_anchor_x, anchor_y}, sep_end, COL_MAJ,  1.f);

    // Build items
    char effect_lbl[24], mode_lbl[24], rgb_lbl[24], brt_lbl[24];
    snprintf(effect_lbl, sizeof(effect_lbl), "%s", effect_name(f.effect_id));
    if (f.playing_gif)
        snprintf(mode_lbl, sizeof(mode_lbl), "GIF #%d", f.gif_id);
    else
        snprintf(mode_lbl, sizeof(mode_lbl), "Pal #%d", f.palette_id);
    snprintf(rgb_lbl, sizeof(rgb_lbl), "R%d G%d B%d", f.r, f.g, f.b);
    snprintf(brt_lbl, sizeof(brt_lbl), "Brt %d%%", (f.brightness * 100) / 255);

    struct Ind { const char* label; bool ok; };
    const Ind items[N_ITEMS] = {
        {"FACE",      f.connected},
        {effect_lbl,  f.connected},
        {mode_lbl,    f.connected},
        {rgb_lbl,     f.connected},
        {brt_lbl,     f.connected},
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
// Three parallel diagonal arms at 130°: [health indicators] [separator] [lora arm]
// SEG_W=150 puts lora at anchor_x+300 (= end of health side's 300px horiz line).

void HudRenderer::draw_lora_indicator(ImDrawList* dl, const AppState& s,
                                       float fw, float fh) {
    constexpr float ROW_H    = 18.f;
    constexpr float DOT_R    = 4.f;
    constexpr float SEG_W    = 150.f;
    constexpr float ARM_EXT  = 140.f;
    constexpr float ANGLE    = 130.f * 3.14159265f / 180.f;
    constexpr int   MAX_ROWS = 4;   // 1 header + up to 3 nodes

    const float tape_w       = fw / 3.f;
    const float tape_x       = fw / 2.f - tape_w / 2.f;
    const float fade_w       = static_cast<float>(cfg_.compass_bg_side_fade);
    const float c_margin     = static_cast<float>(cfg_.compass_bottom_margin);
    const float anchor_y     = fh - c_margin;
    const float ind_anchor_x = tape_x + tape_w + fade_w;
    const float sep_anchor_x = ind_anchor_x + SEG_W;
    const float lora_anchor_x = ind_anchor_x + SEG_W * 2.f;

    const float dir_x    = std::cos(ANGLE) * 1.f;   // right side: goes left
    const float dir_y    = -std::sin(ANGLE);          // goes up
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

    // Master separator diagonal (glow only, no dots, no horizontal)
    const ImVec2 sep_end = {sep_anchor_x + dir_x * diag_len,
                             anchor_y     + dir_y * diag_len};
    dl->AddLine({sep_anchor_x, anchor_y}, sep_end, COL_GLW2, 5.f);
    dl->AddLine({sep_anchor_x, anchor_y}, sep_end, COL_GLW1, 2.5f);
    dl->AddLine({sep_anchor_x, anchor_y}, sep_end, COL_MAJ,  1.f);

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

    const float fw = static_cast<float>(cfg_.compass_bg_side_fade);
    if (s.compass_bg_enabled) {
        const uint8_t a = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        const ImU32   A = with_alpha(col_.compass_bg_color, a);

        // Diagonal inset at the top edge to match the health indicator 130° angle.
        // Both sides taper inward as height increases, connecting flush to the
        // health indicator diagonal lines when those are also enabled.
        constexpr float kIndAngle = 130.f * 3.14159265f / 180.f;
        const float inset = std::abs(std::cos(kIndAngle)) / std::sin(kIndAngle) * th;

        // Solid trapezoid — wider at bottom (anchor_y), narrower at top.
        ImVec2 bg[4] = {
            {origin.x - fw + inset,      origin.y     },  // TL
            {origin.x + tw + fw - inset, origin.y     },  // TR
            {origin.x + tw + fw,         origin.y + th},  // BR
            {origin.x - fw,              origin.y + th},  // BL
        };
        dl->AddConvexPolyFilled(bg, 4, A);
    }

    // Glow line at the tape base — color matches the health indicator lines (glow_base).
    {
        const float lx0 = origin.x - fw, lx1 = origin.x + tw + fw;
        const float line_y = origin.y + th;
        dl->AddLine({lx0, line_y}, {lx1, line_y}, with_alpha(col_.glow_base, 28), 5.f);
        dl->AddLine({lx0, line_y}, {lx1, line_y}, with_alpha(col_.glow_base, 70), 2.5f);
        dl->AddLine({lx0, line_y}, {lx1, line_y}, col_.glow_base, 1.f);
    }

    // Tick lengths — major is user-configurable; mid and minor scale proportionally.
    const float t_maj = static_cast<float>(cfg_.compass_tick_length);
    const float t_mid = t_maj * (16.f / 24.f);
    const float t_min = t_maj * (10.f / 24.f);

    // Tick zone: leave 20 px at the bottom for labels.
    const float tick_bottom = origin.y + th - 20.f;
    const float label_y     = tick_bottom + 3.f;

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
                dl->AddLine({px, tick_bottom - t_maj}, {px, tick_bottom}, col_glow2, t_maj * 0.5f);
                dl->AddLine({px, tick_bottom - t_maj}, {px, tick_bottom}, col_glow1, t_maj * 0.25f);
            }
            dl->AddLine({px, tick_bottom - t_maj}, {px, tick_bottom}, col_major, 3.f);
            const char* card = cardinal_str(static_cast<float>(deg));
            ImVec2 csz = ImGui::CalcTextSize(card);
            hud_glow_text(dl, {px - csz.x * 0.5f, label_y},
                          card, true, col_.glow_base, col_.text_fill);
        } else if (deg % 10 == 0) {
            if (tick_glow)
                dl->AddLine({px, tick_bottom - t_mid}, {px, tick_bottom}, col_glow2, t_mid * 0.375f);
            dl->AddLine({px, tick_bottom - t_mid}, {px, tick_bottom}, col_mid,   2.f);
            char buf[8]; snprintf(buf, sizeof(buf), "%d", deg);
            ImVec2 bsz = ImGui::CalcTextSize(buf);
            hud_glow_text(dl, {px - bsz.x * 0.5f, label_y}, buf,
                          true, col_.glow_base, col_.text_fill);
        } else if (deg % 5 == 0) {
            if (tick_glow)
                dl->AddLine({px, tick_bottom - t_min}, {px, tick_bottom}, col_glow2, t_min * 0.4f);
            dl->AddLine({px, tick_bottom - t_min}, {px, tick_bottom}, col_minor, 2.f);
        }
    }

    if (font_mono_) ImGui::PopFont();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

const char* HudRenderer::cardinal_str(float deg) {
    static const char* pts[] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = static_cast<int>((deg + 22.5f) / 45.f) % 8;
    return pts[idx];
}

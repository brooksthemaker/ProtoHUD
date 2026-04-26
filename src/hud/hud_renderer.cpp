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

// Draw text with an orange glow outline matching the compass tick style.
// selected=true → full white + bright glow; false → dim white + faint glow.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           bool selected = true) {
    constexpr ImU32 GLOW_ON  = IM_COL32(255, 160, 32,  72);
    constexpr ImU32 GLOW_OFF = IM_COL32(255, 160, 32,  22);
    constexpr ImU32 FILL_ON  = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 FILL_OFF = IM_COL32(255, 255, 255, 160);
    const ImU32 glow = selected ? GLOW_ON  : GLOW_OFF;
    const ImU32 fill = selected ? FILL_ON  : FILL_OFF;
    constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
    for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    dl->AddText(pos, fill, text);
}

// Color-parameterized variant: glow_col and fill_col are full-brightness (alpha=255);
// glow alphas are derived internally.
static void hud_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                           bool selected, ImU32 glow_col, ImU32 fill_col) {
    const ImU32 glow = selected ? with_alpha(glow_col, 72) : with_alpha(glow_col, 22);
    const ImU32 fill = selected ? fill_col : with_alpha(fill_col, 160);
    constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
    for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
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

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float th = static_cast<float>(cfg_.top_bar_height);
    const float ch = static_cast<float>(cfg_.compass_height);
    const float pw = static_cast<float>(cfg_.panel_width);
    const float mid_h = fh - th; // compass is center-third only; panels use full height

    draw_top_bar     (dl, s, fw);
    draw_face_panel  (dl, s.face,       { fw - pw, th },  pw, mid_h * 0.5f);
    draw_lora_panel  (dl, s,            { fw - pw, th + mid_h * 0.5f }, pw, mid_h * 0.5f);

    if (!s.lora_messages.empty()) {
        float msg_w = std::min(pw, fw / 3.f);
        draw_lora_messages(dl, s,       { 0.f, th }, msg_w, mid_h);
    }

    draw_health_side(dl, s.health, fw, fh, false);
    draw_health_side(dl, s.health, fw, fh, true);

    const float cw      = fw / 3.f;
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    draw_compass_tape(dl, s, {fw / 2.f - cw / 2.f, fh - ch - c_margin}, cw, ch);
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
                            int w, int h, bool active, const OverlayConfig& cfg) {
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

    // 4. Label
    if (font_mono_) ImGui::PushFont(font_mono_);
    dl->AddText({x + 4.f, y + 4.f}, col_.primary, label);
    if (font_mono_) ImGui::PopFont();

    // 5. Chamfered border outline on top
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
                                    float fw, float fh, bool right_side) {
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

    struct Ind { const char* label; bool ok; };
    const Ind left_items[]  = {{"Proot",     h.teensy_ok},
                                {"LoRa",      h.lora_ok},
                                {"Interface", h.knob_ok},
                                {"Audio",     h.audio_ok}};
    const Ind right_items[] = {{"Left Cam",  h.cam_owl_left},
                                {"Right Cam", h.cam_owl_right},
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

    // Optional parallelogram background (same color/opacity as compass bg)
    if (cfg_.indicator_bg_enabled) {
        const uint8_t bg_a  = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        const ImU32   bg_col = with_alpha(col_.compass_bg_color, bg_a);
        ImVec2 para[4];
        if (right_side) {
            para[0] = {anchor_x,                              anchor_y};
            para[1] = {anchor_x + H_LEN,                      anchor_y};
            para[2] = {anchor_x + H_LEN + dir_x * diag_len,   anchor_y + dir_y * diag_len};
            para[3] = {anchor_x + dir_x * diag_len,            anchor_y + dir_y * diag_len};
        } else {
            para[0] = {anchor_x,                              anchor_y};
            para[1] = {anchor_x + dir_x * diag_len,           anchor_y + dir_y * diag_len};
            para[2] = {anchor_x - H_LEN + dir_x * diag_len,   anchor_y + dir_y * diag_len};
            para[3] = {anchor_x - H_LEN,                      anchor_y};
        }
        dl->AddConvexPolyFilled(para, 4, bg_col);
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
                          col_.glow_base, col_.text_fill);
        } else {
            float tw = ImGui::CalcTextSize(lbl).x;
            hud_glow_text(dl, {ix - DOT_R - 6.f - tw, iy - 7.f}, lbl, items[i].ok,
                          col_.glow_base, col_.text_fill);
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

// ── Face panel ────────────────────────────────────────────────────────────────

void HudRenderer::draw_face_panel(ImDrawList* dl, const FaceState& f,
                                   ImVec2 origin, float pw, float ph) {
    dl->AddLine({origin.x, origin.y}, {origin.x, origin.y + ph}, col_.orange);
    dl->AddLine({origin.x, origin.y}, {origin.x + pw, origin.y},
                IM_COL32(255, 160, 32, 60));

    if (font_ui_) ImGui::PushFont(font_ui_);
    hud_glow_text(dl, {origin.x + 4.f, origin.y + 6.f}, "FACE");

    float py = origin.y + 28.f;
    const float lh = 22.f;

    if (f.connected)
        hud_glow_text(dl, {origin.x + 4.f, py}, "Connected");
    else
        dl->AddText({origin.x + 4.f, py}, col_.danger, "Offline");
    py += lh;

    char buf[64];
    snprintf(buf, sizeof(buf), "Effect: %s", effect_name(f.effect_id));
    hud_glow_text(dl, {origin.x + 4.f, py}, buf, false);
    py += lh;

    if (f.playing_gif) {
        snprintf(buf, sizeof(buf), "GIF: #%d", f.gif_id);
    } else {
        snprintf(buf, sizeof(buf), "Palette: #%d", f.palette_id);
    }
    hud_glow_text(dl, {origin.x + 4.f, py}, buf, false);
    py += lh;

    // Color swatch
    ImU32 swatch = IM_COL32(f.r, f.g, f.b, 255);
    dl->AddRectFilled({origin.x + 4.f, py}, {origin.x + 28.f, py + 16.f}, swatch);
    dl->AddRect      ({origin.x + 4.f, py}, {origin.x + 28.f, py + 16.f},
                      IM_COL32(255, 160, 32, 120));
    snprintf(buf, sizeof(buf), " R%d G%d B%d", f.r, f.g, f.b);
    hud_glow_text(dl, {origin.x + 30.f, py + 1.f}, buf, false);
    py += lh;

    snprintf(buf, sizeof(buf), "Brt: %d%%", (f.brightness * 100) / 255);
    hud_glow_text(dl, {origin.x + 4.f, py}, buf, false);
    if (font_ui_) ImGui::PopFont();
}

// ── LoRa nodes panel ──────────────────────────────────────────────────────────

void HudRenderer::draw_lora_panel(ImDrawList* dl, const AppState& s,
                                   ImVec2 origin, float pw, float ph) {
    dl->AddLine({origin.x, origin.y}, {origin.x, origin.y + ph}, col_.orange);
    dl->AddLine({origin.x, origin.y}, {origin.x + pw, origin.y},
                IM_COL32(255, 160, 32, 60));

    if (font_ui_) ImGui::PushFont(font_ui_);
    hud_glow_text(dl, {origin.x + 4.f, origin.y + 6.f}, "LORA NODES");

    float py = origin.y + 28.f;
    time_t now = std::time(nullptr);

    for (const auto& node : s.lora_nodes) {
        if (py + 44.f > origin.y + ph) break;
        double age = difftime(now, node.last_seen);
        bool fresh = (age < 30.0);

        char buf[96];
        if (!node.name.empty()) {
            snprintf(buf, sizeof(buf), "%-10s %03.0f\xc2\xb0 %.1fkm",
                     node.name.substr(0, 10).c_str(),
                     node.heading_deg, node.distance_m / 1000.f);
        } else {
            snprintf(buf, sizeof(buf), "ID:%08X  %03.0f\xc2\xb0 %.1fkm",
                     node.node_id, node.heading_deg, node.distance_m / 1000.f);
        }
        hud_glow_text(dl, {origin.x + 4.f, py}, buf, fresh);
        if (!fresh && age < 120.0)  // stale but not lost: warn tint
            dl->AddText({origin.x + 4.f, py}, col_.warn, buf);
        py += 18.f;

        snprintf(buf, sizeof(buf), "  RSSI:%d SNR:%d %ds",
                 node.rssi, node.snr,
                 node.last_seen > 0 ? static_cast<int>(age) : -1);
        hud_glow_text(dl, {origin.x + 4.f, py}, buf, false);
        py += 22.f;
    }

    if (s.lora_nodes.empty())
        hud_glow_text(dl, {origin.x + 4.f, py}, "No nodes tracked", false);
    if (font_ui_) ImGui::PopFont();
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
    const float tick_y   = origin.y + th - 8.f;

    const ImU32 col_major = col_.compass_tick;
    const ImU32 col_mid   = with_alpha(col_.compass_tick, 180);
    const ImU32 col_minor = with_alpha(col_.compass_tick, 110);
    const ImU32 col_glow1 = with_alpha(col_.compass_glow, 70);
    const ImU32 col_glow2 = with_alpha(col_.compass_glow, 28);

    if (s.compass_bg_enabled) {
        const uint8_t a  = static_cast<uint8_t>(cfg_.compass_bg_opacity * 255.f);
        const float   fw = static_cast<float>(cfg_.compass_bg_side_fade);
        const ImU32   T  = with_alpha(col_.compass_bg_color, 0);
        const ImU32   A  = with_alpha(col_.compass_bg_color, a);
        // Left strip: fully transparent outer edge, opaque only at inner-bottom corner
        dl->AddRectFilledMultiColor(
            {origin.x - fw, origin.y}, {origin.x,       origin.y + th}, T, T, A, T);
        // Center: transparent top, opaque bottom
        dl->AddRectFilledMultiColor(
            {origin.x,      origin.y}, {origin.x + tw,  origin.y + th}, T, T, A, A);
        // Right strip: opaque only at inner-bottom corner, fully transparent outer edge
        dl->AddRectFilledMultiColor(
            {origin.x + tw, origin.y}, {origin.x + tw + fw, origin.y + th}, T, T, T, A);

        // Extend background below tape, fading back to transparent
        constexpr float LINE_GAP = 6.f;
        constexpr float LINE_EXT = 8.f;
        const float bot  = origin.y + th;
        const float bot2 = bot + LINE_GAP + LINE_EXT;
        dl->AddRectFilledMultiColor({origin.x - fw, bot}, {origin.x,        bot2}, T, A, T, T);
        dl->AddRectFilledMultiColor({origin.x,      bot}, {origin.x + tw,   bot2}, A, A, T, T);
        dl->AddRectFilledMultiColor({origin.x + tw, bot}, {origin.x+tw+fw,  bot2}, A, T, T, T);

        // Glowing horizontal line below tape (same 3-pass glow as major ticks)
        const float line_y = bot + LINE_GAP;
        const float lx0 = origin.x - fw, lx1 = origin.x + tw + fw;
        dl->AddLine({lx0, line_y}, {lx1, line_y}, col_glow2, 5.f);
        dl->AddLine({lx0, line_y}, {lx1, line_y}, col_glow1, 2.5f);
        dl->AddLine({lx0, line_y}, {lx1, line_y}, col_major, 1.f);
    }

    if (font_mono_) ImGui::PushFont(font_mono_);

    // Pass 1 — all tick lines so cardinal labels always render on top
    for (int deg = 0; deg < 360; deg++) {
        float offset = deg - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;

        float px = center_x + offset * ppd;
        if (px < origin.x || px > origin.x + tw) continue;

        if (deg % 45 == 0) {
            dl->AddLine({px, origin.y}, {px, tick_y}, col_glow2, 9.f);
            dl->AddLine({px, origin.y}, {px, tick_y}, col_glow1, 4.f);
            dl->AddLine({px, origin.y}, {px, tick_y}, col_major, 1.5f);
        } else if (deg % 10 == 0) {
            dl->AddLine({px, tick_y - 12.f}, {px, tick_y}, col_mid);
            char buf[8]; snprintf(buf, sizeof(buf), "%d", deg);
            dl->AddText({px - 8.f, tick_y - 28.f}, col_major, buf);
        } else if (deg % 5 == 0) {
            dl->AddLine({px, tick_y - 8.f}, {px, tick_y}, col_minor);
        }
    }

    // Pass 2 — cardinal labels float above the tick area
    for (int deg = 0; deg < 360; deg += 45) {
        float offset = deg - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;

        float px = center_x + offset * ppd;
        if (px < origin.x || px > origin.x + tw) continue;

        dl->AddText({px - 8.f, origin.y - 16.f}, col_major,
                    cardinal_str(static_cast<float>(deg)));
    }

    // Centre cursor triangle — glow halo then sharp fill
    dl->AddTriangleFilled(
        {center_x,        origin.y       },
        {center_x - 12.f, origin.y + 18.f},
        {center_x + 12.f, origin.y + 18.f},
        col_glow2);
    dl->AddTriangleFilled(
        {center_x,       origin.y       },
        {center_x - 8.f, origin.y + 14.f},
        {center_x + 8.f, origin.y + 14.f},
        col_major);

    // Heading readout
    char hdg[16]; snprintf(hdg, sizeof(hdg), "%03.0f°", heading);
    dl->AddText({center_x - 16.f, origin.y + 16.f}, col_major, hdg);

    if (font_mono_) ImGui::PopFont();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

const char* HudRenderer::cardinal_str(float deg) {
    static const char* pts[] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = static_cast<int>((deg + 22.5f) / 45.f) % 8;
    return pts[idx];
}

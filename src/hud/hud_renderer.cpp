#include "hud_renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <cmath>
#include <ctime>
#include <cstring>
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

    draw_health_dots(dl, s.health, {10.f, th + 8.f});

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

void HudRenderer::draw_pip(unsigned int tex_usb1, unsigned int tex_usb2,
                            int w, int h, bool active, const OverlayConfig& cfg) {
    if (!active || (!tex_usb1 && !tex_usb2)) return;

    ImGui::SetCurrentContext(ctx_);
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const float sw     = static_cast<float>(w);
    const float sh     = static_cast<float>(h);
    const float ov_h   = sh * cfg.size;
    const float ov_w   = ov_h * (16.f / 9.f);   // USB cameras are 16:9
    const float margin = static_cast<float>(cfg_.compass_height);
    const auto  pos    = overlay_origin(cfg, sw, sh, ov_w, ov_h, margin);

    // Border + background
    dl->AddRectFilled({ pos.x - 2.f, pos.y - 2.f },
                      { pos.x + ov_w + 2.f, pos.y + ov_h + 2.f },
                      col_.primary);
    dl->AddRectFilled({ pos.x, pos.y },
                      { pos.x + ov_w, pos.y + ov_h },
                      col_.background);

    // Label (top-left corner of the box)
    if (font_mono_) ImGui::PushFont(font_mono_);
    dl->AddText({ pos.x + 4.f, pos.y + 4.f }, col_.primary, "USB CAM");
    if (font_mono_) ImGui::PopFont();

    // Camera image
    GLuint tex = tex_usb1 ? tex_usb1 : tex_usb2;
    dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                 { pos.x, pos.y }, { pos.x + ov_w, pos.y + ov_h });
}

// ── Android mirror overlay ────────────────────────────────────────────────────

void HudRenderer::draw_android_overlay(unsigned int tex, int w, int h,
                                        bool active, bool connecting,
                                        const OverlayConfig& cfg) {
    if (!active) return;

    ImGui::SetCurrentContext(ctx_);
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const float sw     = static_cast<float>(w);
    const float sh     = static_cast<float>(h);
    const float ov_h   = sh * cfg.size;
    const float ov_w   = ov_h * (9.f / 16.f);   // phone portrait aspect
    const float margin = static_cast<float>(cfg_.compass_height);
    const auto  pos    = overlay_origin(cfg, sw, sh, ov_w, ov_h, margin);

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

// ── Health dots ───────────────────────────────────────────────────────────────

void HudRenderer::draw_health_dots(ImDrawList* dl,
                                    const SystemHealth& h, ImVec2 origin) {
    struct Ind { const char* label; bool ok; };
    const Ind items[] = {
        {"Proot",     h.teensy_ok},
        {"LoRa",      h.lora_ok},
        {"Interface", h.knob_ok},
        {"Left Cam",  h.cam_owl_left},
        {"Right Cam", h.cam_owl_right},
        {"Cam 1",     h.cam_usb1},
        {"Cam 2",     h.cam_usb2},
        {"Audio",     h.audio_ok},
        {"Android",   h.android_mirror},
    };

    constexpr float row_h   = 20.f;
    constexpr float dot_r   = 4.f;
    constexpr float dot_cx  = 6.f;
    constexpr float text_x  = 16.f;
    constexpr int   n_items = 9;
    constexpr float panel_w = 108.f;
    constexpr float panel_h = row_h * n_items + 8.f;

    const uint8_t alpha = static_cast<uint8_t>(cfg_.health_panel_opacity * 255.f);
    dl->AddRectFilled(
        {origin.x - 4.f, origin.y - 4.f},
        {origin.x + panel_w, origin.y + panel_h},
        IM_COL32(8, 12, 18, alpha));

    if (font_mono_) ImGui::PushFont(font_mono_);
    float y = origin.y;
    for (const auto& item : items) {
        ImU32 dot_col  = item.ok ? col_.primary : col_.danger;
        ImU32 text_col = item.ok ? col_.text    : col_.text_dim;
        dl->AddCircleFilled({origin.x + dot_cx, y + row_h * 0.5f}, dot_r, dot_col);
        dl->AddText({origin.x + text_x, y + (row_h - 14.f) * 0.5f}, text_col, item.label);
        y += row_h;
    }
    if (font_mono_) ImGui::PopFont();
}

// ── Audio strip ───────────────────────────────────────────────────────────────

void HudRenderer::draw_audio_strip(ImDrawList* dl, const AudioState& a,
                                    ImVec2 origin, float w) {
    if (!a.enabled) {
        dl->AddText(origin, col_.text_dim, "AUDIO OFF");
        return;
    }
    ImU32 col = a.device_ok ? col_.accent : col_.warn;

    char buf[64];
    static const char* outputs[] = {"VITURE","JACK","HDMI"};
    const char* out_str = (a.output >= 0 && a.output < 3) ? outputs[a.output] : "?";
    snprintf(buf, sizeof(buf), "AU \xe2\x86\x92 %s  X:%d", out_str, a.xrun_count);
    dl->AddText(origin, col, buf);

    // CPU load bar
    float bar_y = origin.y + 20.f;
    dl->AddRectFilled({origin.x, bar_y}, {origin.x + w, bar_y + 6.f},
                       IM_COL32(30, 40, 45, 200));
    float load_w = w * std::min(1.f, a.cpu_load);
    ImU32 load_col = (a.cpu_load > 0.8f) ? col_.danger :
                     (a.cpu_load > 0.5f) ? col_.warn : col_.primary;
    dl->AddRectFilled({origin.x, bar_y}, {origin.x + load_w, bar_y + 6.f}, load_col);
}

// ── Face panel ────────────────────────────────────────────────────────────────

void HudRenderer::draw_face_panel(ImDrawList* dl, const FaceState& f,
                                   ImVec2 origin, float pw, float ph) {
    // Transparent background — no fill; left border line as visual anchor only
    dl->AddLine({origin.x, origin.y}, {origin.x, origin.y + ph}, col_.primary);
    dl->AddLine({origin.x, origin.y}, {origin.x + pw, origin.y}, col_.separator);

    ImU32 hdr_col = f.connected ? col_.primary : col_.danger;
    if (font_ui_) ImGui::PushFont(font_ui_);
    dl->AddText({origin.x + 4.f, origin.y + 6.f}, hdr_col, "FACE");

    float py = origin.y + 28.f;
    const float lh = 22.f;

    dl->AddText({origin.x + 4.f, py}, f.connected ? col_.accent : col_.danger,
                f.connected ? "Connected" : "Offline");
    py += lh;

    char buf[64];
    snprintf(buf, sizeof(buf), "Effect: %s", effect_name(f.effect_id));
    dl->AddText({origin.x + 4.f, py}, col_.text, buf);
    py += lh;

    if (f.playing_gif) {
        snprintf(buf, sizeof(buf), "GIF: #%d", f.gif_id);
        dl->AddText({origin.x + 4.f, py}, col_.accent, buf);
    } else {
        snprintf(buf, sizeof(buf), "Palette: #%d", f.palette_id);
        dl->AddText({origin.x + 4.f, py}, col_.text, buf);
    }
    py += lh;

    // Color swatch
    ImU32 swatch = IM_COL32(f.r, f.g, f.b, 255);
    dl->AddRectFilled({origin.x + 4.f, py}, {origin.x + 28.f, py + 16.f}, swatch);
    dl->AddRect      ({origin.x + 4.f, py}, {origin.x + 28.f, py + 16.f}, col_.text_dim);
    snprintf(buf, sizeof(buf), " R%d G%d B%d", f.r, f.g, f.b);
    dl->AddText({origin.x + 30.f, py + 1.f}, col_.text, buf);
    py += lh;

    // Brightness as a percentage number — no bar graph
    snprintf(buf, sizeof(buf), "Brt: %d%%", (f.brightness * 100) / 255);
    dl->AddText({origin.x + 4.f, py}, col_.text_dim, buf);
    if (font_ui_) ImGui::PopFont();
}

// ── LoRa nodes panel ──────────────────────────────────────────────────────────

void HudRenderer::draw_lora_panel(ImDrawList* dl, const AppState& s,
                                   ImVec2 origin, float pw, float ph) {
    // Transparent background — no fill; left border line as visual anchor only
    dl->AddLine({origin.x, origin.y}, {origin.x, origin.y + ph}, col_.primary);
    dl->AddLine({origin.x, origin.y}, {origin.x + pw, origin.y}, col_.separator);

    if (font_ui_) ImGui::PushFont(font_ui_);
    dl->AddText({origin.x + 4.f, origin.y + 6.f}, col_.primary, "LORA NODES");

    float py = origin.y + 28.f;
    time_t now = std::time(nullptr);

    for (const auto& node : s.lora_nodes) {
        if (py + 44.f > origin.y + ph) break;
        double age = difftime(now, node.last_seen);
        ImU32 node_col = (age < 30.0) ? col_.accent :
                         (age < 120.0) ? col_.warn : col_.text_dim;

        char buf[96];
        if (!node.name.empty()) {
            snprintf(buf, sizeof(buf), "%-10s %03.0f° %.1fkm",
                     node.name.substr(0, 10).c_str(),
                     node.heading_deg, node.distance_m / 1000.f);
        } else {
            snprintf(buf, sizeof(buf), "ID:%08X  %03.0f° %.1fkm",
                     node.node_id, node.heading_deg, node.distance_m / 1000.f);
        }
        dl->AddText({origin.x + 4.f, py}, node_col, buf);
        py += 18.f;

        snprintf(buf, sizeof(buf), "  RSSI:%d SNR:%d %ds",
                 node.rssi, node.snr,
                 node.last_seen > 0 ? static_cast<int>(age) : -1);
        dl->AddText({origin.x + 4.f, py}, col_.text_dim, buf);
        py += 22.f;
    }

    if (s.lora_nodes.empty()) {
        dl->AddText({origin.x + 4.f, py}, col_.text_dim, "No nodes tracked");
    }
    if (font_ui_) ImGui::PopFont();
}

// ── LoRa messages panel ───────────────────────────────────────────────────────

void HudRenderer::draw_lora_messages(ImDrawList* dl, const AppState& s,
                                      ImVec2 origin, float pw, float ph) {
    dl->AddRectFilled(origin, {origin.x + pw, origin.y + ph},
                      IM_COL32(10, 15, 20, 230));
    dl->AddLine({origin.x + pw, origin.y},
                {origin.x + pw, origin.y + ph}, col_.primary);

    if (font_ui_) ImGui::PushFont(font_ui_);
    dl->AddText({origin.x + 8.f, origin.y + 6.f}, col_.primary, "MESSAGES");

    float py = origin.y + 28.f;
    for (const auto& msg : s.lora_messages) {
        if (py + 38.f > origin.y + ph) break;

        // Look up sender name
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
        dl->AddText({origin.x + 8.f, py}, col_.accent, hdr);
        py += 16.f;

        ImU32 txt_col = msg.read ? col_.text_dim : col_.text;
        dl->AddText({origin.x + 8.f, py}, txt_col, msg.text.c_str());
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

    constexpr ImU32 col_major  = IM_COL32(255, 160,  32, 255);
    constexpr ImU32 col_mid    = IM_COL32(255, 140,  20, 180);
    constexpr ImU32 col_minor  = IM_COL32(255, 130,  20, 110);
    constexpr ImU32 col_glow1  = IM_COL32(255, 160,  32,  70);
    constexpr ImU32 col_glow2  = IM_COL32(255, 160,  32,  28);

    if (font_mono_) ImGui::PushFont(font_mono_);
    for (int deg = 0; deg < 360; deg++) {
        float offset = deg - heading;
        while (offset >  180.f) offset -= 360.f;
        while (offset < -180.f) offset += 360.f;

        float px = center_x + offset * ppd;
        if (px < origin.x || px > origin.x + tw) continue;

        if (deg % 45 == 0) {
            // Glow — wide dim halos behind the sharp tick
            dl->AddLine({px, origin.y + 2.f}, {px, tick_y}, col_glow2, 9.f);
            dl->AddLine({px, origin.y + 2.f}, {px, tick_y}, col_glow1, 4.f);
            dl->AddLine({px, origin.y + 2.f}, {px, tick_y}, col_major, 1.5f);
            dl->AddText({px - 8.f, origin.y + 4.f}, col_major,
                        cardinal_str(static_cast<float>(deg)));
        } else if (deg % 10 == 0) {
            dl->AddLine({px, tick_y - 12.f}, {px, tick_y}, col_mid);
            char buf[8]; snprintf(buf, sizeof(buf), "%d", deg);
            dl->AddText({px - 8.f, tick_y - 28.f}, col_.text_dim, buf);
        } else if (deg % 5 == 0) {
            dl->AddLine({px, tick_y - 8.f}, {px, tick_y}, col_minor);
        }
    }

    // Centre cursor triangle — glow then sharp fill
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

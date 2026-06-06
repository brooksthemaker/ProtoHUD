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

// Angular width (radians) a string would occupy on an arc of radius r, at the
// currently-set NVG font. Used to centre/lay out curved text.
static float arc_text_width(NVGcontext* vg, const char* text, float r) {
    float tot = 0.f;
    for (const char* p = text; *p; ++p)
        tot += nvgTextBounds(vg, 0, 0, p, p + 1, nullptr);
    return tot / (r < 1.f ? 1.f : r);
}

// Draw text along an arc (each glyph translated to its arc position and rotated to
// the tangent), starting at start_angle and laying out clockwise. Caller sets the
// font + fill first. Returns the end angle so segments can be chained.
static float nvg_text_arc(NVGcontext* vg, float cx, float cy, float r,
                          float start_angle, const char* text) {
    const float rr = (r < 1.f) ? 1.f : r;
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    float ang = start_angle;
    for (const char* p = text; *p; ++p) {
        const float adv = nvgTextBounds(vg, 0, 0, p, p + 1, nullptr);
        const float th  = ang + (adv * 0.5f) / rr;
        nvgSave(vg);
        nvgTranslate(vg, cx + std::cos(th) * rr, cy + std::sin(th) * rr);
        nvgRotate(vg, th + static_cast<float>(M_PI) * 0.5f);
        nvgText(vg, 0, 0, p, p + 1);
        nvgRestore(vg);
        ang += adv / rr;
    }
    return ang;
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

// Crisp black outline for HUD text over bright scenes: draws the string in black
// at the 4 diagonal offsets using the CURRENT font + alignment. Caller draws the
// fill on top. (Was 8 offsets — the 4 diagonals cover horizontal+vertical strokes
// and halve the text-rasterization cost, which is a meaningful per-frame win when
// many labels are outlined.)
static void nvg_text_outline(NVGcontext* vg, float x, float y, const char* t,
                             float o = 1.6f) {
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 215));
    nvgText(vg, x - o, y - o, t, nullptr); nvgText(vg, x + o, y - o, t, nullptr);
    nvgText(vg, x - o, y + o, t, nullptr); nvgText(vg, x + o, y + o, t, nullptr);
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
    // Without an explicit glyph range ImGui only rasterizes Basic Latin +
    // Latin-1, so the typographic punctuation/symbols used throughout the menu
    // copy (… — · • ✕ ✉ → ▶ ▰ ▱ …) fall back to the missing-glyph box / "?".
    // DejaVuSans has them all — pull in the extra Unicode blocks we actually use.
    static const ImWchar kMenuRanges[] = {
        0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
        0x2000, 0x206F,   // General Punctuation (… — ‘ ’ “ ” • ·)
        0x2190, 0x21FF,   // Arrows (← ↑ → ↓)
        0x2200, 0x22FF,   // Mathematical Operators (× ÷ ≈)
        0x2300, 0x23FF,   // Misc Technical (⌚ ⏱)
        0x25A0, 0x25FF,   // Geometric Shapes (▶ ◀ ■ ▰ ▱ ●)
        0x2600, 0x26FF,   // Misc Symbols
        0x2700, 0x27BF,   // Dingbats (✕ ✉ ✓ ✦)
        0,
    };
    fc.GlyphRanges = kMenuRanges;
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
    if (nvg_ && map_img_ >= 0) { nvgDeleteImage(nvg_, map_img_); map_img_ = -1; }
    if (nvg_) {
        for (auto& [tex, img] : pip_nvg_cache_)
            if (img >= 0) nvgDeleteImage(nvg_, img);
        pip_nvg_cache_.clear();
    }
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

void HudRenderer::begin_nvg_overlay(int w, int h) {
    if (!nvg_ || nvg_frame_active_) return;
    nvgBeginFrame(nvg_, static_cast<float>(w), static_cast<float>(h), 1.0f);
    nvg_frame_active_ = true;
}

void HudRenderer::end_nvg_overlay() {
    if (!nvg_ || !nvg_frame_active_) return;
    nvgEndFrame(nvg_);
    nvg_frame_active_ = false;
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glStencilMask(0xFF);
}

void HudRenderer::draw_hud_frame(const AppState& s, int w, int h, bool show_fps) {
    if (!nvg_) return;
    const float fw       = static_cast<float>(w);
    const float fh       = static_cast<float>(h);
    const float ch       = static_cast<float>(cfg_.compass_height);
    const float c_margin = static_cast<float>(cfg_.compass_bottom_margin);
    const bool  flip     = cfg_.hud_flip_vertical;

    const bool own_frame = !nvg_frame_active_;
    if (own_frame) nvgBeginFrame(nvg_, fw, fh, 1.0f);

    draw_map_overlay(nvg_, s, fw, fh);
    // The expanded map can optionally hide the info panel (its data is in the sidebar).
    if (!(s.map_overlay.expanded && s.expanded_hide_info))
        draw_info_panel(nvg_, s, fw, fh);
    fx_update(nvg_, s, fw, fh, frame_dt_);

    // Legacy HUD chrome (edge/corner indicators). The minimap + info panel above
    // are the modular HUD and stay on; this block is the togglable legacy layer.
    if (s.legacy_hud) {
        if (!s.lora_messages.empty()) {
            float pw    = static_cast<float>(cfg_.panel_width);
            float msg_w = std::min(pw, fw / 3.f);
            float msg_y = flip ? (c_margin + ch) : 0.f;
            draw_lora_messages(nvg_, s, 0.f, msg_y, msg_w, fh);
        }

        const float cw        = fw / 3.f;
        const float compass_y = flip ? c_margin : fh - ch - c_margin;
        if (s.compass_tape)
            draw_compass_tape(nvg_, s, fw / 2.f - cw / 2.f, compass_y, cw, ch);
        draw_health_side(nvg_, s.health, fw, fh, false,
                         s.focus_left, s.focus_right, s.night_vision.nv_enabled);
        draw_health_side(nvg_, s.health, fw, fh, true,
                         s.focus_left, s.focus_right, s.night_vision.nv_enabled);
        draw_face_indicator        (nvg_, s.face, fw, fh);
        // The minimap clock (above the map) subsumes the corner clock + timer/alarm
        // indicator when it's active; otherwise fall back to the corner readouts.
        if (!(s.map_overlay.enabled && s.map_overlay.clock)) {
            draw_clock_indicator       (nvg_, s,      fw, fh);
            draw_timer_alarm_indicator (nvg_, s,      fw, fh);
        }
    }
    fx_draw_alarm_pulse(nvg_, s, fw, fh);

    if (show_fps) {
        draw_fps_nvg(nvg_, s, fw, fh);
        fps_shown_in_hud_ = true;
    }

    if (own_frame) {
        nvgEndFrame(nvg_);
        nvg_frame_active_ = false;
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glStencilMask(0xFF);
    }
}

void HudRenderer::draw_toasts(NotificationQueue& live_q, int w, int h) {
    if (!nvg_) return;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const bool own_frame = !nvg_frame_active_;
    if (own_frame) nvgBeginFrame(nvg_, fw, fh, 1.0f);
    toast_renderer_.draw(nvg_, live_q, fw, fh, frame_dt_, nvg_font_ui_, nvg_font_mono_, &icons_);
    if (own_frame) {
        nvgEndFrame(nvg_);
        nvg_frame_active_ = false;
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glStencilMask(0xFF);
    }
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

void HudRenderer::draw_map_overlay(NVGcontext* vg, const AppState& s, float fw, float fh) {
    const auto& cfg = s.map_overlay;
    if (!cfg.enabled) return;

    // Reload image on render thread when path changes
    if (cfg.map_path != map_img_path_) {
        if (map_img_ >= 0) { nvgDeleteImage(vg, map_img_); map_img_ = -1; }
        map_img_path_.clear();
        if (!cfg.map_path.empty()) {
            map_img_ = nvgCreateImage(vg, cfg.map_path.c_str(), 0);
            if (map_img_ >= 0) {
                nvgImageSize(vg, map_img_, &map_img_w_, &map_img_h_);
                map_img_path_ = cfg.map_path;
            }
        }
    }
    // Helldivers-style temporary expanded view takes over; skip the minimap.
    if (cfg.expanded) { draw_map_expanded(vg, s, fw, fh); return; }

    const bool  has_img = (map_img_ >= 0);
    const float half   = cfg.size_px;
    const float aspect = (has_img && map_img_h_ > 0 && map_img_w_ > 0)
                         ? (float)map_img_h_ / (float)map_img_w_ : 1.f;
    const float hw     = half;
    const float hh     = cfg.circle_window ? half : half * aspect;
    // Clamp center so the map window never extends past the screen edge.
    // anchor 0.0 → left/top edge flush, 0.5 → centered, 1.0 → right/bottom edge flush.
    const float cx     = std::clamp(fw * cfg.anchor_x + cfg.pan_x, hw, fw - hw);
    const float cy     = std::clamp(fh * cfg.anchor_y + cfg.pan_y, hh, fh - hh);

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);

    // Window shape helper (circle or rect).
    auto path_window = [&]{
        nvgBeginPath(vg);
        if (cfg.circle_window) nvgCircle(vg, 0.f, 0.f, half);
        else                   nvgRect(vg, -hw, -hh, hw * 2.f, hh * 2.f);
    };

    if (has_img) {
        // Build the image paint inside the rotated frame so it captures rotation.
        nvgSave(vg);
        if (cfg.image_rotate_deg != 0.f)
            nvgRotate(vg, cfg.image_rotate_deg * (float)M_PI / 180.f);
        if (cfg.rotate_with_heading && cfg.calibrated)
            nvgRotate(vg, (s.compass_heading - cfg.map_north_deg) * (float)M_PI / 180.f);
        const float z   = std::max(cfg.zoom, 1.0f);
        NVGpaint img = nvgImagePattern(vg, -hw * z, -hh * z, hw * 2.f * z, hh * 2.f * z,
                                       0.f, map_img_, cfg.opacity);
        nvgRestore(vg);  // back to screen-aligned at (cx, cy)
        path_window();
        nvgFillPaint(vg, img);
        nvgFill(vg);
    } else {
        // No image configured — still draw a dark "always-on" base disc.
        path_window();
        nvgFillColor(vg, nvgRGBA(10, 16, 22, 180));
        nvgFill(vg);
    }

    // Border hugs the window shape exactly — theme-colored (matches the info panel).
    path_window();
    nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 220));
    nvgStrokeWidth(vg, 1.8f);
    nvgStroke(vg);

    // Red crosshair at centre = wearer's position on the map.
    nvgBeginPath(vg);
    nvgMoveTo(vg, -9.f, 0.f); nvgLineTo(vg, 9.f, 0.f);
    nvgMoveTo(vg, 0.f, -9.f); nvgLineTo(vg, 0.f, 9.f);
    nvgStrokeColor(vg, nvgRGBA(255, 70, 70, 220));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);

    nvgRestore(vg);

    const float ringR = cfg.circle_window ? half : std::max(hw, hh);

    // Which screen quadrant the minimap occupies. The chrome that protrudes past
    // the disc (clock/date arc, system gauge) faces the screen interior so it
    // stays visible regardless of the top/bottom + side docking.
    const bool dock_left = cx < fw * 0.5f;
    const bool dock_top  = cy < fh * 0.5f;

    // Compass ring + LoRa markers around the minimap (Battlefield-style).
    if (cfg.compass_ring)
        draw_compass_ring(vg, s, cx, cy, ringR);

    // System gauge — a ~quarter ring on the minimap's left, OUTSIDE the compass,
    // with a gap. Normally shows the controller battery; with system_debug on it
    // shows CPU (inner bar) + GPU/render-load (outer bar, concentric & offset).
    if (cfg.battery_arc) {
        const float DEG = (float)M_PI / 180.f;
        // Gauge sits on the disc's interior side: right side when docked left,
        // left side when docked right — so it never runs off the screen edge.
        const float a0  = (dock_left ? -35.f : 145.f) * DEG;
        const float a1  = (dock_left ?  35.f : 215.f) * DEG;

        // Date-label styling shared by every gauge label (matches the minimap
        // clock's date arc: small UI font + crisp outline).
        const float lbl_scale = std::max(0.6f, s.clock_cfg.font_scale);
        const float lbl_sz    = 11.f * lbl_scale * 0.82f;

        // Draw one gauge arc (dark track + coloured fill) with its value label
        // floated just past the TOP end of the arc, styled like the date label.
        auto gauge = [&](float r, float ga0, float ga1, float v01,
                         NVGcolor fill, const char* label, bool known, float thick) {
            nvgLineCap(vg, NVG_ROUND);
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, r, ga0, ga1, NVG_CW);
            nvgStrokeColor(vg, nvgRGBA(40, 48, 56, 200));
            nvgStrokeWidth(vg, thick);
            nvgStroke(vg);
            if (known) {
                v01 = std::clamp(v01, 0.f, 1.f);
                nvgBeginPath(vg);
                nvgArc(vg, cx, cy, r, ga0, ga0 + (ga1 - ga0) * v01, NVG_CW);
                nvgStrokeColor(vg, fill);
                nvgStrokeWidth(vg, thick);
                nvgStroke(vg);
            }
            nvgLineCap(vg, NVG_BUTT);
            // Label at the arc's TOP end (highest on screen = smallest sin),
            // floated just outside the track. Same font/outline as the date
            // label; the value keeps its gauge colour so load still reads at a
            // glance.
            const float topAng = (std::sin(ga0) <= std::sin(ga1)) ? ga0 : ga1;
            const float lr  = r + thick * 0.5f + lbl_sz * 0.85f;
            const float lx  = cx + std::cos(topAng) * lr;
            const float ly  = cy + std::sin(topAng) * lr;
            nvg_set_font_ui(lbl_sz);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvg_text_outline(vg, lx, ly, label, 1.4f);
            nvgFillColor(vg, known ? fill : nvgRGBA(160, 170, 180, 220));
            nvgText(vg, lx, ly, label, nullptr);
        };

        const float r1 = ringR + 56.f;            // inner bar (normal-mode battery)
        const float r2 = ringR + 68.f;            // outer bar, concentric
        const float off = 10.f * DEG;             // raise the inner bar ~10° at the top
        const float cw  = 15.f * DEG;             // shift the inner two gauges 15° clockwise

        if (cfg.system_debug) {
            // Gauge stack, inner → outer: phone battery, CPU, GPU, RAM. The phone
            // arc sits innermost and 5° CCW; CPU at the baseline angle; GPU +5° CW;
            // RAM +10° CW. CPU/GPU/RAM are thin so all three fit in the radial band
            // the old CPU+GPU pair used (ringR+56 … ringR+68).
            const float cpu = std::clamp(s.sys_metrics.cpu_pct / 100.f, 0.f, 1.f);
            const float ft  = s.sys_metrics.frame_time_ms;
            const float gpu_inst = std::clamp(ft / (1000.f / 60.f), 0.f, 1.f);
            gpu_load_smooth_ += (gpu_inst - gpu_load_smooth_) * 0.10f;
            const float gpu = gpu_load_smooth_;
            const float ram = s.sys_metrics.ram_total_mb > 0.f
                ? std::clamp(s.sys_metrics.ram_used_mb / s.sys_metrics.ram_total_mb, 0.f, 1.f)
                : 0.f;
            auto load_col = [](float v) {
                return v > 0.8f ? nvgRGBA(230, 70, 60, 235)
                     : v > 0.5f ? nvgRGBA(235, 180, 50, 230)
                     :            nvgRGBA(70, 210, 90, 230);
            };
            const float thin   = 5.f;             // thinner CPU/GPU/RAM arcs
            const float r_phn  = ringR + 47.f;    // phone battery — innermost
            const float r_cpu  = ringR + 56.f;
            const float r_gpu  = ringR + 62.f;
            const float r_ram  = ringR + 68.f;

            // Phone battery first (innermost, 5° CCW), light blue, when bound.
            const int ppct = s.health.phone_battery_pct;
            if (ppct >= 0) {
                const float ppc = std::clamp(ppct / 100.f, 0.f, 1.f);
                char ppb[12]; snprintf(ppb, sizeof(ppb), "%s%d%%",
                                       s.health.phone_charging ? "P+" : "P", ppct);
                gauge(r_phn, a0 - 5.f * DEG, a1 - 5.f * DEG, ppc,
                      nvgRGBA(120, 190, 255, 235), ppb, true, 7.f);
            }
            char cb[12]; snprintf(cb, sizeof(cb), "C%2.0f", cpu * 100.f);
            char gb[12]; snprintf(gb, sizeof(gb), "G%2.0f", gpu * 100.f);
            char rb[12]; snprintf(rb, sizeof(rb), "R%2.0f", ram * 100.f);
            gauge(r_cpu, a0,             a1,             cpu, load_col(cpu), cb, true, thin);
            gauge(r_gpu, a0 +  5.f * DEG, a1 +  5.f * DEG, gpu, load_col(gpu), gb, true, thin);
            gauge(r_ram, a0 + 10.f * DEG, a1 + 10.f * DEG, ram, load_col(ram), rb, true, thin);
        } else {
            // Controller battery — always drawn (inner arc). If a phone
            // battery is known (KDE Connect bridge bound to a paired
            // device), draw it on the outer arc using the same colour
            // ramp so the two read consistently. "P" prefix on the label
            // disambiguates from the controller's bare percentage.
            const int bpct = s.health.wireless_battery_pct;   // -1 = unknown
            const float pct = std::clamp(bpct / 100.f, 0.f, 1.f);
            auto bat_col = [](float v) {
                return v > 0.5f ? nvgRGBA(70, 210, 90, 230)
                     : v > 0.2f ? nvgRGBA(235, 180, 50, 230)
                     :            nvgRGBA(230, 70, 60, 235);
            };
            NVGcolor bc = bat_col(pct);
            char pb[8];
            if (bpct >= 0) snprintf(pb, sizeof(pb), "%d%%", bpct);
            else           snprintf(pb, sizeof(pb), "--");

            const int  ppct = s.health.phone_battery_pct;
            if (ppct >= 0) {
                const float ppc = std::clamp(ppct / 100.f, 0.f, 1.f);
                NVGcolor pc = bat_col(ppc);
                char ppb[10];
                snprintf(ppb, sizeof(ppb), "%s%d%%",
                         s.health.phone_charging ? "P+" : "P", ppct);
                gauge(r1, a0 + off + cw, a1 + off + cw, pct, bc, pb, bpct >= 0, 8.f);
                gauge(r2, a0 + cw,       a1 + cw,       ppc, pc, ppb, true, 8.f);
            } else {
                gauge(r1, a0 + cw, a1 + cw, pct, bc, pb, bpct >= 0, 8.f);
            }
        }
    }

    // Warning countdown rings hugging the minimap outline. Each active deadline
    // inside its warning window (alarm = final 5 min, timer = final 1 min) draws a
    // ring that empties clockwise from the top and ramps yellow→orange→red as it
    // nears firing. Multiple active countdowns stack as concentric rings just
    // inside the map edge. Real time is used (deadlines are real epochs).
    {
        const time_t now_real = std::time(nullptr);
        const float  TWO_PI   = 2.f * static_cast<float>(M_PI);
        auto cd_color = [](float f) {
            return f > 0.5f  ? nvgRGBA(235, 200, 50, 245)    // yellow
                 : f > 0.25f ? nvgRGBA(240, 150, 40, 248)    // orange
                 :             nvgRGBA(230, 60,  55, 252);   // red
        };
        auto draw_cd_ring = [&](float r, float frac, NVGcolor c) {
            const float a0 = -static_cast<float>(M_PI) * 0.5f;   // start at 12 o'clock
            float sweep = frac * TWO_PI;
            if (sweep > TWO_PI - 0.02f) sweep = TWO_PI - 0.02f;
            nvgLineCap(vg, NVG_ROUND);
            // Faint full-window track.
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, r, a0, a0 + TWO_PI - 0.02f, NVG_CW);
            nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 30));
            nvgStrokeWidth(vg, 3.f); nvgStroke(vg);
            // Dark backing for contrast over the feed.
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, r, a0, a0 + sweep, NVG_CW);
            nvgStrokeColor(vg, nvgRGBA(0, 0, 0, 170));
            nvgStrokeWidth(vg, 5.f); nvgStroke(vg);
            // Colored depleting ring.
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, r, a0, a0 + sweep, NVG_CW);
            nvgStrokeColor(vg, c);
            nvgStrokeWidth(vg, 3.f); nvgStroke(vg);
        };
        int stack = 0;
        auto add_cd = [&](float rem, float win) {
            if (rem <= 0.f || rem > win) return;
            const float frac = rem / win;
            draw_cd_ring(ringR - 4.f - stack * 5.f, frac, cd_color(frac));
            ++stack;
        };
        if (s.timer_alarm.timer_active)
            add_cd(static_cast<float>(s.timer_alarm.timer_end - now_real), 60.f);
        if (s.timer_alarm.alarm_active)
            add_cd(static_cast<float>(s.timer_alarm.alarm_fire_at - now_real), 300.f);
    }

    // Clock above the minimap, with an active timer/alarm shown beside it.
    if (cfg.clock) {
        time_t now = std::time(nullptr) + static_cast<time_t>(s.clock_cfg.manual_offset_s);
        struct tm tmv; localtime_r(&now, &tmv);
        char tbuf[32];
        const char* tfmt = s.clock_cfg.use_24h
            ? (s.clock_cfg.show_seconds ? "%H:%M:%S" : "%H:%M")
            : (s.clock_cfg.show_seconds ? "%I:%M:%S %p" : "%I:%M %p");
        strftime(tbuf, sizeof(tbuf), tfmt, &tmv);

        // Active timer (countdown) or alarm (fire time) shown next to the clock.
        std::string extra; ImU32 extra_col = IM_COL32(235, 180, 50, 235);
        if (s.timer_alarm.timer_active) {
            extra = "T-" + fmt_countdown(s.timer_alarm.timer_end);
            extra_col = IM_COL32(235, 180, 50, 235);
        } else if (s.timer_alarm.alarm_active) {
            struct tm av; time_t at = s.timer_alarm.alarm_fire_at; localtime_r(&at, &av);
            char ab[16]; strftime(ab, sizeof(ab), s.clock_cfg.use_24h ? "%H:%M" : "%I:%M%p", &av);
            extra = std::string("A ") + ab;
            extra_col = IM_COL32(230, 90, 90, 235);
        }

        const float scale = std::max(0.6f, s.clock_cfg.font_scale);
        const float csz   = 11.f * scale;          // ~half the previous size
        const float esz   = csz * 0.92f;
        const float dsz   = csz * 0.82f;
        const float DEGc  = static_cast<float>(M_PI) / 180.f;
        // Clock/date arc curves along the disc edge facing the screen interior:
        // above the disc when docked low, below when docked high; leaning toward
        // the interior horizontally so it never clips at the screen edge, plus a
        // ~10° nudge toward the right so it clears the corner cardinal.
        const float clock_deg = dock_top ? (dock_left ?  60.f :  120.f)
                                         : (dock_left ? -60.f : -120.f);
        const float clock_nudge = (clock_deg < 0.f ? 10.f : -10.f);   // ~10° to the right
        const float clock_angle = (clock_deg + clock_nudge) * DEGc;
        const float rc    = ringR + (cfg.compass_ring ? 46.f : 16.f) + csz * 0.5f;

        // Crisp 4-corner black outline for arc text — a real outline, not a soft blur
        // (which reads as a drop shadow). Offsetting the arc centre translates the
        // whole curved string by that pixel amount.
        auto arc_outline = [&](float rr, float st, const char* t) {
            const float o = 1.4f;
            nvgFillColor(vg, nvgRGBA(0, 0, 0, 230));
            nvg_text_arc(vg, cx - o, cy - o, rr, st, t);
            nvg_text_arc(vg, cx + o, cy - o, rr, st, t);
            nvg_text_arc(vg, cx - o, cy + o, rr, st, t);
            nvg_text_arc(vg, cx + o, cy + o, rr, st, t);
        };

        // Clock — curved text on an arc concentric with the map.
        nvg_set_font_ui(csz);
        const float wc    = arc_text_width(vg, tbuf, rc);
        const float start = clock_angle - wc * 0.5f;
        arc_outline(rc, start, tbuf);
        if (s_glow && s_glow_intensity > 0.f) {
            nvgFontBlur(vg, 3.f);
            nvgFillColor(vg, nvg_col_a(col_.glow_base,
                         static_cast<uint8_t>(72.f * s_glow_intensity)));
            nvg_text_arc(vg, cx, cy, rc, start, tbuf);
            nvgFontBlur(vg, 0.f);
        }
        nvgFillColor(vg, nvg_col(col_.text_fill));
        nvg_text_arc(vg, cx, cy, rc, start, tbuf);

        // Active timer/alarm continues the arc just past the clock.
        if (!extra.empty()) {
            nvg_set_font_ui(esz);
            const float estart = clock_angle + wc * 0.5f + 7.f / rc;
            arc_outline(rc, estart, extra.c_str());      // crisp outline pass
            nvgFillColor(vg, nvg_col(extra_col));
            nvg_text_arc(vg, cx, cy, rc, estart, extra.c_str());
        }

        // Date on an inner concentric arc, under the clock.
        if (cfg.clock_date) {
            const std::string ds = fmt_date();
            const float rd = rc - csz * 0.95f;
            nvg_set_font_ui(dsz);
            const float wd = arc_text_width(vg, ds.c_str(), rd);
            const float ds0 = clock_angle - wd * 0.5f;
            arc_outline(rd, ds0, ds.c_str());            // crisp outline pass
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 200));
            nvg_text_arc(vg, cx, cy, rd, ds0, ds.c_str());
        }
    }
}

// ── Procedural weather glyphs ────────────────────────────────────────────────────
// Drawn when no wx-*.png icon art is present, so the weather widget always shows a
// recognizable icon. `sz` is the target box size; centered at (cx, cy).
static void draw_weather_glyph(NVGcontext* vg, int code, bool day,
                               float cx, float cy, float sz) {
    const float s = sz * 0.5f;
    const NVGcolor sun     = nvgRGBA(255, 200,  60, 255);
    const NVGcolor moon    = nvgRGBA(225, 230, 245, 255);
    const NVGcolor cloud   = nvgRGBA(205, 214, 224, 255);
    const NVGcolor cloud_d = nvgRGBA(150, 162, 176, 255);
    const NVGcolor rain    = nvgRGBA( 90, 165, 235, 255);
    const NVGcolor snow    = nvgRGBA(235, 244, 255, 255);
    const NVGcolor bolt    = nvgRGBA(255, 210,  70, 255);
    const NVGcolor edge    = nvgRGBA( 12,  18,  26, 235);   // dark outline for contrast

    auto cloud_shape = [&](float ox, float oy, float scale, NVGcolor c) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx + ox - s * 0.35f * scale, cy + oy,                s * 0.30f * scale);
        nvgCircle(vg, cx + ox + s * 0.02f * scale, cy + oy - s * 0.20f * scale, s * 0.38f * scale);
        nvgCircle(vg, cx + ox + s * 0.40f * scale, cy + oy,                s * 0.28f * scale);
        nvgRoundedRect(vg, cx + ox - s * 0.62f * scale, cy + oy,
                       s * 1.18f * scale, s * 0.36f * scale, s * 0.16f * scale);
        nvgFillColor(vg, c); nvgFill(vg);
        nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, std::max(2.f, s * 0.07f)); nvgStroke(vg);
    };
    auto sun_disc = [&](float ox, float oy, float r) {
        nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, std::max(2.f, r * 0.30f));
        for (int i = 0; i < 8; ++i) {                       // dark ray underlay then bright
            const float a = i / 8.f * 2.f * static_cast<float>(M_PI);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx + ox + std::cos(a) * r * 1.30f, cy + oy + std::sin(a) * r * 1.30f);
            nvgLineTo(vg, cx + ox + std::cos(a) * r * 1.80f, cy + oy + std::sin(a) * r * 1.80f);
            nvgStroke(vg);
        }
        nvgStrokeColor(vg, sun); nvgStrokeWidth(vg, std::max(1.5f, r * 0.18f));
        for (int i = 0; i < 8; ++i) {
            const float a = i / 8.f * 2.f * static_cast<float>(M_PI);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx + ox + std::cos(a) * r * 1.35f, cy + oy + std::sin(a) * r * 1.35f);
            nvgLineTo(vg, cx + ox + std::cos(a) * r * 1.75f, cy + oy + std::sin(a) * r * 1.75f);
            nvgStroke(vg);
        }
        nvgBeginPath(vg); nvgCircle(vg, cx + ox, cy + oy, r);
        nvgFillColor(vg, sun); nvgFill(vg);
        nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, std::max(2.f, r * 0.12f)); nvgStroke(vg);
    };
    auto disc = [&](float ox, float oy, float r, NVGcolor c) {
        nvgBeginPath(vg); nvgCircle(vg, cx + ox, cy + oy, r);
        nvgFillColor(vg, c); nvgFill(vg);
        nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, std::max(2.f, r * 0.14f)); nvgStroke(vg);
    };
    auto rain_streaks = [&](NVGcolor c) {
        nvgStrokeColor(vg, c); nvgStrokeWidth(vg, std::max(2.5f, s * 0.12f));
        for (int i = -1; i <= 1; ++i) {
            const float x = cx + i * s * 0.36f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, cy + s * 0.40f);
            nvgLineTo(vg, x - s * 0.12f, cy + s * 0.82f);
            nvgStroke(vg);
        }
    };
    auto snow_dots = [&]() {
        for (int i = -1; i <= 1; ++i) {
            nvgBeginPath(vg); nvgCircle(vg, cx + i * s * 0.36f, cy + s * 0.60f, s * 0.10f);
            nvgFillColor(vg, snow); nvgFill(vg);
            nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, 1.5f); nvgStroke(vg);
        }
    };

    if (code <= 0) {                                   // clear
        if (day) sun_disc(0.f, 0.f, s * 0.52f);
        else     disc(0.f, 0.f, s * 0.52f, moon);
    } else if (code <= 2) {                             // partly cloudy
        if (day) sun_disc(-s * 0.32f, -s * 0.34f, s * 0.30f);
        else     disc(-s * 0.32f, -s * 0.30f, s * 0.26f, moon);
        cloud_shape(s * 0.10f, s * 0.12f, 1.0f, cloud);
    } else if (code == 3) {                            // overcast
        cloud_shape(0.f, 0.f, 1.1f, cloud_d);
    } else if (code == 45 || code == 48) {             // fog
        cloud_shape(0.f, -s * 0.18f, 1.0f, cloud);
        nvgStrokeColor(vg, cloud); nvgStrokeWidth(vg, std::max(2.f, s * 0.10f));
        for (int i = 0; i < 3; ++i) {
            const float y = cy + s * 0.38f + i * s * 0.22f;
            nvgBeginPath(vg); nvgMoveTo(vg, cx - s * 0.55f, y); nvgLineTo(vg, cx + s * 0.55f, y); nvgStroke(vg);
        }
    } else if (code >= 51 && code <= 57) {             // drizzle
        cloud_shape(0.f, -s * 0.15f, 1.0f, cloud); snow_dots();
    } else if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) {  // rain
        cloud_shape(0.f, -s * 0.15f, 1.0f, cloud); rain_streaks(rain);
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {    // snow
        cloud_shape(0.f, -s * 0.15f, 1.0f, cloud); snow_dots();
    } else if (code >= 95) {                            // thunderstorm
        cloud_shape(0.f, -s * 0.15f, 1.0f, cloud_d);
        nvgFillColor(vg, bolt);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + s * 0.10f, cy + s * 0.30f);
        nvgLineTo(vg, cx - s * 0.18f, cy + s * 0.46f);
        nvgLineTo(vg, cx + s * 0.02f, cy + s * 0.52f);
        nvgLineTo(vg, cx - s * 0.12f, cy + s * 0.88f);
        nvgLineTo(vg, cx + s * 0.26f, cy + s * 0.40f);
        nvgLineTo(vg, cx + s * 0.06f, cy + s * 0.34f);
        nvgClosePath(vg); nvgFill(vg);
    } else {
        cloud_shape(0.f, 0.f, 1.0f, cloud);
    }
}

// ── Procedural status glyphs ─────────────────────────────────────────────────────
// Small line icons for the info-panel status ring (wifi / bluetooth / gamepad /
// audio / ssh / lora). Drawn when no status-*.png art is present. Stroked in `c`
// (caller dims it when the status is inactive); centered at (cx, cy); `sz` = box size.
enum class StatusGlyph { Wifi, Bluetooth, Gamepad, Audio, Ssh, Lora };

static const char* status_png_name(StatusGlyph g) {
    switch (g) {
        case StatusGlyph::Wifi:      return "status-wifi";
        case StatusGlyph::Bluetooth: return "status-bt";
        case StatusGlyph::Gamepad:   return "status-gamepad";
        case StatusGlyph::Audio:     return "status-audio";
        case StatusGlyph::Ssh:       return "status-ssh";
        case StatusGlyph::Lora:      return "status-lora";
    }
    return "status-wifi";
}

static void draw_status_glyph(NVGcontext* vg, StatusGlyph g,
                              float cx, float cy, float sz, NVGcolor c) {
    const float h  = sz * 0.5f;
    const float lw = std::max(1.5f, sz * 0.10f);
    const float PI = static_cast<float>(M_PI);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, lw);

    switch (g) {
    case StatusGlyph::Wifi: {
        const float bx = cx, by = cy + h * 0.55f;
        for (int i = 1; i <= 3; ++i) {                     // nested top arcs + base dot
            nvgBeginPath(vg);
            nvgArc(vg, bx, by, h * 0.32f * i, -2.36f, -0.78f, NVG_CW);
            nvgStroke(vg);
        }
        nvgBeginPath(vg); nvgCircle(vg, bx, by, lw * 0.9f);
        nvgFillColor(vg, c); nvgFill(vg);
        break;
    }
    case StatusGlyph::Bluetooth: {
        // Rune: lower-right knee → top → bottom → upper-right knee (diagonals cross
        // the spine, forming the two right-pointing flags).
        const float w = h * 0.55f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + w, cy + h * 0.5f);
        nvgLineTo(vg, cx,     cy - h);
        nvgLineTo(vg, cx,     cy + h);
        nvgLineTo(vg, cx + w, cy - h * 0.5f);
        nvgStroke(vg);
        break;
    }
    case StatusGlyph::Gamepad: {
        nvgBeginPath(vg);                                  // body
        nvgRoundedRect(vg, cx - h * 0.85f, cy - h * 0.42f, h * 1.7f, h * 0.84f, h * 0.34f);
        nvgStroke(vg);
        const float dx = cx - h * 0.40f;                   // d-pad
        nvgBeginPath(vg);
        nvgMoveTo(vg, dx - h * 0.22f, cy); nvgLineTo(vg, dx + h * 0.22f, cy);
        nvgMoveTo(vg, dx, cy - h * 0.22f); nvgLineTo(vg, dx, cy + h * 0.22f);
        nvgStroke(vg);
        nvgBeginPath(vg);                                  // buttons
        nvgCircle(vg, cx + h * 0.34f, cy - h * 0.12f, lw * 0.85f);
        nvgCircle(vg, cx + h * 0.58f, cy + h * 0.12f, lw * 0.85f);
        nvgFillColor(vg, c); nvgFill(vg);
        break;
    }
    case StatusGlyph::Audio: {
        nvgBeginPath(vg);                                  // speaker box + cone
        nvgMoveTo(vg, cx - h * 0.70f, cy - h * 0.22f);
        nvgLineTo(vg, cx - h * 0.40f, cy - h * 0.22f);
        nvgLineTo(vg, cx - h * 0.05f, cy - h * 0.50f);
        nvgLineTo(vg, cx - h * 0.05f, cy + h * 0.50f);
        nvgLineTo(vg, cx - h * 0.40f, cy + h * 0.22f);
        nvgLineTo(vg, cx - h * 0.70f, cy + h * 0.22f);
        nvgClosePath(vg);
        nvgStroke(vg);
        for (int i = 1; i <= 2; ++i) {                     // sound waves
            nvgBeginPath(vg);
            nvgArc(vg, cx - h * 0.05f, cy, h * 0.30f * i, -0.7f, 0.7f, NVG_CW);
            nvgStroke(vg);
        }
        break;
    }
    case StatusGlyph::Ssh: {
        nvgBeginPath(vg);                                  // terminal window
        nvgRoundedRect(vg, cx - h * 0.8f, cy - h * 0.62f, h * 1.6f, h * 1.24f, h * 0.18f);
        nvgStroke(vg);
        nvgBeginPath(vg);                                  // ">" prompt
        nvgMoveTo(vg, cx - h * 0.42f, cy - h * 0.18f);
        nvgLineTo(vg, cx - h * 0.16f, cy + h * 0.06f);
        nvgLineTo(vg, cx - h * 0.42f, cy + h * 0.30f);
        nvgStroke(vg);
        nvgBeginPath(vg);                                  // "_" cursor
        nvgMoveTo(vg, cx + h * 0.04f, cy + h * 0.30f);
        nvgLineTo(vg, cx + h * 0.42f, cy + h * 0.30f);
        nvgStroke(vg);
        break;
    }
    case StatusGlyph::Lora: {
        const float ny = cy - h * 0.18f;                   // node (antenna top)
        nvgBeginPath(vg);                                  // mast + base
        nvgMoveTo(vg, cx, ny); nvgLineTo(vg, cx, cy + h * 0.72f);
        nvgMoveTo(vg, cx - h * 0.30f, cy + h * 0.72f);
        nvgLineTo(vg, cx + h * 0.30f, cy + h * 0.72f);
        nvgStroke(vg);
        nvgBeginPath(vg); nvgCircle(vg, cx, ny, lw * 0.9f);
        nvgFillColor(vg, c); nvgFill(vg);
        for (int side = 0; side < 2; ++side) {             // emission waves L/R
            const float a0 = side ? -0.6f : PI - 0.6f;
            const float a1 = side ?  0.6f : PI + 0.6f;
            for (int i = 1; i <= 2; ++i) {
                nvgBeginPath(vg);
                nvgArc(vg, cx, ny, h * 0.28f * i, a0, a1, NVG_CW);
                nvgStroke(vg);
            }
        }
        break;
    }
    }
}

// ── Cycling info panel ──────────────────────────────────────────────────────────
// A configurable region (mirroring the minimap on the opposite side) that auto-
// cycles through glanceable widgets: analog clock, notifications, schedule, weather.
// Drawn once per frame (mono overlay), so the dwell timer ticks here safely.
void HudRenderer::draw_info_panel(NVGcontext* vg, const AppState& s, float fw, float fh) {
    const InfoPanelConfig& cfg = s.info_panel;
    if (!cfg.enabled) return;

    // Build the list of enabled widgets in fixed order.
    constexpr int kCount = static_cast<int>(InfoWidget::Count);
    int order[kCount]; int n = 0;
    for (int i = 0; i < kCount; ++i)
        if (cfg.show[i]) order[n++] = i;
    if (n == 0) return;

    // Advance the cycle (once per frame).
    info_cycle_t_ += frame_dt_;
    if (n > 1 && info_cycle_t_ >= std::max(1.f, cfg.cycle_sec)) {
        info_cycle_t_   = 0.f;
        info_cycle_idx_ = (info_cycle_idx_ + 1) % n;
    }
    if (info_cycle_idx_ >= n) info_cycle_idx_ = 0;
    const int widget = order[info_cycle_idx_];

    const float r  = s.map_overlay.size_px;   // twin: mirror the minimap's footprint
    const float px = std::clamp(fw * cfg.anchor_x + cfg.pan_x, r, fw - r);
    const float py = std::clamp(fh * cfg.anchor_y + cfg.pan_y, r, fh - r);
    const float TWO_PI = 2.f * static_cast<float>(M_PI);

    // Text with a black outline for legibility over the camera feed. Font and
    // alignment must already be set (nvg_text_outline uses the current state).
    auto otext = [&](float x, float y, const char* str, NVGcolor fill) {
        nvg_text_outline(vg, x, y, str, 1.4f);
        nvgFillColor(vg, fill);
        nvgText(vg, x, y, str, nullptr);
    };

    // Backing disc + theme-colored border (matches the minimap circle).
    nvgBeginPath(vg); nvgCircle(vg, px, py, r);
    nvgFillColor(vg, nvgRGBA(10, 16, 22, 170)); nvgFill(vg);
    nvgBeginPath(vg); nvgCircle(vg, px, py, r);
    nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 220)); nvgStrokeWidth(vg, 1.8f); nvgStroke(vg);

    // (Page name is no longer drawn inside the disc — it appears in the label ring
    //  around the panel; see the two-ring block after the scissor is popped.)

    // Clip widget bodies to the disc's bounding box so long text never spills out.
    nvgSave(vg);
    nvgScissor(vg, px - r, py - r, r * 2.f, r * 2.f);

    if (widget == static_cast<int>(InfoWidget::Clock)) {
        // Analog clock — no title, so it fills the disc. Face is selectable:
        //   0=Ticks 1=Numbers 2=Minimal  (hands follow the live theme)
        //   3=Halo 4=Solar 5=Fallout 6=Space  (recolour to that preset + a
        //   small signature flourish)   7=Auto (track whatever theme is active)
        const time_t now = std::time(nullptr) +
                           static_cast<time_t>(s.clock_cfg.manual_offset_s);
        struct tm tmv; localtime_r(&now, &tmv);
        const float cr = r * 0.82f;
        const int   face = s.info_panel.clock_face;
        const float HALF_PI = static_cast<float>(M_PI) * 0.5f;

        // Resolve marker style, signature, and colours for the selected face.
        enum { M_TICKS, M_NUMBERS, M_QUARTERS };
        int      markers   = M_TICKS;
        int      signature = 0;   // 0=none 1=Halo ring 2=Solar rays 3=Fallout 4=Space
        NVGcolor markCol   = nvgRGBA(200, 220, 230, 150);     // tick/number colour
        NVGcolor handCol   = nvg_col(col_.text_fill);         // hour/minute hands
        const NVGcolor secCol = nvgRGBA(235, 80, 70, 235);    // second hand + hub
        switch (face) {
            case 1: markers = M_NUMBERS;  markCol = nvg_col_a(col_.text_fill, 220); break;
            case 2: markers = M_QUARTERS; break;
            case 3: markers = M_TICKS;    signature = 1;       // Halo (UNSC holo-cyan)
                    markCol = nvgRGBA(120, 205, 235, 200); handCol = nvgRGBA(150, 235, 255, 255); break;
            case 4: markers = M_TICKS;    signature = 2;       // Solar (orange)
                    markCol = nvgRGBA(255, 160,  32, 210); handCol = nvgRGBA(255, 160,  32, 255); break;
            case 5: markers = M_NUMBERS;  signature = 3;       // Fallout (green)
                    markCol = nvgRGBA(  0, 255,  80, 230); handCol = nvgRGBA(  0, 255,  80, 255); break;
            case 6: markers = M_QUARTERS; signature = 4;       // Space (blue)
                    markCol = nvgRGBA( 80, 100, 255, 210); handCol = nvgRGBA(200, 220, 255, 255); break;
            case 7: markers = M_TICKS;                         // Auto (live theme)
                    markCol = nvg_col_a(col_.compass_tick, 200); handCol = nvg_col(col_.text_fill); break;
            default: break;                                    // 0 = Ticks (grey)
        }

        if (markers == M_NUMBERS) {
            nvg_set_font_ui(cr * 0.20f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            for (int h = 1; h <= 12; ++h) {
                const float a = h / 12.f * TWO_PI - HALF_PI;
                char nb[4]; snprintf(nb, sizeof(nb), "%d", h);
                otext(px + std::cos(a) * cr * 0.84f,
                      py + std::sin(a) * cr * 0.84f, nb, markCol);
            }
        } else {
            const int step = (markers == M_QUARTERS) ? 3 : 1;
            for (int i = 0; i < 12; i += step) {
                const float a  = static_cast<float>(i) / 12.f * TWO_PI - HALF_PI;
                const float r0 = cr * 0.86f, r1 = cr * 0.99f;
                nvgBeginPath(vg);
                nvgMoveTo(vg, px + std::cos(a) * r0, py + std::sin(a) * r0);
                nvgLineTo(vg, px + std::cos(a) * r1, py + std::sin(a) * r1);
                nvgStrokeColor(vg, markCol);
                nvgStrokeWidth(vg, (i % 3 == 0) ? 2.5f : 1.f); nvgStroke(vg);
            }
        }

        // Per-theme signature flourish.
        if (signature == 1) {                 // Halo — UNSC ring array (holo cyan)
            // Thin holographic ring just outside the dial.
            nvgBeginPath(vg); nvgCircle(vg, px, py, cr * 1.05f);
            nvgStrokeColor(vg, nvgRGBA(150, 235, 255, 110)); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
            // Bright segment across the top — the Halo array arcing overhead.
            const float topA = static_cast<float>(M_PI) * 1.5f;     // straight up (y-down)
            nvgLineCap(vg, NVG_ROUND);
            nvgBeginPath(vg); nvgArc(vg, px, py, cr * 1.05f, topA - 1.2f, topA + 1.2f, NVG_CW);
            nvgStrokeColor(vg, nvgRGBA(150, 235, 255,  80)); nvgStrokeWidth(vg, 6.f); nvgStroke(vg);
            nvgBeginPath(vg); nvgArc(vg, px, py, cr * 1.05f, topA - 1.2f, topA + 1.2f, NVG_CW);
            nvgStrokeColor(vg, nvgRGBA(190, 245, 255, 220)); nvgStrokeWidth(vg, 2.f); nvgStroke(vg);
            // Inner Forerunner accent ring around the hub.
            nvgBeginPath(vg); nvgCircle(vg, px, py, cr * 0.32f);
            nvgStrokeColor(vg, nvgRGBA(120, 205, 235, 110)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
        } else if (signature == 2) {          // Solar — diagonal sun rays
            for (int k = 0; k < 4; ++k) {
                const float a = (k / 4.f) * TWO_PI + HALF_PI * 0.5f;
                nvgBeginPath(vg);
                nvgMoveTo(vg, px + std::cos(a) * cr * 1.02f, py + std::sin(a) * cr * 1.02f);
                nvgLineTo(vg, px + std::cos(a) * cr * 1.14f, py + std::sin(a) * cr * 1.14f);
                nvgStrokeColor(vg, nvgRGBA(255, 160, 32, 200)); nvgStrokeWidth(vg, 2.f); nvgStroke(vg);
            }
        } else if (signature == 3) {          // Fallout — indicator triangle at 12
            const float ty = py - cr * 0.99f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, px, ty + cr * 0.10f);
            nvgLineTo(vg, px - cr * 0.06f, ty);
            nvgLineTo(vg, px + cr * 0.06f, ty);
            nvgClosePath(vg);
            nvgFillColor(vg, nvgRGBA(0, 255, 80, 220)); nvgFill(vg);
        } else if (signature == 4) {          // Space — 4-point star at 12
            const float sx = px, sy = py - cr * 0.84f, sr = cr * 0.12f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, sx, sy - sr);
            nvgLineTo(vg, sx + sr * 0.28f, sy - sr * 0.28f);
            nvgLineTo(vg, sx + sr, sy);
            nvgLineTo(vg, sx + sr * 0.28f, sy + sr * 0.28f);
            nvgLineTo(vg, sx, sy + sr);
            nvgLineTo(vg, sx - sr * 0.28f, sy + sr * 0.28f);
            nvgLineTo(vg, sx - sr, sy);
            nvgLineTo(vg, sx - sr * 0.28f, sy - sr * 0.28f);
            nvgClosePath(vg);
            nvgFillColor(vg, nvgRGBA(200, 220, 255, 230)); nvgFill(vg);
        }

        auto hand = [&](float frac, float len, float w, NVGcolor c) {
            const float a = frac * TWO_PI - HALF_PI;
            nvgLineCap(vg, NVG_ROUND);
            nvgBeginPath(vg); nvgMoveTo(vg, px, py);
            nvgLineTo(vg, px + std::cos(a) * len, py + std::sin(a) * len);
            nvgStrokeColor(vg, c); nvgStrokeWidth(vg, w); nvgStroke(vg);
        };
        const float hh = (tmv.tm_hour % 12) + tmv.tm_min / 60.f;
        const float mm = tmv.tm_min + tmv.tm_sec / 60.f;
        hand(hh / 12.f, cr * 0.52f, 4.f,  handCol);
        hand(mm / 60.f, cr * 0.80f, 2.5f, handCol);
        hand(tmv.tm_sec / 60.f, cr * 0.90f, 1.f, secCol);
        nvgBeginPath(vg); nvgCircle(vg, px, py, 3.f);
        nvgFillColor(vg, secCol); nvgFill(vg);

    } else if (widget == static_cast<int>(InfoWidget::Notifications)) {
        nvg_set_font_ui(12.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        char cnt[32]; snprintf(cnt, sizeof(cnt), "%d unread", s.notifs.unread_count());
        otext(px, py - r + 24.f, cnt, nvg_col_a(col_.text_fill, 180));

        // Group by app/source: one row per app with its icon and how many
        // notifications it has (unread shown brighter, with the count on the
        // right). Newest app first; App-type groups key on the sender/app name
        // (KDE Connect sets the title to the app), others on their kind.
        struct Grp { std::string key, icon; int total = 0, unread = 0; };
        std::vector<Grp> groups;
        for (const auto& nt : s.notifs.items) {
            if (nt.dismissed) continue;
            std::string key;
            switch (nt.type) {
                case NotifType::App:
                    key = !nt.title.empty() ? nt.title
                        : (!nt.icon.empty() ? nt.icon : std::string("App")); break;
                case NotifType::Alarm: key = "Alarms"; break;
                case NotifType::Timer: key = "Timers"; break;
                case NotifType::LoRa:  key = "LoRa";   break;
            }
            const std::string icon = !nt.icon.empty()
                ? nt.icon : std::string(notif_type_icon(nt.type));
            Grp* g = nullptr;
            for (auto& e : groups) if (e.key == key) { g = &e; break; }
            if (!g) { groups.push_back({ key, icon, 0, 0 }); g = &groups.back(); }
            g->total++; if (!nt.read) g->unread++;
        }

        if (groups.empty()) {
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            otext(px, py, "no notifications", nvg_col_a(col_.text_fill, 150));
        } else {
            const int   rows = std::min(static_cast<int>(groups.size()), 5);
            const float lh   = std::clamp(r * 0.24f, 20.f, 30.f);
            const float isz  = lh * 0.74f;
            const float lx   = px - r * 0.74f;        // left edge (icon)
            const float namx = lx + isz + 8.f;        // app name
            const float cntx = px + r * 0.76f;        // right-aligned count
            float ty = py - (rows - 1) * lh * 0.5f;   // vertically centered block
            const float name_sz = std::clamp(r * 0.14f, 12.f, 18.f);
            for (int i = 0; i < rows; ++i) {
                const Grp& g   = groups[i];
                const bool hot = g.unread > 0;
                const float a  = hot ? 1.f : 0.55f;
                // App icon (fallback: a dot in the type/text colour).
                if (!icons_.draw(vg, g.icon, lx + isz * 0.5f, ty, isz, a)) {
                    nvgBeginPath(vg); nvgCircle(vg, lx + isz * 0.5f, ty, isz * 0.32f);
                    nvgFillColor(vg, nvg_col_a(col_.text_fill, hot ? 200 : 110)); nvgFill(vg);
                }
                nvg_set_font_ui(name_sz);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                otext(namx, ty, g.key.c_str(),
                      hot ? nvg_col_a(col_.text_fill, 235) : nvg_col_a(col_.text_fill, 140));
                char nb[16]; snprintf(nb, sizeof(nb), "%d", g.total);
                nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                otext(cntx, ty, nb,
                      hot ? nvg_col_a(col_.glow_base, 255) : nvg_col_a(col_.text_fill, 150));
                ty += lh;
            }
        }

    } else if (widget == static_cast<int>(InfoWidget::Schedule)) {
        // Each upcoming event as two centered lines: title, then "location | time".
        const time_t now = std::time(nullptr);
        float ty = py - r * 0.60f; int shown = 0;
        for (const auto& e : s.scheduler_events) {
            if (e.start_utc == 0) continue;
            if (e.end_utc != 0 && e.end_utc < now) continue;   // skip finished
            if (shown >= 3) break;
            struct tm tmv; localtime_r(&e.start_utc, &tmv);
            char tm[16];
            if (e.all_day) snprintf(tm, sizeof(tm), "all-day");
            else           strftime(tm, sizeof(tm), "%H:%M", &tmv);
            char sub[96];
            if (!e.location.empty())
                snprintf(sub, sizeof(sub), "%s  |  %s", e.location.c_str(), tm);
            else
                snprintf(sub, sizeof(sub), "%s", tm);

            nvg_set_font_ui(r * 0.12f);                        // title
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            otext(px, ty, e.title.c_str(), nvg_col_a(col_.text_fill, 235));
            nvg_set_font_ui(r * 0.095f);                       // location | time
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            otext(px, ty + r * 0.13f, sub, nvg_col_a(col_.text_fill, 180));
            ty += r * 0.32f; ++shown;
        }
        if (shown == 0) {
            nvg_set_font_ui(14.f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            otext(px, py, "no events", nvg_col_a(col_.text_fill, 150));
        }

    } else if (widget == static_cast<int>(InfoWidget::Weather)) {
        // Weather page 1 — current conditions.
        const WeatherState& w = s.weather;
        if (!w.ok) {
            nvg_set_font_ui(14.f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            otext(px, py, s.weather_cfg.enabled ? "..." : "off", nvg_col_a(col_.text_fill, 150));
        } else {
            const float iy = py - r * 0.45f, isz = r * 0.48f;
            if (!icons_.draw(vg, wmo_icon(w.code, w.is_day), px, iy, isz, 1.f))
                draw_weather_glyph(vg, w.code, w.is_day, px, iy, isz);
            char b[48];
            // NB: nvg_set_font_ui() resets alignment, so re-center after each call.
            nvg_set_font_ui(r * 0.15f);                       // actual | feels-like
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            snprintf(b, sizeof(b), "%.0f°  feels %.0f°",
                     static_cast<double>(w.temp), static_cast<double>(w.feels));
            otext(px, py - r * 0.02f, b, nvg_col_a(col_.text_fill, 235));
            nvg_set_font_ui(r * 0.12f);                       // High / Low
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            snprintf(b, sizeof(b), "H %.0f°   L %.0f°",
                     static_cast<double>(w.temp_high), static_cast<double>(w.temp_low));
            otext(px, py + r * 0.18f, b, nvg_col_a(col_.text_fill, 210));
            if (w.humidity >= 0) {                            // Humidity
                snprintf(b, sizeof(b), "Humidity %d%%", w.humidity);
                otext(px, py + r * 0.36f, b, nvg_col_a(col_.text_fill, 210));
            }
            if (!w.location.empty()) {
                nvg_set_font_ui(r * 0.10f);
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                otext(px, py + r * 0.54f, w.location.c_str(), nvg_col_a(col_.text_fill, 150));
            }
        }
    } else {  // WeatherPrecip — page 2: precipitation.
        const WeatherState& w = s.weather;
        if (!w.ok) {
            nvg_set_font_ui(14.f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            otext(px, py, s.weather_cfg.enabled ? "..." : "off", nvg_col_a(col_.text_fill, 150));
        } else {
            // Big condition icon (rain/snow/cloud/sun) + precip amount + rain chance.
            const float iy = py - r * 0.20f, isz = r * 0.64f;
            if (!icons_.draw(vg, wmo_icon(w.code, w.is_day), px, iy, isz, 1.f))
                draw_weather_glyph(vg, w.code, w.is_day, px, iy, isz);
            char b[40];
            // nvg_set_font_ui() resets alignment — re-center after it.
            nvg_set_font_ui(r * 0.13f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            snprintf(b, sizeof(b), "Precip %.1f %s", static_cast<double>(w.precip_now),
                     s.weather_cfg.metric ? "mm" : "in");
            otext(px, py + r * 0.36f, b, nvg_col_a(col_.text_fill, 225));
            if (w.rain_prob >= 0) snprintf(b, sizeof(b), "Rain %d%%", w.rain_prob);
            else                  snprintf(b, sizeof(b), "Rain --");
            otext(px, py + r * 0.56f, b, nvg_col_a(col_.text_fill, 210));
        }
    }

    nvgRestore(vg);  // pop scissor

    // Page labels + status icons ring the disc, both on the interior 120°→-24° arc:
    //   • inner = page labels as annular-sector "trapezoids" (curved long edges that
    //     follow the disc, straight radial short edges), styled like the radial menu.
    //     Active page = accent box + dark text; inactive = dark box + white text.
    //   • outer = system status badges (doubled), lit when active / dimmed.
    {
        const float DEG     = static_cast<float>(M_PI) / 180.f;
        const float HALF_PI = static_cast<float>(M_PI) * 0.5f;
        const float base_sz = std::clamp(r * 0.15f, 14.f, 26.f);
        const float lbl_h   = base_sz * 0.66f;             // labels ~⅓ smaller
        const float icon_sz = base_sz * 2.f;               // icons doubled
        // When the disc sits in the top half of the screen, swing the whole
        // label/icon ring 90° clockwise so it fans below the disc instead of off
        // the top edge. Position-based (not the dock flag) so it fires however the
        // panel ended up there. The labels also flip (see arc_label) only when
        // top-docked so they read right-way-up against the inverted arc.
        const bool  top_dock = py < fh * 0.5f;
        const float arc_rot  = top_dock ? -90.f : 0.f;
        const float arc0     = 120.f + arc_rot, arc1 = -24.f + arc_rot;  // shared label/icon span

        // ── Inner ring: page-label wedges (radial-menu look) ────────────────────
        static const char* kNames[kCount] = { "CLOCK", "ALERTS", "SCHEDULE",
                                              "WEATHER", "PRECIP" };
        const float r0in  = r + 6.f;                       // band inner radius
        const float bandW = lbl_h + 10.f;
        const float r1in  = r0in + bandW;                  // band outer radius
        const float rmid  = (r0in + r1in) * 0.5f;          // text baseline radius
        const float gapR  = 3.f * DEG;                     // angular gap between wedges

        // Curved label text: each glyph follows the wedge's arc (tangent-rotated)
        // so the band reads like a visor ring. The whole run flips as one only
        // when the disc is top-docked — so it never reads upside-down there — and
        // is NOT flipped per glyph by angle (which used to leave the bottom-half
        // labels facing the opposite way from the top-half ones).
        auto arc_label = [&](float center_ang, const char* str, NVGcolor col) {
            float total = 0.f;
            for (const char* p = str; *p; ++p) total += nvgTextBounds(vg, 0, 0, p, p + 1, nullptr);
            total /= rmid;
            nvgFillColor(vg, col);
            const bool flip = top_dock;
            float ang = flip ? center_ang + total * 0.5f : center_ang - total * 0.5f;
            for (const char* p = str; *p; ++p) {
                const float adv = nvgTextBounds(vg, 0, 0, p, p + 1, nullptr);
                const float th  = flip ? ang - (adv * 0.5f) / rmid : ang + (adv * 0.5f) / rmid;
                nvgSave(vg);
                nvgTranslate(vg, px + std::cos(th) * rmid, py + std::sin(th) * rmid);
                nvgRotate(vg, flip ? th - HALF_PI : th + HALF_PI);
                nvgText(vg, 0, 0, p, p + 1);
                nvgRestore(vg);
                ang += flip ? -adv / rmid : adv / rmid;
            }
        };

        nvg_set_font_ui(lbl_h);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        for (int i = 0; i < n; ++i) {
            const char* nm     = kNames[order[i]];
            const bool  active = (i == info_cycle_idx_);
            // Tile the arc into n wedges; screen angle = -(unit angle).
            const float u0 = arc0 + (arc1 - arc0) * (float)i / n;
            const float u1 = arc0 + (arc1 - arc0) * (float)(i + 1) / n;
            const float a0 = -u0 * DEG + gapR * 0.5f;       // screen (a0 < a1)
            const float a1 = -u1 * DEG - gapR * 0.5f;
            const float sc = (a0 + a1) * 0.5f;
            const NVGcolor fill   = active ? nvg_col_a(col_.glow_base, 210)
                                           : nvgRGBA(18, 24, 30, 185);
            const NVGcolor border = active ? nvg_col_a(col_.glow_base, 235)
                                           : nvg_col_a(col_.glow_base, 70);
            nvgBeginPath(vg);                               // annular sector path
            nvgArc(vg, px, py, r1in, a0, a1, NVG_CW);       // outer edge (curved)
            nvgArc(vg, px, py, r0in, a1, a0, NVG_CCW);      // inner edge (curved) back
            nvgClosePath(vg);                               // straight radial short edges
            nvgFillColor(vg, fill); nvgFill(vg);
            nvgStrokeColor(vg, border); nvgStrokeWidth(vg, active ? 2.f : 1.f); nvgStroke(vg);
            arc_label(sc, nm, active ? nvgRGBA(20, 22, 26, 255)        // dark text on accent
                                     : nvgRGBA(230, 235, 240, 230));    // white text on dark box
        }

        // ── Outer ring: status badges (outside the label band) ──────────────────
        bool bt_on = false;
        for (const auto& d : s.bt_devices) if (d.connected) { bt_on = true; break; }
        struct { StatusGlyph g; bool on; } items[] = {
            { StatusGlyph::Wifi,      s.wifi.connected || s.health.wifi_ok },
            { StatusGlyph::Bluetooth, bt_on },
            { StatusGlyph::Gamepad,   s.health.gamepad_ok },
            { StatusGlyph::Audio,     s.health.audio_ok || s.audio.enabled },
            { StatusGlyph::Ssh,       s.ssh.active || s.health.ssh_active },
            { StatusGlyph::Lora,      s.health.lora_ok },
        };
        const int   ni     = static_cast<int>(sizeof(items) / sizeof(items[0]));
        const float icon_r = r1in + icon_sz * 0.62f + 6.f;
        for (int i = 0; i < ni; ++i) {
            const float deg = arc0 + (ni > 1 ? (arc1 - arc0) * (float)i / (ni - 1) : 0.f);
            const float a   = deg * DEG;
            const float ix  = px + std::cos(a) * icon_r;
            const float iy  = py - std::sin(a) * icon_r;        // unit-circle → screen (y down)
            const bool  on  = items[i].on;
            nvgBeginPath(vg); nvgCircle(vg, ix, iy, icon_sz * 0.62f);   // dark badge backing
            nvgFillColor(vg, nvgRGBA(10, 16, 22, on ? 175 : 120)); nvgFill(vg);
            nvgStrokeColor(vg, on ? nvg_col_a(col_.glow_base, 205) : nvgRGBA(255, 255, 255, 55));
            nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
            const float alpha = on ? 1.f : 0.35f;
            const NVGcolor gc = on ? nvg_col_a(col_.text_fill, 240)
                                   : nvg_col_a(col_.text_fill, 80);
            if (!icons_.draw(vg, status_png_name(items[i].g), ix, iy, icon_sz * 0.92f, alpha))
                draw_status_glyph(vg, items[i].g, ix, iy, icon_sz * 0.80f, gc);
        }
    }
}

// ── Compass ring around the minimap ─────────────────────────────────────────────
// Cardinal letters + degree ticks ringing the minimap, rotating with heading so the
// forward direction is at the top, plus LoRa node bearing markers + distance labels
// around the outer edge (Battlefield-style).
void HudRenderer::draw_compass_ring(NVGcontext* vg, const AppState& s,
                                    float cx, float cy, float radius, bool bold) {
    // The compass bezel always rotates with heading (that's what a compass does);
    // map.rotate_with_heading separately controls whether the MAP IMAGE turns too.
    // `bold` (used by the expanded map) thickens ticks + emboldens the cardinals.
    const float heading = s.compass_heading;
    const float tw_mul  = bold ? 2.0f : 1.0f;
    const float DEG = (float)M_PI / 180.f;
    auto bearing_angle = [&](float beta_deg) -> float {
        float rel = beta_deg - heading;          // 0 = forward
        return -(float)M_PI * 0.5f + rel * DEG;  // forward at top, screen y-down
    };

    // Compass ring hugs the minimap; the battery/system gauge sits outside it.
    const float r_tick  = radius + 8.f;
    const float r_card  = radius + 26.f;
    const float r_mark  = radius + 26.f;
    const float r_dist  = radius + 42.f;

    const NVGcolor col_major = nvg_col(col_.compass_tick);
    const NVGcolor col_minor = nvg_col_a(col_.compass_tick, 110);

    // Degree ticks every 15°, longer at the 90° cardinals.
    for (int d = 0; d < 360; d += 15) {
        const bool card = (d % 90 == 0);
        const float a = bearing_angle((float)d);
        const float ca = std::cos(a), sa = std::sin(a);
        const float t  = card ? 13.f : 6.f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + ca * r_tick,        cy + sa * r_tick);
        nvgLineTo(vg, cx + ca * (r_tick + t),  cy + sa * (r_tick + t));
        nvgStrokeColor(vg, card ? col_major : col_minor);
        nvgStrokeWidth(vg, (card ? 2.0f : 1.0f) * tw_mul);
        nvgStroke(vg);
    }

    // Cardinal letters (bigger + faux-bold in the expanded view).
    static const char* kCard[4] = { "N", "E", "S", "W" };
    static const int   kDeg [4] = { 0, 90, 180, 270 };
    nvg_set_font_ui(bold ? 24.f : 16.f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    for (int i = 0; i < 4; ++i) {
        const float a = bearing_angle((float)kDeg[i]);
        const float lx = cx + std::cos(a) * r_card;
        const float ly = cy + std::sin(a) * r_card;
        nvg_text_outline(vg, lx, ly, kCard[i], bold ? 2.2f : 1.6f);
        nvg_glow_text(vg, lx, ly, kCard[i], i == 0, col_.glow_base, col_.text_fill);
        if (bold) {   // extra offset fill passes = faux bold
            nvgFillColor(vg, nvg_col(col_.text_fill));
            nvgText(vg, lx + 0.7f, ly, kCard[i], nullptr);
            nvgText(vg, lx - 0.7f, ly, kCard[i], nullptr);
        }
    }

    // LoRa node bearing markers + distance labels around the outside.
    nvg_set_font_mono(11.f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    for (const auto& node : s.lora_nodes) {
        if (node.distance_m <= 0.f) continue;
        const float a  = bearing_angle(node.heading_deg);
        const float ca = std::cos(a), sa = std::sin(a);
        const float mx = cx + ca * r_mark, my = cy + sa * r_mark;
        const NVGcolor nc = nvg_col(s.lora_node_colors[node.local_id % 8]);

        // Triangle pointing outward at the node bearing.
        const float bx = -sa, by = ca;   // tangent
        nvgBeginPath(vg);
        nvgMoveTo(vg, mx + ca * 7.f,           my + sa * 7.f);
        nvgLineTo(vg, mx - ca * 4.f + bx * 5.f, my - sa * 4.f + by * 5.f);
        nvgLineTo(vg, mx - ca * 4.f - bx * 5.f, my - sa * 4.f - by * 5.f);
        nvgClosePath(vg);
        nvgFillColor(vg, nc);
        nvgFill(vg);

        // Distance label just outside the marker.
        char db[16];
        if (node.distance_m >= 1000.f) snprintf(db, sizeof(db), "%.1fk", node.distance_m / 1000.f);
        else                           snprintf(db, sizeof(db), "%.0fm", node.distance_m);
        nvg_text_outline(vg, cx + ca * r_dist, cy + sa * r_dist, db, 1.4f);
        nvgFillColor(vg, nvg_col_a(s.lora_node_colors[node.local_id % 8], 230));
        nvgText(vg, cx + ca * r_dist, cy + sa * r_dist, db, nullptr);
    }
}

// ── Expanded map (Helldivers-style pan/zoom) ────────────────────────────────────
// A larger, temporary view that grows from the minimap's location. Pan/zoom are
// driven by input (state.map_overlay.view_pan_*/view_zoom). Stays SBS-safe by
// growing in place rather than re-centering across the eye seam.
void HudRenderer::draw_map_expanded(NVGcontext* vg, const AppState& s, float fw, float fh) {
    const auto& cfg = s.map_overlay;

    // Dim the scene behind the expanded map.
    nvgBeginPath(vg);
    nvgRect(vg, 0.f, 0.f, fw, fh);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 150));
    nvgFill(vg);

    const float half = std::min(fh, fw) * 0.42f;     // big disc
    const float cx   = std::clamp(fw * cfg.anchor_x + cfg.pan_x, half, fw - half);
    const float cy   = std::clamp(fh * cfg.anchor_y + cfg.pan_y, half, fh - half);
    const bool  circle = cfg.circle_window;

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);

    auto path_win = [&]{
        nvgBeginPath(vg);
        if (circle) nvgCircle(vg, 0.f, 0.f, half);
        else        nvgRect(vg, -half, -half, half * 2.f, half * 2.f);
    };

    if (map_img_ >= 0) {
        const float z = std::max(cfg.zoom, 1.0f) * std::max(cfg.view_zoom, 1.0f);
        const float panx = cfg.view_pan_x * half * 2.f;
        const float pany = cfg.view_pan_y * half * 2.f;
        nvgSave(vg);
        if (cfg.image_rotate_deg != 0.f)
            nvgRotate(vg, cfg.image_rotate_deg * (float)M_PI / 180.f);
        if (cfg.rotate_with_heading && cfg.calibrated)
            nvgRotate(vg, (s.compass_heading - cfg.map_north_deg) * (float)M_PI / 180.f);
        NVGpaint img = nvgImagePattern(vg, -half * z - panx, -half * z - pany,
                                       half * 2.f * z, half * 2.f * z, 0.f, map_img_, 1.0f);
        nvgRestore(vg);
        path_win();
        nvgFillPaint(vg, img);
        nvgFill(vg);
    } else {
        path_win();
        nvgFillColor(vg, nvgRGBA(10, 16, 22, 220));
        nvgFill(vg);
    }

    path_win();
    nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 220));
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);

    // Centre crosshair (wearer).
    nvgBeginPath(vg);
    nvgMoveTo(vg, -12.f, 0.f); nvgLineTo(vg, 12.f, 0.f);
    nvgMoveTo(vg, 0.f, -12.f); nvgLineTo(vg, 0.f, 12.f);
    nvgStrokeColor(vg, nvgRGBA(255, 70, 70, 230));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);

    nvgRestore(vg);

    // Compass bezel around the expanded map (turns as the user turns) — bold.
    draw_compass_ring(vg, s, cx, cy, half, /*bold=*/true);

    // Zoom readout (corner of the map).
    char zb[32]; snprintf(zb, sizeof(zb), "%.1fx", std::max(cfg.view_zoom, 1.0f));
    nvg_set_font_ui(15.f);
    nvgFillColor(vg, nvg_col_a(col_.text_fill, 210));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, cx + half - 36.f, cy - half + 12.f, zb, nullptr);

    // ── Controls panel (fixed bottom-left, on-screen regardless of map size) ──
    {
        const bool locked = cfg.rotate_with_heading;
        struct Row { const char* k; const char* v; };
        const Row rows[] = {
            { "MOVE",     "ARROWS / DPAD" },
            { "ZOOM",     "+ / -   \xC2\xB7   LB / RB" },
            { "ROTATION", locked ? "LOCKED  (R / A)" : "FREE  (R / A)" },
            { "CLOSE",    "ESC / B / N" },
        };
        const float lh = 22.f, padx = 12.f, pady = 10.f;
        const float pw = 290.f, ph = lh * 4.f + pady * 2.f;
        // Bottom-right of the screen (the left edge now holds the info sidebar).
        const float px = fw - pw - 22.f, py = fh - ph - 20.f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, px, py, pw, ph, 6.f);
        nvgFillColor(vg, nvgRGBA(8, 12, 18, 215));
        nvgFill(vg);
        nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 190));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        nvg_set_font_mono(13.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        float ly = py + pady;
        for (const auto& r : rows) {
            nvgFillColor(vg, nvg_col_a(col_.glow_base, 235));
            nvgText(vg, px + padx, ly, r.k, nullptr);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 225));
            nvgText(vg, px + padx + 96.f, ly, r.v, nullptr);
            ly += lh;
        }
    }

    // Left info column: system indicators, time/date, weather + forecast, schedule.
    draw_expanded_sidebar(vg, s, fw, fh);
}

// ── Expanded-map left sidebar ────────────────────────────────────────────────────
// A fixed left column shown only over the expanded map: Old-HUD status indicators,
// the time + date, current weather with a 3-day forecast, and the schedule.
void HudRenderer::draw_expanded_sidebar(NVGcontext* vg, const AppState& s,
                                        float fw, float fh) {
    (void)fw;
    const float x0   = 20.f;
    const float top  = 20.f;
    const float colW = 290.f;
    const float bottom = fh - 20.f;
    const float pad  = 14.f;
    const float x    = x0 + pad;          // content left
    const float cw   = colW - pad * 2.f;  // content width
    float y          = top + pad;         // running cursor

    // Panel backing.
    nvgBeginPath(vg); nvgRoundedRect(vg, x0, top, colW, bottom - top, 8.f);
    nvgFillColor(vg, nvgRGBA(8, 12, 18, 205)); nvgFill(vg);
    nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 150)); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);

    auto header = [&](const char* t) {
        nvg_set_font_ui(13.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvg_col_a(col_.glow_base, 235));
        nvgText(vg, x, y, t, nullptr);
        y += 16.f;
        nvgBeginPath(vg); nvgMoveTo(vg, x, y); nvgLineTo(vg, x + cw, y);
        nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 90)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
        y += 9.f;
    };

    // ── 1. System indicators (everything from the Old HUD) ──────────────────
    header("SYSTEM");
    {
        const SystemHealth& h = s.health;
        bool bt_on = false;
        for (const auto& d : s.bt_devices) if (d.connected) { bt_on = true; break; }
        struct Ind { const char* label; bool ok; };
        const Ind inds[] = {
            { "Proot",     h.teensy_ok },
            { "LoRa",      h.lora_ok },
            { "Interface", h.knob_ok },
            { "WiFi",      s.wifi.connected || h.wifi_ok },
            { "Bluetooth", bt_on },
            { "Gamepad",   h.gamepad_ok },
            { "Audio",     h.audio_ok || s.audio.enabled },
            { "SSH",       s.ssh.active || h.ssh_active },
            { "Android",   h.android_mirror },
            { "MPU",       h.mpu9250_ok },
            { "L.Cam",     h.cam_owl_left },
            { "R.Cam",     h.cam_owl_right },
            { "Cam 1",     h.cam_usb1 },
            { "Cam 2",     h.cam_usb2 },
        };
        const int   N    = static_cast<int>(sizeof(inds) / sizeof(inds[0]));
        const float rowH = 18.f;
        const float colX2 = x + cw * 0.5f;
        nvg_set_font_mono(12.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        for (int i = 0; i < N; ++i) {
            const float rx = (i % 2 == 0) ? x : colX2;
            const float ry = y + (i / 2) * rowH + rowH * 0.5f;
            nvgBeginPath(vg); nvgCircle(vg, rx + 4.f, ry, 4.f);
            nvgFillColor(vg, inds[i].ok ? nvg_col(col_.ind_good) : nvg_col(col_.ind_fail));
            nvgFill(vg);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, inds[i].ok ? 230 : 150));
            nvgText(vg, rx + 14.f, ry, inds[i].label, nullptr);
        }
        y += ((N + 1) / 2) * rowH + 12.f;
    }

    // ── 2. Time & date ──────────────────────────────────────────────────────
    header("TIME");
    {
        const time_t now = std::time(nullptr) + static_cast<time_t>(s.clock_cfg.manual_offset_s);
        struct tm tmv; localtime_r(&now, &tmv);
        char tbuf[24], dbuf[40];
        if (s.clock_cfg.use_24h) {
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        } else {
            int h12 = tmv.tm_hour % 12; if (!h12) h12 = 12;
            snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                     tmv.tm_hour < 12 ? "AM" : "PM");
        }
        static const char* kDow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char* kMon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec"};
        snprintf(dbuf, sizeof(dbuf), "%s, %s %d", kDow[tmv.tm_wday], kMon[tmv.tm_mon], tmv.tm_mday);
        nvg_set_font_ui(30.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvg_col(col_.text_fill));
        nvgText(vg, x, y, tbuf, nullptr);
        y += 33.f;
        nvg_set_font_ui(14.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvg_col_a(col_.text_fill, 205));
        nvgText(vg, x, y, dbuf, nullptr);
        y += 24.f;
    }

    // ── 3. Weather + 3-day forecast ─────────────────────────────────────────
    header("WEATHER");
    {
        const WeatherState& w = s.weather;
        if (!w.ok) {
            nvg_set_font_ui(13.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 150));
            nvgText(vg, x, y, s.weather_cfg.enabled ? "fetching..." : "off", nullptr);
            y += 22.f;
        } else {
            const float isz = 46.f;
            if (!icons_.draw(vg, wmo_icon(w.code, w.is_day), x + isz * 0.5f, y + isz * 0.5f, isz, 1.f))
                draw_weather_glyph(vg, w.code, w.is_day, x + isz * 0.5f, y + isz * 0.5f, isz);
            char b[80];
            nvg_set_font_ui(26.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvg_col(col_.text_fill));
            snprintf(b, sizeof(b), "%.0f\xC2\xB0", static_cast<double>(w.temp));
            nvgText(vg, x + isz + 10.f, y, b, nullptr);
            nvg_set_font_ui(12.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 205));
            snprintf(b, sizeof(b), "%s  feels %.0f\xC2\xB0",
                     w.condition.c_str(), static_cast<double>(w.feels));
            nvgText(vg, x + isz + 10.f, y + 28.f, b, nullptr);
            if (!w.location.empty())
                nvgText(vg, x + isz + 10.f, y + 42.f, w.location.c_str(), nullptr);
            y += isz + 14.f;

            static const char* kDow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            const time_t base = std::time(nullptr);
            const float rowH = 26.f;
            for (int i = 0; i < w.forecast_count; ++i) {
                const WeatherDay& fd = w.forecast[i];
                const time_t dt = base + static_cast<time_t>(i) * 86400;
                struct tm dtm; localtime_r(&dt, &dtm);
                const float ry = y + rowH * 0.5f;
                nvg_set_font_mono(12.f);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg, nvg_col_a(col_.text_fill, 220));
                nvgText(vg, x, ry, i == 0 ? "Today" : kDow[dtm.tm_wday], nullptr);
                if (!icons_.draw(vg, wmo_icon(fd.code, true), x + 70.f, ry, 20.f, 1.f))
                    draw_weather_glyph(vg, fd.code, true, x + 70.f, ry, 20.f);
                char hb[32];
                snprintf(hb, sizeof(hb), "%.0f / %.0f\xC2\xB0",
                         static_cast<double>(fd.tmax), static_cast<double>(fd.tmin));
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg, nvg_col_a(col_.text_fill, 230));
                nvgText(vg, x + 92.f, ry, hb, nullptr);
                if (fd.rain_prob >= 0) {
                    char rb[16]; snprintf(rb, sizeof(rb), "%d%%", fd.rain_prob);
                    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(vg, nvg_col_a(col_.glow_base, 220));
                    nvgText(vg, x + cw, ry, rb, nullptr);
                }
                y += rowH;
            }
            y += 10.f;
        }
    }

    // ── 4. Schedule ─────────────────────────────────────────────────────────
    header("SCHEDULE");
    {
        const time_t now = std::time(nullptr);
        int shown = 0;
        for (const auto& e : s.scheduler_events) {
            if (e.start_utc == 0) continue;
            if (e.end_utc != 0 && e.end_utc < now) continue;   // skip finished
            if (shown >= 6 || y > bottom - 34.f) break;
            struct tm tmv; localtime_r(&e.start_utc, &tmv);
            char tb[16];
            if (e.all_day) snprintf(tb, sizeof(tb), "all-day");
            else           strftime(tb, sizeof(tb), "%H:%M", &tmv);
            char sub[112];
            if (!e.location.empty()) snprintf(sub, sizeof(sub), "%s  %s", tb, e.location.c_str());
            else                     snprintf(sub, sizeof(sub), "%s", tb);
            nvg_set_font_ui(13.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 235));
            nvgText(vg, x, y, e.title.c_str(), nullptr);
            nvg_set_font_mono(11.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 170));
            nvgText(vg, x, y + 15.f, sub, nullptr);
            y += 32.f; ++shown;
        }
        if (shown == 0) {
            nvg_set_font_ui(13.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, nvg_col_a(col_.text_fill, 150));
            nvgText(vg, x, y, "no events", nullptr);
        }
    }
}

void HudRenderer::draw_fps_nvg(NVGcontext* vg, const AppState& snap, float fw, float fh) {
    const float fps = snap.sys_metrics.fps_avg_smooth > 0.f
                        ? snap.sys_metrics.fps_avg_smooth
                        : snap.sys_metrics.fps_avg;
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
                              float /*bottom_margin*/) {
    constexpr float kEdge = 20.f;
    float cx = sw * cfg.anchor_x + cfg.pan_x;
    float cy = sh * cfg.anchor_y + cfg.pan_y;
    // Clamp so the pip stays on screen with a kEdge margin
    cx = std::clamp(cx, ov_w * 0.5f + kEdge, sw - ov_w * 0.5f - kEdge);
    cy = std::clamp(cy, ov_h * 0.5f + kEdge, sh - ov_h * 0.5f - kEdge);
    return { cx - ov_w * 0.5f, cy - ov_h * 0.5f };
}

// ── PiP underlay (NanoVG, drawn before HUD chrome) ───────────────────────────

// ── Shared single-pip NVG renderer ───────────────────────────────────────────
// Caller must have an active NVG frame. Draws background, image (rotation-
// aware, always rounded corners), and theme-coloured border. No ImGui.

void HudRenderer::draw_pip_nvg_single(NVGcontext* vg, unsigned int tex,
                                       const OverlayConfig& cfg,
                                       float fw, float fh)
{
    const float margin = static_cast<float>(cfg_.compass_height);
    const float C      = cfg_.pip_corner_clip_px;

    const float ov_h = fh * cfg.size;
    const float ov_w = ov_h * (16.f / 9.f);

    using R = OverlayConfig::Rotation;
    const bool  is_portrait = (cfg.rotation == R::Portrait ||
                                cfg.rotation == R::PortraitFlipped);
    const float disp_w = is_portrait ? ov_h * (9.f / 16.f) : ov_w;
    const float disp_h = ov_h;

    const auto pos = overlay_origin(cfg, fw, fh, disp_w, disp_h, margin);

    // Cache GL tex → NVG image handle
    auto it = pip_nvg_cache_.find(tex);
    if (it == pip_nvg_cache_.end()) {
        int img = nvglCreateImageFromHandleGLES2(vg, tex, 1280, 720, 0);
        pip_nvg_cache_[tex] = img;
        it = pip_nvg_cache_.find(tex);
    }
    const int img = it->second;
    if (img < 0) return;

    float rot_angle = 0.f;
    switch (cfg.rotation) {
        case R::Portrait:         rot_angle =  (float)M_PI * 0.5f;  break;
        case R::LandscapeFlipped: rot_angle =  (float)M_PI;          break;
        case R::PortraitFlipped:  rot_angle = -(float)M_PI * 0.5f;  break;
        default: break;
    }

    const float cx = pos.x + disp_w * 0.5f;
    const float cy = pos.y + disp_h * 0.5f;
    const float dw = is_portrait ? disp_h : disp_w;
    const float dh = is_portrait ? disp_w : disp_h;
    const float hw = dw * 0.5f;
    const float hh = dh * 0.5f;

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    if (rot_angle != 0.f) nvgRotate(vg, rot_angle);

    // Background
    nvgBeginPath(vg);
    nvgRoundedRect(vg, -hw, -hh, dw, dh, C);
    nvgFillColor(vg, nvgRGBA(10, 15, 20, 200));
    nvgFill(vg);

    // Camera image — always rounded via nvgScissor+nvgRoundedRect clip
    {
        NVGpaint paint = nvgImagePattern(vg, -hw, -hh, dw, dh, 0.f, img, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -hw, -hh, dw, dh, C);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

    // Border — follow the live theme color (glow_base, same as compass/arms)
    {
        const ImU32 pc = col_.glow_base;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -hw, -hh, dw, dh, C);
        nvgStrokeColor(vg, nvgRGBA(pc & 0xFF, (pc >> 8) & 0xFF, (pc >> 16) & 0xFF, 160));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }

    nvgRestore(vg);
}

void HudRenderer::draw_pip_underlays(
    unsigned int tex1, bool act1, const OverlayConfig& c1,
    unsigned int tex2, bool act2, const OverlayConfig& c2,
    unsigned int tex3, bool act3, const OverlayConfig& c3,
    int ew, int eh)
{
    if (!nvg_) return;

    struct Entry { unsigned int tex; bool act; const OverlayConfig* cfg; };
    const Entry entries[3] = {
        { tex1, act1, &c1 }, { tex2, act2, &c2 }, { tex3, act3, &c3 }
    };

    bool any = false;
    for (auto& e : entries)
        if (e.act && e.tex) { any = true; break; }
    if (!any) return;

    const float fw = static_cast<float>(ew);
    const float fh = static_cast<float>(eh);

    const bool own_frame = !nvg_frame_active_;
    if (own_frame) nvgBeginFrame(nvg_, fw, fh, 1.0f);
    for (auto& e : entries)
        if (e.act && e.tex)
            draw_pip_nvg_single(nvg_, e.tex, *e.cfg, fw, fh);
    if (own_frame) {
        nvgEndFrame(nvg_);
        nvg_frame_active_ = false;
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glStencilMask(0xFF);
    }
}

void HudRenderer::draw_pip_overlays(
    unsigned int /*tex1*/, bool /*act1*/, const OverlayConfig& /*c1*/,
    unsigned int /*tex2*/, bool /*act2*/, const OverlayConfig& /*c2*/,
    unsigned int /*tex3*/, bool /*act3*/, const OverlayConfig& /*c3*/,
    int /*ew*/, int /*eh*/)
{
    // All pips are drawn as underlays before HUD chrome via draw_pip_underlays.
}

// ── PiP ──────────────────────────────────────────────────────────────────────

void HudRenderer::draw_pip(unsigned int /*tex*/, const char* /*label*/,
                            int /*w*/, int /*h*/, bool /*active*/,
                            const OverlayConfig& /*cfg*/,
                            const CameraFocusState& /*focus*/, bool /*nv_active*/) {
    // All pip rendering is now handled by draw_pip_underlays / draw_pip_overlays.
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
    // (no corner label)

    ImGui::End();
}

// ── Panel preview (FacePanel HUB75 live feed) ─────────────────────────────────

void HudRenderer::draw_panel_preview(unsigned int tex, int tex_w, int tex_h,
                                     int screen_w, int screen_h,
                                     float anchor_x, float anchor_y,
                                     float pan_x, float pan_y, float size_frac,
                                     int view) {
    if (tex == 0) return;
    if (tex_w <= 0 || tex_h <= 0) { tex_w = ShmFrameReader::W; tex_h = ShmFrameReader::H; }

    ImGui::SetCurrentContext(ctx_);

    // view: 0 = whole canvas, 1 = left half, 2 = right half. A half crops one
    // panel (one face) via UVs; width and aspect halve to match. Native
    // backends already crop to the face in pick_face_tex, so the caller
    // forces view=0 for those.
    const bool   half   = (view == 1 || view == 2);
    const float  wpx    = static_cast<float>(tex_w) * (half ? 0.5f : 1.0f);
    const float  aspect = wpx / static_cast<float>(tex_h);
    const ImVec2 uv0 = (view == 2) ? ImVec2(0.5f, 0.f) : ImVec2(0.f, 0.f);
    const ImVec2 uv1 = (view == 1) ? ImVec2(0.5f, 1.f) : ImVec2(1.f, 1.f);
    float ph = size_frac * static_cast<float>(screen_h);
    if (ph < 8.f) ph = 8.f;
    const float pw = ph * aspect;

    // Padding around the image inside the window.
    const float pad = 6.f;
    const float win_w = pw + pad * 2.f;
    const float win_h = ph + pad * 2.f;

    // Anchor fraction (0..1) places the window within the screen, then nudge.
    const float wx = anchor_x * (static_cast<float>(screen_w) - win_w) + pan_x;
    const float wy = anchor_y * (static_cast<float>(screen_h) - win_h) + pan_y;

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
                 img0, img1, uv0, uv1);

    // Accent border
    dl->AddRect({p.x, p.y}, {p.x + win_w, p.y + win_h},
                col_.accent, 4.f, 0, 1.5f);

    // Small "LED" label in the corner
    // (no corner label)

    ImGui::End();
}

// ── Protoface portrait beside the minimap ───────────────────────────────────────
// A scaled, one-side preview of the LED face placed to the left of the minimap
// (American Fugitive-style portrait). Drawn on the ImGui foreground list in display
// coords, matching the minimap geometry.
void HudRenderer::draw_face_portrait(unsigned int tex, int tex_w, int tex_h,
                                     bool tex_is_centred_face,
                                     int screen_w, int screen_h,
                                     const AppState& s) {
    if (tex == 0) return;
    const auto& cfg = s.map_overlay;
    if (!cfg.enabled || cfg.expanded) return;   // only alongside the live minimap
    if (tex_w <= 0 || tex_h <= 0) { tex_w = ShmFrameReader::W; tex_h = ShmFrameReader::H; }

    ImGui::SetCurrentContext(ctx_);

    // Minimap geometry (matches draw_map_overlay, display pixel coords).
    const float fw = static_cast<float>(screen_w);
    const float fh = static_cast<float>(screen_h);
    const float half = cfg.size_px;
    const float am   = (map_img_h_ > 0 && map_img_w_ > 0)
                       ? (float)map_img_h_ / (float)map_img_w_ : 1.f;
    const float hh = cfg.circle_window ? half : half * am;
    const float cx = std::clamp(fw * cfg.anchor_x + cfg.pan_x, half, fw - half);
    const float cy = std::clamp(fh * cfg.anchor_y + cfg.pan_y, hh, fh - hh);
    const float ringR = cfg.circle_window ? half : std::max(half, hh);

    // Source crop: HUB75 canvas is a mirrored-pair so we sample one half;
    // native backends arrive already cropped to the single face, so we
    // sample the whole texture and ignore the left/right portrait pick.
    const float u_lo   = tex_is_centred_face ? 0.f  : (cfg.portrait_right_half ? 0.5f : 0.f);
    const float u_hi   = tex_is_centred_face ? 1.f  : (cfg.portrait_right_half ? 1.f  : 0.5f);
    const float wpx    = static_cast<float>(tex_w) * (u_hi - u_lo);
    const float aspect = wpx / static_cast<float>(tex_h);
    const ImVec2 uv0   = {u_lo, 0.f};
    const ImVec2 uv1   = {u_hi, 1.f};

    const float ph    = ringR * 0.55f * std::max(0.5f, cfg.portrait_scale);  // portrait height
    const float pw    = ph * aspect;            // 2:1 width
    const float pad   = 6.f;
    const float win_w = pw + pad * 2.f;
    const float win_h = ph + pad * 2.f;
    // To the left of the map cluster, clearing the compass ring (and the
    // battery/system gauge when it's shown on that side).
    const float clear = cfg.battery_arc ? 76.f : 34.f;
    const float x0    = cx - ringR - clear - win_w;
    const float y0    = cy - win_h * 0.5f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled({x0, y0}, {x0 + win_w, y0 + win_h}, IM_COL32(8, 12, 18, 220), 6.f);
    dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                 {x0 + pad, y0 + pad}, {x0 + pad + pw, y0 + pad + ph}, uv0, uv1);
    dl->AddRect({x0, y0}, {x0 + win_w, y0 + win_h}, col_.accent, 6.f, 0, 1.5f);
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
    char lcam_lbl[32], rcam_lbl[32];
    const char* nv_tag = nv_enabled ? " NV" : "";
    snprintf(lcam_lbl, sizeof(lcam_lbl), "L.Cam%s%s", focus_suffix(focus_left),  nv_tag);
    snprintf(rcam_lbl, sizeof(rcam_lbl), "R.Cam%s%s", focus_suffix(focus_right), nv_tag);

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
    constexpr float diag_len = 90.f;  // fixed to match face/lora arms: (MAX_ROWS+1)*ROW_H

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
    nvgTextAlign(nvg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    const float arm_cx    = clock_anchor_x + ARM_EXT * 0.5f;
    const char* rows[2]   = { time_str, date_str };
    for (int i = 0; i < n_rows; ++i) {
        const float t  = static_cast<float>(i + 1) * eff_row_h;
        const float iy = anchor_y + dir_y * t;
        nvg_glow_text(vg, arm_cx, iy, rows[i], true, col_.glow_base, col_.text_fill);
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
    // Deep-Rock-style curved arc compass: ticks/labels laid along a shallow arc
    // (a large circle whose centre is far off-screen), with radial ticks, cardinal
    // letters, degree numbers, LoRa bearing markers, and a centre heading readout.
    const float heading  = s.compass_heading;
    const float ppd      = tw / 120.f;            // 120° visible across the width
    const float center_x = ox + tw / 2.f;
    const bool  flip     = cfg_.hud_flip_vertical;
    const float dir      = flip ? -1.f : 1.f;     // +1 = arc dips downward at edges

    // Arc geometry. Apex (peak) sits at the tape's outer edge so the ticks/labels
    // hang inward (down when bottom-anchored, up when flipped). Centre is R_big away.
    const float R_big  = tw * 2.6f;               // larger = flatter arc
    const float apex_y = flip ? (oy + th - 8.f) : (oy + 8.f);

    // Point on the arc at horizontal x, plus the local sin/cos of the subtended angle.
    auto arc_at = [&](float px, float& yy, float& sp, float& cp) {
        const float t = std::clamp((px - center_x) / R_big, -1.f, 1.f);
        const float phi = std::asin(t);
        sp = std::sin(phi); cp = std::cos(phi);
        yy = apex_y + dir * R_big * (1.f - cp);
    };

    const ImU32 col_major = col_.compass_tick;
    const ImU32 col_mid   = with_alpha(col_.compass_tick, 180);
    const ImU32 col_minor = with_alpha(col_.compass_tick, 110);
    const bool  tick_glow = cfg_.compass_tick_glow;

    const float t_maj = static_cast<float>(cfg_.compass_tick_length);
    const float t_mid = t_maj * (16.f / 24.f);
    const float t_min = t_maj * (10.f / 24.f);

    // ── Arc baseline (glow polyline) ─────────────────────────────────────────
    {
        constexpr int N = 48;
        float xs[N + 1], ys[N + 1], sp, cp;
        for (int i = 0; i <= N; ++i) {
            xs[i] = ox + tw * (float)i / N;
            arc_at(xs[i], ys[i], sp, cp);
        }
        auto poly = [&](NVGcolor c, float w) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, xs[0], ys[0]);
            for (int i = 1; i <= N; ++i) nvgLineTo(vg, xs[i], ys[i]);
            nvgStrokeColor(vg, c); nvgStrokeWidth(vg, w); nvgStroke(vg);
        };
        if (tick_glow) {
            poly(nvg_col_a(col_.glow_base, 28), 7.f);
            poly(nvg_col_a(col_.glow_base, 70), 4.f);
        }
        poly(nvg_col(col_.glow_base), 1.5f);
    }

    // ── Ticks (radial) batched per tier ──────────────────────────────────────
    auto tick_batch = [&](int mod, int skip_mod, float len, float w, NVGcolor col) {
        nvgBeginPath(vg);
        for (int deg = 0; deg < 360; ++deg) {
            if (deg % mod != 0) continue;
            if (skip_mod && deg % skip_mod == 0) continue;   // drawn by a higher tier
            float off = (float)deg - heading;
            while (off > 180.f) off -= 360.f;
            while (off < -180.f) off += 360.f;
            float px = center_x + off * ppd;
            if (px < ox || px > ox + tw) continue;
            float yy, sp, cp; arc_at(px, yy, sp, cp);
            nvgMoveTo(vg, px, yy);
            nvgLineTo(vg, px - sp * len, yy + dir * cp * len);   // inward (radial)
        }
        nvgStrokeColor(vg, col); nvgStrokeWidth(vg, w); nvgStroke(vg);
    };
    if (tick_glow) {
        tick_batch(45, 0,  t_maj, t_maj * 0.4f, nvg_col_a(col_.compass_glow, 50));
        tick_batch(10, 45, t_mid, t_mid * 0.4f, nvg_col_a(col_.compass_glow, 32));
    }
    tick_batch(5, 10, t_min, 2.f, nvg_col(col_minor));   // minor (5°, not 10°)
    tick_batch(10, 45, t_mid, 2.f, nvg_col(col_mid));     // mid   (10°, not 45°)
    tick_batch(45, 0,  t_maj, 3.f, nvg_col(col_major));   // major (45° cardinals)

    // ── Labels: cardinals (large) + degree numbers, below the ticks ──────────
    for (int deg = 0; deg < 360; ++deg) {
        const bool card = (deg % 45 == 0);
        const bool tens = (deg % 10 == 0);
        if (!card && !tens) continue;
        float off = (float)deg - heading;
        while (off > 180.f) off -= 360.f;
        while (off < -180.f) off += 360.f;
        float px = center_x + off * ppd;
        if (px < ox || px > ox + tw) continue;
        float yy, sp, cp; arc_at(px, yy, sp, cp);

        char buf[8];
        if (card) { strncpy(buf, cardinal_str((float)deg), 7); buf[7] = '\0'; }
        else      { snprintf(buf, sizeof(buf), "%d", deg); }

        const float gap = t_maj + (card ? 14.f : 11.f);
        const float lx  = px - sp * gap;
        const float ly  = yy + dir * cp * gap;
        nvg_set_font_mono(card ? 22.f : 0.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvg_glow_text(vg, lx, ly, buf, true, col_.glow_base, col_.text_fill);
    }

    // ── LoRa node bearing markers (triangles above the arc, pointing at it) ──
    for (const auto& node : s.lora_nodes) {
        if (node.distance_m <= 0.f) continue;
        float off = node.heading_deg - heading;
        while (off > 180.f) off -= 360.f;
        while (off < -180.f) off += 360.f;
        float px = center_x + off * ppd;
        if (px < ox || px > ox + tw) continue;
        float yy, sp, cp; arc_at(px, yy, sp, cp);
        const float ux = sp,  uy = -dir * cp;   // outward unit
        const float txu = cp, tyu = dir * sp;   // tangent unit
        const float ax = px + ux * 11.f, ay = yy + uy * 11.f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, px, yy);                       // tip on the arc
        nvgLineTo(vg, ax + txu * 5.f, ay + tyu * 5.f);
        nvgLineTo(vg, ax - txu * 5.f, ay - tyu * 5.f);
        nvgClosePath(vg);
        nvgFillColor(vg, nvg_col(s.lora_node_colors[node.local_id % 8]));
        nvgFill(vg);
        nvgStrokeColor(vg, nvg_col_a(s.lora_node_colors[node.local_id % 8], 200));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);
    }

    // ── Centre marker: forward chevron + heading readout box ─────────────────
    {
        const float ax = center_x, ay = apex_y;
        // Chevron pointing inward (at the arc) marking forward.
        nvgBeginPath(vg);
        nvgMoveTo(vg, ax - 7.f, ay - dir * 9.f);
        nvgLineTo(vg, ax + 7.f, ay - dir * 9.f);
        nvgLineTo(vg, ax,       ay);
        nvgClosePath(vg);
        nvgFillColor(vg, nvg_col(col_.glow_base));
        nvgFill(vg);

        // Heading readout box, outward from the arc (above when non-flipped).
        char hb[8];
        const int hd = ((int)lroundf(heading) % 360 + 360) % 360;
        snprintf(hb, sizeof(hb), "%d", hd);
        nvg_set_font_mono(15.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        float b[4]; nvgTextBounds(vg, 0, 0, hb, nullptr, b);
        const float bw = (b[2] - b[0]) + 16.f, bh = 20.f;
        const float bx = ax - bw * 0.5f;
        const float by = (dir > 0) ? (ay - 13.f - bh) : (ay + 13.f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx, by, bw, bh, 4.f);
        nvgFillColor(vg, nvg_col_a(col_.compass_bg_color, 210));
        nvgFill(vg);
        nvgStrokeColor(vg, nvg_col_a(col_.glow_base, 220));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);
        nvg_glow_text(vg, ax, by + bh * 0.5f, hb, true, col_.glow_base, col_.text_fill);
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
    // Collect unique base colors (strip alpha — we compute alpha per-particle from life)
    ImU32 unique_cols[32]; int n_unique = 0;
    for (int i = 0; i < n_particles_; ++i) {
        ImU32 base = particles_[i].color & 0x00FFFFFFu;
        bool found = false;
        for (int j = 0; j < n_unique; ++j) if (unique_cols[j] == base) { found = true; break; }
        if (!found && n_unique < 32) unique_cols[n_unique++] = base;
    }

    for (int ci = 0; ci < n_unique; ++ci) {
        ImU32 base = unique_cols[ci];
        const uint8_t r = base & 0xFF, g = (base>>8) & 0xFF, b = (base>>16) & 0xFF;

        // Pass 1 — bright core circles (batched, one nvgFill per color)
        nvgBeginPath(vg);
        uint8_t max_a = 0;
        for (int i = 0; i < n_particles_; ++i) {
            if ((particles_[i].color & 0x00FFFFFFu) != base) continue;
            const Particle& p = particles_[i];
            float frac = p.life / p.life_total;
            // Twinkle: brief fade-in, full brightness in mid-life, fade-out
            float af = (frac > 0.85f) ? (1.f - frac) / 0.15f
                     : (frac < 0.20f) ? frac / 0.20f : 1.f;
            uint8_t a = static_cast<uint8_t>(af * 230.f);
            if (a > max_a) max_a = a;
            nvgCircle(vg, p.x, p.y, p.size * 0.8f);
        }
        nvgFillColor(vg, nvgRGBA(r, g, b, max_a));
        nvgFill(vg);

        // Pass 2 — 6-pointed star rays (batched, one nvgStroke per color)
        // Each particle gets 3 axis-aligned line pairs through its center.
        nvgBeginPath(vg);
        uint8_t max_ray_a = 0;
        for (int i = 0; i < n_particles_; ++i) {
            if ((particles_[i].color & 0x00FFFFFFu) != base) continue;
            const Particle& p = particles_[i];
            float frac = p.life / p.life_total;
            float af = (frac > 0.85f) ? (1.f - frac) / 0.15f
                     : (frac < 0.20f) ? frac / 0.20f : 1.f;
            uint8_t a = static_cast<uint8_t>(af * 150.f);
            if (a > max_ray_a) max_ray_a = a;
            float arm  = p.size * 2.8f;    // long axis (H/V)
            float darm = arm   * 0.65f;    // diagonal arms (shorter)
            // Horizontal + vertical
            nvgMoveTo(vg, p.x - arm,  p.y);       nvgLineTo(vg, p.x + arm,  p.y);
            nvgMoveTo(vg, p.x,        p.y - arm);  nvgLineTo(vg, p.x,        p.y + arm);
            // 45° diagonals
            nvgMoveTo(vg, p.x - darm, p.y - darm); nvgLineTo(vg, p.x + darm, p.y + darm);
            nvgMoveTo(vg, p.x - darm, p.y + darm); nvgLineTo(vg, p.x + darm, p.y - darm);
        }
        nvgStrokeWidth(vg, 0.75f);
        nvgStrokeColor(vg, nvgRGBA(r, g, b, max_ray_a));
        nvgStroke(vg);
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

// Draw comets: radial-glow head + single linear-gradient tail stroke.
// 2 NVG paths per particle vs the old 4 — and the gradient fade looks more natural.
// TODO(perf): batch same-color comet tails into one nvgStrokePaint path
//             once NanoVG supports per-vertex paint coordinates.
void HudRenderer::fx_draw_lines(NVGcontext* vg) const {
    for (int i = 0; i < n_line_particles_; ++i) {
        const LineParticle& p = line_particles_[i];
        const float frac = p.life / p.life_total;
        float af = (frac > 0.85f) ? (1.f - frac) / 0.15f
                 : (frac < 0.25f) ? frac / 0.25f : 1.f;

        const float spd = std::sqrt(p.vx * p.vx + p.vy * p.vy);
        float dx = 1.f, dy = 0.f;
        if (spd > 0.01f) { dx = p.vx / spd; dy = p.vy / spd; }

        const float hx = p.x + dx * 3.f, hy = p.y + dy * 3.f;
        const float tx = p.x - dx * p.len, ty = p.y - dy * p.len;

        const uint8_t r = p.color & 0xFF;
        const uint8_t g = (p.color >> 8) & 0xFF;
        const uint8_t b = (p.color >> 16) & 0xFF;

        // Head: radial glow (bright core fading to transparent over 5 px)
        NVGpaint halo = nvgRadialGradient(vg, hx, hy, 0.6f, 5.f,
            nvgRGBA(r, g, b, static_cast<uint8_t>(af * 255.f)),
            nvgRGBA(r, g, b, 0));
        nvgBeginPath(vg);
        nvgCircle(vg, hx, hy, 5.f);
        nvgFillPaint(vg, halo);
        nvgFill(vg);

        // Tail: single gradient stroke, bright at head, fully transparent at tip
        NVGpaint tail = nvgLinearGradient(vg, hx, hy, tx, ty,
            nvgRGBA(r, g, b, static_cast<uint8_t>(af * 200.f)),
            nvgRGBA(r, g, b, 0));
        nvgBeginPath(vg);
        nvgMoveTo(vg, hx, hy);
        nvgLineTo(vg, tx, ty);
        nvgStrokeWidth(vg, 1.6f);
        nvgStrokePaint(vg, tail);
        nvgStroke(vg);
    }
}

// Layered dark-blue/violet gradient vignette along all four edges to evoke a nebula cloud.
void HudRenderer::fx_draw_nebula_cloud(NVGcontext* vg, float fw, float fh) const {
    // Each layer uses nvgBoxGradient over a fullscreen rect: one draw call covers
    // all four edges and corners simultaneously instead of four separate strips.
    // depth   — inner-box offset from screen edge (half the old values)
    // feather — gradient width; 1.4× depth gives a softer inner falloff
    struct CloudLayer { float depth; float feather; uint8_t r, g, b, a; };
    static const CloudLayer layers[] = {
        {  40.f,  56.f,  3,  2, 18, 215 },
        {  65.f,  91.f,  8,  4, 38, 130 },
        {  98.f, 137.f, 20,  8, 65,  55 },
    };
    for (const auto& l : layers) {
        const NVGcolor clear = nvgRGBA(l.r, l.g, l.b,    0);
        const NVGcolor edge  = nvgRGBA(l.r, l.g, l.b, l.a);
        NVGpaint p = nvgBoxGradient(vg, l.depth, l.depth,
                                    fw - 2.f*l.depth, fh - 2.f*l.depth,
                                    0.f, l.feather, clear, edge);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, fw, fh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
}

void HudRenderer::fx_draw_vignette(NVGcontext* vg, float fw, float fh) const {
    // Soft outer band: depth halved, feather 1.4× depth for a gentle inner fade
    {
        constexpr float d = 100.f, f = 140.f;
        NVGpaint p = nvgBoxGradient(vg, d, d, fw - 2.f*d, fh - 2.f*d,
                                    0.f, f, nvgRGBA(0,0,0,0), nvgRGBA(0,0,0,160));
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, fw, fh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
    // Hard inner ring: depth halved, feather scaled to match
    {
        constexpr float d = 28.f, f = 44.f;
        NVGpaint p = nvgBoxGradient(vg, d, d, fw - 2.f*d, fh - 2.f*d,
                                    0.f, f, nvgRGBA(0,0,0,0), nvgRGBA(0,0,0,220));
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

void HudRenderer::draw_sys_panel(const AppState& snap, int w, int h, bool active,
                                 float x_offset, bool narrow) {
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

    // Panel geometry — top-left, two columns. The debug readout outgrew a single
    // column once IMU + per-core CPU/GPU sections were added, so content flows
    // into a second column when the first fills up.
    constexpr float COLW  = 330.f;            // per-column content width
    constexpr float GAP   = 12.f;             // gap between columns
    // Narrow = single column (~half width); the content fits one column on a tall
    // display, so the second column's space is just empty there.
    const float PW = narrow ? COLW : (COLW * 2.f + GAP);
    constexpr float PAD   = 10.f;
    constexpr float MRG   = 14.f;             // left/top margin from screen edge
    const float px = MRG + x_offset;          // x_offset opens it right of the map sidebar
    const float py = MRG;
    const float PH = std::min(sh - MRG * 2.f, 1040.f);

    // Background + border
    dl->AddRectFilled({px, py}, {px + PW, py + PH}, col_.panel_bg, 6.f);
    dl->AddRect      ({px, py}, {px + PW, py + PH}, col_.primary,  6.f, 0, 1.5f);

    if (font_mono_) ImGui::PushFont(font_mono_);
    const float lh = ImGui::GetTextLineHeight();

    float cx = px;          // current column origin x
    float cy = py + PAD;    // current y cursor

    // Wrap into the second column once the first can't fit `need` more pixels.
    // Narrow mode keeps everything in one column (no second column).
    auto wrap_col = [&](float need) {
        if (!narrow && cx <= px + 0.5f && cy + need > py + PH - PAD) {
            cx = px + COLW + GAP;
            cy = py + PAD;
        }
    };

    // Helper: draw a section header + separator
    auto section = [&](const char* title) {
        wrap_col(60.f);
        hud_glow_text(dl, {cx + PAD, cy}, title, true, col_.primary, col_.primary);
        cy += lh + 2.f;
        dl->AddLine({cx + PAD, cy}, {cx + COLW - PAD, cy}, with_alpha(col_.primary, 80), 1.f);
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
        hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
        cy += lh + 4.f;
    }

    // CPU sparkline row
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "CPU  %3.0f%%", static_cast<double>(snap.sys_metrics.cpu_pct));
        hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
        const float spw = COLW - PAD * 2.f - 80.f;
        sparkline(snap.sys_metrics.cpu_history, kSysHistLen, snap.sys_metrics.history_head,
                  {cx + PAD + 78.f, cy}, spw, lh,
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
        hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
        const float spw = COLW - PAD * 2.f - 80.f;
        sparkline(snap.sys_metrics.ram_history, kSysHistLen, snap.sys_metrics.history_head,
                  {cx + PAD + 78.f, cy}, spw, lh,
                  total > 0.f ? total : 4096.f, col_.warn);
        cy += lh + 8.f;
    }

    // ── CPU CORES ───────────────────────────────────────────────────────────────
    // Per-logical-core utilisation bar + current frequency.
    section("CPU CORES");
    {
        const auto& m   = snap.sys_metrics;
        const int   n   = std::clamp(m.cpu_core_count, 0, kMaxCpuCores);
        if (m.cpu_temp_c > 0.f) {
            char tb[32];
            snprintf(tb, sizeof(tb), "Temp  %.1f C", static_cast<double>(m.cpu_temp_c));
            const bool hot = m.cpu_temp_c >= 75.f;
            hud_glow_text(dl, {cx + PAD, cy}, tb, hot,
                          col_.glow_base, hot ? col_.warn : col_.text_fill);
            cy += lh + 4.f;
        }
        const float bx = cx + PAD + 44.f;
        const float bw = COLW - PAD * 2.f - 44.f - 56.f;  // room for label + MHz
        for (int i = 0; i < n; ++i) {
            char cb[16];
            snprintf(cb, sizeof(cb), "C%-2d", i);
            hud_glow_text(dl, {cx + PAD, cy}, cb, false, col_.glow_base, col_.text_fill);
            const float frac = std::clamp(m.cpu_core_pct[i] / 100.f, 0.f, 1.f);
            const ImU32 barc = (frac > 0.85f) ? col_.warn : col_.accent;
            dl->AddRect      ({bx, cy + 2.f}, {bx + bw, cy + lh - 2.f},
                              with_alpha(col_.accent, 60));
            dl->AddRectFilled({bx, cy + 2.f}, {bx + bw * frac, cy + lh - 2.f}, barc);
            char vb[24];
            if (m.cpu_core_mhz[i] > 0.f)
                snprintf(vb, sizeof(vb), "%3.0f%% %4.0f", static_cast<double>(m.cpu_core_pct[i]),
                         static_cast<double>(m.cpu_core_mhz[i]));
            else
                snprintf(vb, sizeof(vb), "%3.0f%%", static_cast<double>(m.cpu_core_pct[i]));
            hud_glow_text(dl, {bx + bw + 6.f, cy}, vb, false, col_.glow_base, col_.text_dim);
            cy += lh + 3.f;
        }
        if (n == 0) {
            hud_glow_text(dl, {cx + PAD, cy}, "--", false, col_.glow_base, col_.text_dim);
            cy += lh + 4.f;
        }
        cy += 4.f;
    }

    // ── GPU ───────────────────────────────────────────────────────────────────
    // VideoCore per-domain clock breakdown + temperature (via vcgencmd).
    section("GPU");
    {
        const auto& g = snap.gpu;
        if (!g.available) {
            hud_glow_text(dl, {cx + PAD, cy}, "unavailable", false,
                          col_.glow_base, col_.text_dim);
            cy += lh + 4.f;
        } else {
            char gb[40];
            if (g.temp_c > 0.f) {
                const bool hot = g.temp_c >= 75.f;
                snprintf(gb, sizeof(gb), "Temp  %.1f C", static_cast<double>(g.temp_c));
                hud_glow_text(dl, {cx + PAD, cy}, gb, hot,
                              col_.glow_base, hot ? col_.warn : col_.text_fill);
                cy += lh + 4.f;
            }
            const int n = std::clamp(g.clock_count, 0, kMaxGpuClocks);
            for (int i = 0; i < n; ++i) {
                snprintf(gb, sizeof(gb), "  %-5s %4.0f MHz",
                         g.clocks[i].name, static_cast<double>(g.clocks[i].mhz));
                hud_glow_text(dl, {cx + PAD, cy}, gb, false, col_.glow_base, col_.text_fill);
                cy += lh + 2.f;
            }
            cy += 4.f;
        }
    }

    // ── IMU ───────────────────────────────────────────────────────────────────
    // Every value delivered by both IMUs, for fault-finding.
    section("IMU");
    {
        const auto& d = snap.imu_data;
        char ib[56];

        // VITURE XR glasses fused pose
        dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            d.xr_active ? col_.ind_good : col_.ind_inactive);
        snprintf(ib, sizeof(ib), "  XR  %s", d.xr_active ? "live" : "no data");
        hud_glow_text(dl, {cx + PAD + 4.f, cy}, ib, d.xr_active,
                      col_.glow_base, col_.text_fill);
        if (d.xr_active && d.xr_rate_hz > 0.f) {
            char rb[16];
            snprintf(rb, sizeof(rb), "%.0f Hz", static_cast<double>(d.xr_rate_hz));
            hud_glow_text(dl, {cx + COLW - PAD - 52.f, cy}, rb, false,
                          col_.glow_base, col_.text_dim);
        }
        cy += lh + 2.f;
        snprintf(ib, sizeof(ib), " R%6.1f P%6.1f Y%6.1f",
                 static_cast<double>(d.xr_roll), static_cast<double>(d.xr_pitch),
                 static_cast<double>(d.xr_yaw));
        hud_glow_text(dl, {cx + PAD, cy}, ib, false, col_.glow_base,
                      d.xr_active ? col_.text_fill : col_.text_dim);
        cy += lh + 6.f;

        // MPU-9250 raw 9-axis
        dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            d.mpu_ok ? col_.ind_good : col_.ind_fail);
        snprintf(ib, sizeof(ib), "  MPU-9250  %s", d.mpu_ok ? "ok" : "no data");
        hud_glow_text(dl, {cx + PAD + 4.f, cy}, ib, d.mpu_ok,
                      col_.glow_base, col_.text_fill);
        if (d.mpu_ok && d.mpu_rate_hz > 0.f) {
            char rb[16];
            snprintf(rb, sizeof(rb), "%.0f Hz", static_cast<double>(d.mpu_rate_hz));
            hud_glow_text(dl, {cx + COLW - PAD - 52.f, cy}, rb, false,
                          col_.glow_base, col_.text_dim);
        }
        cy += lh + 2.f;
        if (d.mpu_ok) {
            snprintf(ib, sizeof(ib), " Acc %6.2f %6.2f %6.2f g",
                     static_cast<double>(d.accel_g[0]), static_cast<double>(d.accel_g[1]),
                     static_cast<double>(d.accel_g[2]));
            hud_glow_text(dl, {cx + PAD, cy}, ib, false, col_.glow_base, col_.text_fill);
            cy += lh + 2.f;
            snprintf(ib, sizeof(ib), " Gyr %6.0f %6.0f %6.0f d/s",
                     static_cast<double>(d.gyro_dps[0]), static_cast<double>(d.gyro_dps[1]),
                     static_cast<double>(d.gyro_dps[2]));
            hud_glow_text(dl, {cx + PAD, cy}, ib, false, col_.glow_base, col_.text_fill);
            cy += lh + 2.f;
            snprintf(ib, sizeof(ib), " Mag %6.0f %6.0f %6.0f uT",
                     static_cast<double>(d.mag_ut[0]), static_cast<double>(d.mag_ut[1]),
                     static_cast<double>(d.mag_ut[2]));
            hud_glow_text(dl, {cx + PAD, cy}, ib, false, col_.glow_base, col_.text_fill);
            cy += lh + 2.f;
            snprintf(ib, sizeof(ib), " Hdg %5.1f deg    %4.1f C",
                     static_cast<double>(d.mpu_heading), static_cast<double>(d.temp_c));
            hud_glow_text(dl, {cx + PAD, cy}, ib, false, col_.glow_base, col_.text_fill);
            cy += lh + 2.f;
        }
        cy += 6.f;
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
        hud_glow_text(dl, {cx + PAD, cy}, buf, snap.wifi.connected,
                      col_.glow_base, col_.text_fill);
        cy += lh + 2.f;

        if (snap.wifi.connected) {
            // IP + signal bars
            const int dbm   = snap.wifi.signal_dbm;
            const int bars  = (dbm >= -50) ? 4 : (dbm >= -65) ? 3 : (dbm >= -75) ? 2 : 1;
            snprintf(buf, sizeof(buf), "  %s  %d dBm", snap.wifi.ip.c_str(), dbm);
            hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_fill);
            // Draw signal bars (4 rectangles of increasing height)
            const float bx = cx + COLW - PAD - 28.f;
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
        hud_glow_text(dl, {cx + PAD, cy}, buf, snap.ping.reachable,
                      col_.glow_base, col_.text_fill);
        const float spw = COLW - PAD * 2.f - 80.f;
        // Scale: 500 ms max
        sparkline(snap.ping.history, kPingHistLen, snap.ping.history_head,
                  {cx + PAD + 78.f, cy}, spw, lh,
                  500.f, snap.ping.reachable ? col_.primary : with_alpha(col_.primary, 80));
        cy += lh + 8.f;
    }

    // ── BLUETOOTH ─────────────────────────────────────────────────────────────
    section("BLUETOOTH");

    if (snap.bt_devices.empty()) {
        hud_glow_text(dl, {cx + PAD, cy}, "No devices", false, col_.glow_base, col_.text_dim);
        cy += lh + 4.f;
    } else {
        constexpr int MAX_BT = 4;
        int shown = 0;
        for (const auto& dev : snap.bt_devices) {
            if (shown >= MAX_BT) break;
            const ImU32 dot_col = dev.connected ? col_.ind_good : col_.ind_inactive;
            dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f, dot_col);
            char buf[64];
            snprintf(buf, sizeof(buf), "  %s", dev.name.c_str());
            hud_glow_text(dl, {cx + PAD + 4.f, cy}, buf, dev.connected,
                          col_.glow_base, col_.text_fill);
            cy += lh + 2.f;
            ++shown;
        }
        cy += 4.f;
    }

    // ── PERFORMANCE ───────────────────────────────────────────────────────────
    section("PERFORMANCE");

    // FPS (smoothed) + frame time sparkline
    {
        const float fps_s = snap.sys_metrics.fps_avg_smooth;
        const float fps_r = snap.sys_metrics.fps_avg;
        const float ft    = snap.sys_metrics.frame_time_ms;
        char buf[64];
        const float fps_show = fps_s > 0.f ? fps_s : fps_r;
        if (fps_show > 0.f)
            snprintf(buf, sizeof(buf), "FPS  %4.1f", static_cast<double>(fps_show));
        else
            snprintf(buf, sizeof(buf), "FPS  --");
        hud_glow_text(dl, {cx + PAD, cy}, buf, fps_show > 0.f, col_.glow_base, col_.text_fill);
        const float spw = COLW - PAD * 2.f - 80.f;
        // Sparkline: frame time in ms, capped at 50ms (20 FPS floor)
        sparkline(snap.sys_metrics.ft_history, kSysHistLen, snap.sys_metrics.ft_history_head,
                  {cx + PAD + 78.f, cy}, spw, lh, 50.f, col_.primary);
        cy += lh + 4.f;

        if (fps_r > 0.f && fps_s > 0.f)
            snprintf(buf, sizeof(buf), "Inst  %4.1f", static_cast<double>(fps_r));
        else
            snprintf(buf, sizeof(buf), "Inst  --");
        hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_dim);
        cy += lh + 2.f;

        if (ft > 0.f)
            snprintf(buf, sizeof(buf), "Frame  %.1fms", static_cast<double>(ft));
        else
            snprintf(buf, sizeof(buf), "Frame  --");
        hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_dim);
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
        dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            ok ? col_.ind_good : col_.ind_fail);
        hud_glow_text(dl, {cx + PAD + 4.f, cy}, buf, ok, col_.glow_base, col_.text_fill);
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
        dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            ok ? col_.ind_good : col_.ind_inactive);
        hud_glow_text(dl, {cx + PAD + 4.f, cy}, buf, ok, col_.glow_base, col_.text_fill);
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
        dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f,
                            ok ? col_.ind_good : col_.ind_inactive);
        hud_glow_text(dl, {cx + PAD + 4.f, cy}, buf, ok, col_.glow_base, col_.text_fill);
        cy += lh + 8.f;
    }

    // ── SSH ───────────────────────────────────────────────────────────────────
    {
        wrap_col(2.f * lh + 10.f);
        dl->AddLine({cx + PAD, cy}, {cx + COLW - PAD, cy}, with_alpha(col_.primary, 80), 1.f);
        cy += 5.f;
        char buf[32];
        if (snap.ssh.active) {
            snprintf(buf, sizeof(buf), "SSH  Active :%d", snap.ssh.port);
            dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f, col_.ind_good);
        } else {
            snprintf(buf, sizeof(buf), "SSH  Inactive");
            dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f, col_.ind_fail);
        }
        hud_glow_text(dl, {cx + PAD + 4.f, cy}, buf, snap.ssh.active,
                      col_.glow_base, col_.text_fill);
        cy += lh + 8.f;
    }

    // ── I2C ───────────────────────────────────────────────────────────────────
    section("I2C");
    {
        char buf[64];
        if (snap.i2c_scan_busy) {
            hud_glow_text(dl, {cx + PAD, cy}, "Scanning...", true, col_.glow_base, col_.warn);
            cy += lh + 4.f;
        } else if (snap.i2c_scan_results.empty()) {
            snprintf(buf, sizeof(buf), "%s  no scan", snap.i2c_scan_bus.c_str());
            hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_dim);
            cy += lh + 4.f;
        } else {
            snprintf(buf, sizeof(buf), "Bus: %s", snap.i2c_scan_bus.c_str());
            hud_glow_text(dl, {cx + PAD, cy}, buf, false, col_.glow_base, col_.text_dim);
            cy += lh + 2.f;
            constexpr int kCols = 6;
            int col_idx = 0;
            const float cell_w = (COLW - PAD * 2.f) / kCols;
            for (uint8_t addr : snap.i2c_scan_results) {
                snprintf(buf, sizeof(buf), "0x%02X", static_cast<int>(addr));
                const float ax = cx + PAD + col_idx * cell_w;
                dl->AddRectFilled({ax, cy}, {ax + cell_w - 2.f, cy + lh},
                                  with_alpha(col_.primary, 30), 2.f);
                hud_glow_text(dl, {ax + 2.f, cy}, buf, true, col_.glow_base, col_.text_fill);
                if (++col_idx >= kCols) { col_idx = 0; cy += lh + 3.f; }
            }
            if (col_idx > 0) cy += lh + 3.f;
            cy += 2.f;
        }
    }

    // ── GPIO ──────────────────────────────────────────────────────────────────
    if (!snap.gpio_states.empty()) {
        section("GPIO");
        for (const auto& ps : snap.gpio_states) {
            char buf[32];
            const bool hi  = (ps.value == 1);
            const bool na  = (ps.value < 0);
            const ImU32 vc = na ? col_.ind_inactive : (hi ? col_.ind_good : col_.text_dim);
            snprintf(buf, sizeof(buf), "  GPIO%-3d  %s", ps.pin,
                     na ? "N/A" : (hi ? "HI" : "LO"));
            dl->AddCircleFilled({cx + PAD + 5.f, cy + lh * 0.5f}, 4.f, vc);
            hud_glow_text(dl, {cx + PAD + 4.f, cy}, buf, !na, col_.glow_base, vc);
            cy += lh + 3.f;
        }
        cy += 2.f;
    }

    if (font_mono_) ImGui::PopFont();

    ImGui::End();
}


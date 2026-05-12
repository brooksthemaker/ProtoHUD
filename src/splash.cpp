#define STB_IMAGE_IMPLEMENTATION
#include "splash.h"

#include <stb_image.h>
#include <GLES2/gl2.h>
#include <cmath>
#include <cstring>
#include <algorithm>

static constexpr float kPi = 3.14159265f;

SplashScreen::SplashScreen(const SplashConfig& cfg) : cfg_(cfg) {}

SplashScreen::~SplashScreen() {
    if (logo_tex_) glDeleteTextures(1, &logo_tex_);
}

bool SplashScreen::load_image(const std::string& path) {
    if (path.empty()) return false;
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) return false;
    glGenTextures(1, &logo_tex_);
    glBindTexture(GL_TEXTURE_2D, logo_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    logo_w_ = w;
    logo_h_ = h;
    return true;
}

void SplashScreen::draw_eye(ImDrawList* dl,
                             float eye_x, float eye_y,
                             float eye_w, float eye_h,
                             float t, const char* status, float progress,
                             ImFont* font) const {
    // ── Background ────────────────────────────────────────────────────────────
    dl->AddRectFilled({eye_x, eye_y}, {eye_x + eye_w, eye_y + eye_h},
                      IM_COL32(6, 8, 10, 255));

    const float cx = eye_x + eye_w * 0.5f;
    const float cy = eye_y + eye_h * 0.42f;  // slightly above centre

    const ImU32 COL_PRIMARY = IM_COL32(0, 220, 180, 255);
    const ImU32 COL_DIM     = IM_COL32(0, 160, 130, 160);
    const ImU32 COL_TEXT    = IM_COL32(200, 240, 230, 255);
    const ImU32 COL_SUBDIM  = IM_COL32(120, 160, 150, 200);

    constexpr float RING_R = 96.f;

    // ── Spinning dots ring ────────────────────────────────────────────────────
    // Subtle track circle
    dl->AddCircle({cx, cy}, RING_R, IM_COL32(0, 100, 80, 30), 64, 1.5f);

    if (cfg_.animated) {
        constexpr int   N_DOTS = 8;
        constexpr float DOT_R  = 5.f;

        for (int i = 0; i < N_DOTS; ++i) {
            float angle = (i * 2.f * kPi / N_DOTS) + t * 1.5f;
            float dx = cx + RING_R * std::cos(angle);
            float dy = cy + RING_R * std::sin(angle);

            // Phase: how close is this dot to being the "lead" (0 = lead, 1 = tail)
            float norm_i   = static_cast<float>(i) / N_DOTS;
            float lead_pos = fmod(t * 1.5f / (2.f * kPi), 1.f);
            float diff     = fmod(norm_i - lead_pos + 1.f, 1.f);
            float brightness = std::pow(1.f - diff, 3.5f);

            uint8_t a  = static_cast<uint8_t>(30.f + brightness * 225.f);
            float   r  = DOT_R * (0.45f + brightness * 0.8f);
            dl->AddCircleFilled({dx, dy}, r + 5.f, IM_COL32(0, 220, 180, static_cast<uint8_t>(a / 5)));
            dl->AddCircleFilled({dx, dy}, r,        IM_COL32(0, 220, 180, a));
        }
    } else {
        // Static ring
        dl->AddCircle({cx, cy}, RING_R, COL_DIM, 64, 1.5f);
    }

    // ── Logo or procedural hexagon mark ───────────────────────────────────────
    constexpr float LOGO_HALF = 52.f;
    if (logo_tex_) {
        float aspect = logo_w_ > 0 ? static_cast<float>(logo_w_) / static_cast<float>(logo_h_) : 1.f;
        float lw, lh;
        if (aspect >= 1.f) { lw = LOGO_HALF * 2.f; lh = lw / aspect; }
        else               { lh = LOGO_HALF * 2.f; lw = lh * aspect; }
        dl->AddImage(
            reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(logo_tex_)),
            {cx - lw * 0.5f, cy - lh * 0.5f},
            {cx + lw * 0.5f, cy + lh * 0.5f});
    } else {
        // Procedural mark: outer hexagon + inner circle + "PH"
        constexpr int   HEX_N = 6;
        constexpr float HEX_R = 46.f;
        ImVec2 hex[HEX_N];
        for (int i = 0; i < HEX_N; ++i) {
            float a = i * 2.f * kPi / HEX_N - kPi / 6.f;
            hex[i] = {cx + HEX_R * std::cos(a), cy + HEX_R * std::sin(a)};
        }
        dl->AddPolyline(hex, HEX_N, COL_PRIMARY, ImDrawFlags_Closed, 2.f);
        dl->AddCircleFilled({cx, cy}, 28.f, IM_COL32(0, 220, 180, 18));
        if (font) {
            ImVec2 ts = font->CalcTextSizeA(20.f, FLT_MAX, 0.f, "PH");
            dl->AddText(font, 20.f, {cx - ts.x * 0.5f, cy - ts.y * 0.5f}, COL_PRIMARY, "PH");
        } else {
            ImVec2 ts = ImGui::CalcTextSize("PH");
            dl->AddText({cx - ts.x * 0.5f, cy - ts.y * 0.5f}, COL_PRIMARY, "PH");
        }
    }

    // ── Title ─────────────────────────────────────────────────────────────────
    const float title_y = cy + RING_R + 22.f;
    if (!cfg_.title.empty()) {
        const char* txt = cfg_.title.c_str();
        if (font) {
            ImVec2 ts = font->CalcTextSizeA(26.f, FLT_MAX, 0.f, txt);
            dl->AddText(font, 26.f, {cx - ts.x * 0.5f, title_y}, COL_TEXT, txt);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText({cx - ts.x * 0.5f, title_y}, COL_TEXT, txt);
        }
    }

    // ── Subtitle ──────────────────────────────────────────────────────────────
    if (!cfg_.subtitle.empty()) {
        const char* txt = cfg_.subtitle.c_str();
        if (font) {
            ImVec2 ts = font->CalcTextSizeA(14.f, FLT_MAX, 0.f, txt);
            dl->AddText(font, 14.f, {cx - ts.x * 0.5f, title_y + 30.f}, COL_SUBDIM, txt);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText({cx - ts.x * 0.5f, title_y + 30.f}, COL_SUBDIM, txt);
        }
    }

    // ── Status text ───────────────────────────────────────────────────────────
    if (status && *status) {
        float status_y = title_y + (cfg_.subtitle.empty() ? 26.f : 52.f);
        if (font) {
            ImVec2 ts = font->CalcTextSizeA(13.f, FLT_MAX, 0.f, status);
            dl->AddText(font, 13.f, {cx - ts.x * 0.5f, status_y},
                        IM_COL32(100, 140, 130, 200), status);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(status);
            dl->AddText({cx - ts.x * 0.5f, status_y},
                        IM_COL32(100, 140, 130, 200), status);
        }
    }

    // ── Progress bar ──────────────────────────────────────────────────────────
    const float bar_margin = eye_w * 0.15f;
    const float bar_x      = eye_x + bar_margin;
    const float bar_w      = eye_w - 2.f * bar_margin;
    const float bar_y      = eye_y + eye_h - 52.f;
    const float bar_h      = 4.f;

    // Track
    dl->AddRectFilled({bar_x, bar_y}, {bar_x + bar_w, bar_y + bar_h},
                      IM_COL32(20, 40, 35, 200), 2.f);
    // Fill
    float fill_w = bar_w * std::min(1.f, progress);
    if (fill_w > 1.f) {
        dl->AddRectFilled({bar_x, bar_y}, {bar_x + fill_w, bar_y + bar_h},
                          IM_COL32(0, 220, 180, 200), 2.f);
        // Glow pulse at leading edge
        if (cfg_.animated) {
            float pulse = 0.5f + 0.5f * std::sin(t * 5.f);
            uint8_t ga  = static_cast<uint8_t>(50.f + pulse * 130.f);
            float   gx  = bar_x + fill_w;
            dl->AddRectFilled({gx - 14.f, bar_y - 3.f}, {gx + 3.f, bar_y + bar_h + 3.f},
                              IM_COL32(0, 220, 180, ga), 2.f);
        }
    }

    // Thin border line along bottom of eye area
    dl->AddLine({eye_x, eye_y + eye_h - 1.f}, {eye_x + eye_w, eye_y + eye_h - 1.f},
                IM_COL32(0, 80, 60, 60), 1.f);
}

void SplashScreen::draw(ImDrawList* dl, float sw, float sh, float t,
                         const char* status, float progress, ImFont* font) const {
    if (!cfg_.enabled) return;
    const float eye_w = sw * 0.5f;
    draw_eye(dl, 0.f,    0.f, eye_w, sh, t, status, progress, font);
    draw_eye(dl, eye_w,  0.f, eye_w, sh, t, status, progress, font);

    // Thin centre divider matching the glasses bezzel
    dl->AddLine({eye_w, 0.f}, {eye_w, sh}, IM_COL32(0, 60, 45, 80), 1.f);
}

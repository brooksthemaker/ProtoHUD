#include "menu_system.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <string>

static std::string to_upper(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (unsigned char c : s) r += static_cast<char>(::toupper(c));
    return r;
}

MenuSystem::MenuSystem(std::vector<MenuItem> root)
    : root_items_(std::move(root)) {}

void MenuSystem::push_level(const std::vector<MenuItem>& items) {
    if (items.empty()) return;
    std::vector<MenuItem> aug = items;
    // Back only makes sense when there is a parent level
    if (!stack_.empty())
        aug.push_back({ "< Back", [this]{ back();  }, {} });
    aug.push_back(    { "Exit",   [this]{ close(); }, {} });
    stack_.push_back({ std::move(aug) });
    cursor_ = 0;
    emit_detents();
}

void MenuSystem::pop_level() {
    if (stack_.size() > 1) {
        stack_.pop_back();
        cursor_ = 0;
        emit_detents();
    } else {
        close();
    }
}

void MenuSystem::emit_detents() {
    if (detent_cb_ && !stack_.empty())
        detent_cb_(static_cast<int>(stack_.back().items.size()));
}

void MenuSystem::navigate(int direction) {
    if (!open_ || stack_.empty()) return;
    int n = static_cast<int>(stack_.back().items.size());
    cursor_ = ((cursor_ + direction) % n + n) % n;
}

void MenuSystem::select() {
    if (!open_ || stack_.empty()) return;
    auto& items = stack_.back().items;
    if (cursor_ >= static_cast<int>(items.size())) return;
    auto& item = items[cursor_];

    if (!item.children.empty()) {
        push_level(item.children);
    } else if (item.action) {
        item.action();
        // Menu stays open — user explicitly navigates away via Back or Exit.
    }
}

void MenuSystem::back() { pop_level(); }

const std::string& MenuSystem::current_label() const {
    static std::string empty;
    if (stack_.empty()) return empty;
    const auto& items = stack_.back().items;
    if (cursor_ < static_cast<int>(items.size()))
        return items[cursor_].label;
    return empty;
}

// ── draw ──────────────────────────────────────────────────────────────────────

// Derive alpha-variant of an ImU32 (format ABGR, alpha in high byte).
static ImU32 menu_with_alpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24u);
}

// Convert ImU32 to ImVec4, optionally overriding alpha.
static ImVec4 col_to_vec4(ImU32 col, float alpha_override = -1.f) {
    float r = ((col >>  0) & 0xFF) / 255.f;
    float g = ((col >>  8) & 0xFF) / 255.f;
    float b = ((col >> 16) & 0xFF) / 255.f;
    float a = alpha_override >= 0.f ? alpha_override : ((col >> 24) & 0xFF) / 255.f;
    return {r, g, b, a};
}

// Draw text with a glow outline using the supplied accent color.
// selected = full fill + bright glow; unselected = dim fill + faint glow.
static void draw_glow_text(ImDrawList* dl, ImVec2 pos, const char* text,
                            bool selected, ImU32 accent_col) {
    const ImU32 glow     = selected ? menu_with_alpha(accent_col, 72) : menu_with_alpha(accent_col, 22);
    const ImU32 glow_far = menu_with_alpha(accent_col, 28);
    const ImU32 fill_sel = IM_COL32(255, 255, 255, 255);
    const ImU32 fill_dim = IM_COL32(255, 255, 255, 160);
    const ImU32 fill     = selected ? fill_sel : fill_dim;

    constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
    constexpr int D2[4][2] = {{-2,0},{2,0},{0,-2},{0,2}};
    for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    if (selected)
        for (auto& o : D2) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow_far, text);
    dl->AddText(pos, fill, text);
}

void MenuSystem::draw(int screen_w, int screen_h) {
    if (!open_ || stack_.empty()) return;

    const auto& items  = stack_.back().items;
    const float item_h = 38.f;
    const float pad_x  = 18.f;
    const float pad_y  = 14.f;
    const float width  = 360.f;
    const float total_h = pad_y * 2.f
                          + item_h * static_cast<float>(items.size());

    const float x = 48.f;
    const float y = ((float)screen_h - total_h) * 0.5f;

    const ImU32 COL_SEP = menu_with_alpha(accent_color_, 45);

    ImGui::SetNextWindowPos ({ x, y }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ width, total_h }, ImGuiCond_Always);
    if (!bg_enabled_) ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      bg_color_);
    ImGui::PushStyleColor(ImGuiCol_Border,        col_to_vec4(accent_color_, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Header,        col_to_vec4(accent_color_, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col_to_vec4(accent_color_, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  col_to_vec4(accent_color_, 0.32f));
    // Suppress Selectable's own text — we draw it manually via DrawList
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0.f, 0.f));

    ImGui::Begin("##menu", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float line_h = ImGui::GetTextLineHeight();
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        bool selected = (i == cursor_);

        char id[32]; snprintf(id, sizeof(id), "##item%d", i);
        if (ImGui::Selectable(id, selected, 0,
                              ImVec2(0.f, item_h - 1.f))) {
            cursor_ = i;
            select();
        }

        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();

        // Left accent bar
        if (selected)
            dl->AddRectFilled({rmin.x - pad_x,       rmin.y},
                              {rmin.x - pad_x + 4.f,  rmax.y}, accent_color_);

        // Glow text — vertically centered in the item row
        std::string label = to_upper(items[i].label);
        if (!items[i].children.empty()) label += "   >";
        ImVec2 tpos = {rmin.x + 4.f, rmin.y + (item_h - line_h) * 0.5f - 0.5f};
        draw_glow_text(dl, tpos, label.c_str(), selected, accent_color_);

        // Radio indicator for toggle items (right edge of row)
        if (items[i].get_state) {
            bool on      = items[i].get_state();
            const float r  = 5.f;
            const float cx = rmax.x - 10.f;
            const float cy = rmin.y + item_h * 0.5f;
            if (on) {
                dl->AddCircleFilled({cx, cy}, r,    accent_color_);
                dl->AddCircleFilled({cx, cy}, 2.5f, IM_COL32(255, 255, 255, 255));
            } else {
                dl->AddCircle({cx, cy}, r, menu_with_alpha(accent_color_, 60), 0, 1.5f);
            }
        }

        // Thin bottom separator
        dl->AddLine({rmin.x - pad_x, rmax.y - 1.f},
                    {rmax.x + pad_x, rmax.y - 1.f}, COL_SEP, 1.f);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
}

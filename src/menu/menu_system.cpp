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
    stack_.push_back({ items });
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
        close();
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

// Draw text with an orange glow outline, matching the compass tick style.
// selected = full white + bright glow; unselected = dim white + faint glow.
static void draw_glow_text(ImDrawList* dl, ImVec2 pos, const char* text, bool selected) {
    constexpr ImU32 GLOW_SEL = IM_COL32(255, 160, 32,  72);
    constexpr ImU32 GLOW_DIM = IM_COL32(255, 160, 32,  22);
    constexpr ImU32 FILL_SEL = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 FILL_DIM = IM_COL32(255, 255, 255, 160);

    const ImU32 glow = selected ? GLOW_SEL : GLOW_DIM;
    const ImU32 fill = selected ? FILL_SEL : FILL_DIM;

    // 8-direction glow at 1px, 4-cardinal at 2px
    constexpr int D1[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
    constexpr int D2[4][2] = {{-2,0},{2,0},{0,-2},{0,2}};
    for (auto& o : D1) dl->AddText({pos.x+o[0], pos.y+o[1]}, glow, text);
    if (selected)
        for (auto& o : D2) dl->AddText({pos.x+o[0], pos.y+o[1]},
                                       IM_COL32(255,160,32,28), text);
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

    // Left-side positioning, vertically centered
    const float x = 48.f;
    const float y = ((float)screen_h - total_h) * 0.5f;

    // Compass orange palette
    constexpr ImU32 COL_ORANGE = IM_COL32(255, 160,  32, 255);
    constexpr ImU32 COL_SEP    = IM_COL32(255, 160,  32,  45);

    ImGui::SetNextWindowBringToDisplayFront();
    ImGui::SetNextWindowPos ({ x, y }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ width, total_h }, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(1.f, 0.627f, 0.125f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(1.f, 0.627f, 0.125f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.f, 0.627f, 0.125f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.f, 0.627f, 0.125f, 0.32f));
    // Suppress Selectable's own text — we draw it manually via DrawList
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0.f, 0.f));

    ImGui::Begin("##menu", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Item list
    const float line_h = ImGui::GetTextLineHeight();
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        bool selected = (i == cursor_);

        // Empty-label selectable — provides background highlight + click detection
        char id[32]; snprintf(id, sizeof(id), "##item%d", i);
        if (ImGui::Selectable(id, selected,
                              ImGuiSelectableFlags_SpanAvailWidth,
                              ImVec2(0.f, item_h - 1.f))) {
            cursor_ = i;
            select();
        }

        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();

        // Left accent bar
        if (selected)
            dl->AddRectFilled({rmin.x - pad_x,       rmin.y},
                              {rmin.x - pad_x + 4.f,  rmax.y}, COL_ORANGE);

        // Glow text — vertically centered in the item row
        std::string label = to_upper(items[i].label);
        if (!items[i].children.empty()) label += "   >";
        ImVec2 tpos = {rmin.x + 4.f, rmin.y + (item_h - line_h) * 0.5f - 0.5f};
        draw_glow_text(dl, tpos, label.c_str(), selected);

        // Thin bottom separator
        dl->AddLine({rmin.x - pad_x, rmax.y - 1.f},
                    {rmax.x + pad_x, rmax.y - 1.f}, COL_SEP, 1.f);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

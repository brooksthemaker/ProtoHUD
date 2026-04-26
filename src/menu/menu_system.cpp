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

void MenuSystem::draw(int screen_w, int screen_h) {
    if (!open_ || stack_.empty()) return;

    const auto& items  = stack_.back().items;
    const float item_h = 38.f;
    const float pad_x  = 18.f;
    const float pad_y  = 14.f;
    const float width  = 360.f;
    const float crumb_h = 32.f;
    const float total_h = pad_y * 2.f + crumb_h
                          + item_h * static_cast<float>(items.size());

    // Left-side positioning, vertically centered
    const float x = 48.f;
    const float y = ((float)screen_h - total_h) * 0.5f;

    ImGui::SetNextWindowBringToDisplayFront();
    ImGui::SetNextWindowPos ({ x, y }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ width, total_h }, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.f, 0.78f, 0.62f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.f, 0.55f, 0.43f, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.f, 0.55f, 0.43f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.f, 0.55f, 0.43f, 0.42f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0.f, 0.f));

    ImGui::Begin("##menu", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Breadcrumb trail
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.71f, 0.55f, 0.72f));
    for (size_t d = 0; d < stack_.size(); d++) {
        if (d > 0) { ImGui::SameLine(); ImGui::TextUnformatted("  >  "); ImGui::SameLine(); }
        ImGui::TextUnformatted(d == 0 ? "MENU" : stack_[d - 1].items[0].label.c_str());
    }
    ImGui::PopStyleColor();

    // Thin teal separator under breadcrumb
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine({x, p.y + 4.f}, {x + width, p.y + 4.f},
                    IM_COL32(0, 180, 140, 80), 1.f);
        ImGui::Dummy({0.f, 12.f});
    }

    // Item list
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        bool selected = (i == cursor_);

        ImGui::PushStyleColor(ImGuiCol_Text, selected
            ? ImVec4(1.f, 1.f, 1.f, 1.f)
            : ImVec4(0.72f, 0.86f, 0.80f, 0.80f));

        std::string label = to_upper(items[i].label);
        if (!items[i].children.empty()) label += "   >";

        if (ImGui::Selectable(label.c_str(), selected,
                              ImGuiSelectableFlags_SpanAvailWidth,
                              ImVec2(0.f, item_h - 1.f))) {
            cursor_ = i;
            select();
        }

        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();

        // Left accent bar on selected item (drawn in the left-padding zone)
        if (selected) {
            dl->AddRectFilled({rmin.x - pad_x,      rmin.y},
                              {rmin.x - pad_x + 4.f, rmax.y},
                              IM_COL32(0, 220, 180, 255));
        }

        // Thin bottom separator between items
        dl->AddLine({rmin.x - pad_x, rmax.y - 1.f},
                    {rmax.x + pad_x, rmax.y - 1.f},
                    IM_COL32(0, 180, 140, 38), 1.f);

        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
}

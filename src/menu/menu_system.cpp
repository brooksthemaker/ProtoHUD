#include "menu_system.h"

#include <imgui.h>
#include <algorithm>

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
    const float item_h = 44.f;
    const float pad    = 12.f;
    const float width  = 380.f;
    const float total_h = pad * 2.f + item_h * static_cast<float>(items.size())
                          + 28.f;  // extra for breadcrumb

    float x = (screen_w  - width)   / 2.f;
    float y = (screen_h  - total_h) / 2.f;

    // Frameless, non-moving, non-resizable overlay window
    ImGui::SetNextWindowPos ({ x - 4.f, y - 30.f }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ width + 8.f, total_h + 34.f }, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.78f, 0.62f, 0.86f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(pad, pad));

    ImGui::Begin("##menu", nullptr, flags);

    // Breadcrumb trail
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.71f, 0.55f, 0.72f));
    for (size_t d = 0; d < stack_.size(); d++) {
        if (d > 0) { ImGui::SameLine(); ImGui::TextUnformatted("  >  "); ImGui::SameLine(); }
        ImGui::TextUnformatted(d == 0 ? "MENU" : stack_[d - 1].items[0].label.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Item list
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        bool selected = (i == cursor_);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.f, 0.55f, 0.43f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.90f, 0.86f, 1.f));
        }

        std::string label = items[i].label;
        if (!items[i].children.empty()) label += "  \xe2\x80\xba";  // UTF-8 ›

        // Use Selectable so we can detect clicks (keyboard nav handled outside)
        if (ImGui::Selectable(label.c_str(), selected,
                              0, ImVec2(0.f, item_h - 4.f))) {
            cursor_ = i;
            select();
        }

        ImGui::PopStyleColor(selected ? 2 : 1);
        ImGui::Spacing();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

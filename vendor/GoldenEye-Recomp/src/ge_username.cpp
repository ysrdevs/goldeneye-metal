// ge - ReXGlue Recompiled Project
//
// First-boot username prompt implementation. See ge_username.h.

#include "ge_username.h"

#include <rex/cvar.h>

#include <imgui.h>

namespace ge {

UsernameDialog::UsernameDialog(rex::ui::ImGuiDrawer* drawer,
                               std::function<void(const std::string&)> on_set)
    : rex::ui::ImGuiDialog(drawer), on_set_(std::move(on_set)) {}

UsernameDialog::~UsernameDialog() = default;

void UsernameDialog::OnDraw(ImGuiIO& io) {
  if (done_) {
    return;
  }
  // Let the game settle into boot before popping up.
  if (delay_ < 120) {
    ++delay_;
    return;
  }
  // Resolve once (config is loaded by now): if a name was already chosen on a
  // previous boot, never show again.
  if (!checked_) {
    checked_ = true;
    if (rex::cvar::GetFlagByName("ge_username") != "User") {
      done_ = true;
      return;
    }
  }

  // Dim the scene behind the box (modal feel) without pausing the game.
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                IM_COL32(0, 0, 0, 150));

  ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f));

  // Briefing-folder styling (manila box, dark ink, red action button).
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.84f, 0.78f, 0.62f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.10f, 0.06f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.92f, 0.82f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.14f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.18f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.14f, 0.10f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.24f, 0.14f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 20));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

  ImGui::Begin("##ge_username", nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::SetWindowFontScale(1.5f);

  ImGui::TextUnformatted("ENTER YOUR NAME");
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.24f, 0.14f, 1.0f));
  ImGui::TextUnformatted("Shown to other players online.");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  if (focus_) {
    ImGui::SetKeyboardFocusHere();
    focus_ = false;
  }
  ImGui::SetNextItemWidth(-1.0f);
  bool entered = ImGui::InputText("##name", buf_, sizeof(buf_),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::Spacing();

  bool empty = buf_[0] == '\0';
  if (empty) {
    ImGui::BeginDisabled();
  }
  bool ok = ImGui::Button("OK", ImVec2(140.0f, 0.0f));
  if (empty) {
    ImGui::EndDisabled();
  }

  if ((entered || ok) && !empty) {
    if (on_set_) {
      on_set_(std::string(buf_));
    }
    done_ = true;
  }

  ImGui::SetWindowFontScale(1.0f);
  ImGui::End();

  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(7);
}

}  // namespace ge

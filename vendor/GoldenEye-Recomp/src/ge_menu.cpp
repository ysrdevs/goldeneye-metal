// ge - ReXGlue Recompiled Project
//
// In-game pause menu implementation. See ge_menu.h.
//
// Drawn procedurally for now (flat fills + outlines that approximate the
// manila-folder briefing screen). Every visual block is marked `// TEXTURE:`
// so it can be swapped for an ImGui::Image() once the Photoshop art exists --
// the layout geometry stays the same.

#include "ge_menu.h"
#include "ge_host_pause.h"
#include "ge_postfx.h"
#include "ge_testing_tools.h"

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/perf/metal_performance.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>  // VirtualKeyToString (key rebinding)
#include <rex/ui/virtual_key.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// While the menu is waiting to capture a rebind key, ge_inject_keyboard swallows
// all game input so the key being bound can't act. Implemented in ge_hooks.cpp.
namespace ge {
void SetRebindCapturing(bool);
}

namespace {

// --- Small cvar accessors (the menu reads/writes settings by name) ---
float GetCvarF(const char* name) {
  return static_cast<float>(std::atof(rex::cvar::GetFlagByName(name).c_str()));
}
void SetCvarF(const char* name, float v) {
  rex::cvar::SetFlagByName(name, std::to_string(v));
}
bool GetCvarB(const char* name) {
  return rex::cvar::GetFlagByName(name) == "true";
}
void SetCvarB(const char* name, bool v) {
  rex::cvar::SetFlagByName(name, v ? "true" : "false");
}
std::string GetCvarS(const char* name) {
  return rex::cvar::GetFlagByName(name);
}
void SetCvarS(const char* name, std::string_view v) {
  rex::cvar::SetFlagByName(name, v);
}

// --- Briefing palette (sampled from the reference screenshot) ---
constexpr ImU32 kFolder = IM_COL32(214, 201, 162, 255);     // manila paper
constexpr ImU32 kFolderEdge = IM_COL32(120, 108, 78, 255);  // darker rim
constexpr ImU32 kTab = IM_COL32(196, 184, 146, 255);        // unselected tab
constexpr ImU32 kTabSel = IM_COL32(224, 213, 176, 255);     // selected tab
constexpr ImU32 kInk = IM_COL32(52, 44, 32, 255);           // body text
constexpr ImU32 kInkDim = IM_COL32(92, 82, 60, 255);        // secondary text
constexpr ImU32 kTitle = IM_COL32(40, 33, 24, 255);         // big serif title
constexpr ImU32 kReticle = IM_COL32(196, 36, 28, 255);      // red selection crosshair
constexpr ImU32 kStamp = IM_COL32(176, 42, 34, 70);         // faded CLASSIFIED stamp

struct Tab {
  const char* label;  // short, drawn vertically on the tab
  const char* title;  // big serif heading shown in the body
};

enum TabIndex {
  kAudioTab = 0,
  kVideoTab,
  kInputTab,
#if !defined(__APPLE__)
  kOnlineTab,
#endif
  kTestingTab,
  kSystemTab,
  kTabCountValue,
};

constexpr int kTabCount = static_cast<int>(kTabCountValue);

constexpr Tab kTabs[] = {
    {"AUDIO", "AUDIO"},   {"VIDEO", "VIDEO"},   {"INPUT", "CONTROLS"},
#if !defined(__APPLE__)
    {"ONLINE", "ONLINE"},
#endif
    {"TOOLS", "TESTING"}, {"SYSTEM", "SYSTEM"},
};
static_assert(static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0])) == kTabCount);

// ImGui's DisplaySize is expressed in logical window coordinates. On a Retina
// display the Metal drawable is larger, but the immediate renderer scales this
// coordinate space to it, so menu geometry must stay in logical pixels too.
// Cap the folder at larger logical resolutions rather than letting its explicit
// title/tab fonts grow without bound.
constexpr float kReferenceFolderHeight = 590.0f;  // 82% of a 720p logical window.
constexpr float kMaximumFolderHeight = 900.0f;
constexpr float kFolderWidthToHeight = 0.80f;
constexpr float kTabWidthToFolderWidth = 0.13f;

float GetMenuUiScale(float folder_height) {
  return std::clamp(folder_height / kReferenceFolderHeight, 0.80f, 1.20f);
}

void TextWrappedColored(ImU32 color, const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// Rotate the glyphs AddText() just appended about `pivot` by `angle` radians.
void AddTextRotated(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
                    const char* text, float angle, ImVec2 pivot) {
  int v0 = dl->VtxBuffer.Size;
  dl->AddText(font, size, pos, col, text);
  int v1 = dl->VtxBuffer.Size;
  const float s = std::sin(angle), c = std::cos(angle);
  for (int i = v0; i < v1; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - pivot.x, dy = v.pos.y - pivot.y;
    v.pos.x = pivot.x + dx * c - dy * s;
    v.pos.y = pivot.y + dx * s + dy * c;
  }
}

// Draw a short label as a centered vertical stack of characters.
void AddVerticalLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                      const char* text) {
  const float line_h = size + 1.0f;
  int n = 0;
  for (const char* p = text; *p; ++p)
    ++n;
  float y = center.y - (n * line_h) * 0.5f;
  for (const char* p = text; *p; ++p, y += line_h) {
    char ch[2] = {*p, 0};
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ch);
    dl->AddText(font, size, ImVec2(center.x - sz.x * 0.5f, y), col, ch);
  }
}

// Keyboard rebinding ---------------------------------------------------------
struct KeyBind {
  const char* label;
  const char* cvar;
};

#if defined(__APPLE__)
constexpr const char* kMouseSensitivityCvar = "mnk_sensitivity";
constexpr const char* kMouseEnableCvar = "mnk_mouse_enabled";
constexpr float kMouseSensitivityMin = 0.05f;
constexpr float kMouseSensitivityMax = 10.0f;

// macOS input is delivered by the common native keyboard/mouse driver.
// Keep this menu pointed at the cvars that driver actually consumes.
constexpr KeyBind kBinds[] = {
    {"Move Forward", "keybind_lstick_up"},
    {"Move Back", "keybind_lstick_down"},
    {"Move Left", "keybind_lstick_left"},
    {"Move Right", "keybind_lstick_right"},
    {"A", "keybind_a"},
    {"B", "keybind_b"},
    {"X", "keybind_x"},
    {"Y", "keybind_y"},
    {"Left Trigger", "keybind_left_trigger"},
    {"Right Trigger", "keybind_right_trigger"},
    {"Left Bumper", "keybind_left_shoulder"},
    {"Toggle Original/Remastered (RB)", "keybind_right_shoulder"},
    {"Left Stick (L3)", "keybind_lstick_press"},
    {"Right Stick (R3)", "keybind_rstick_press"},
    {"D-Pad Up", "keybind_dpad_up"},
    {"D-Pad Down", "keybind_dpad_down"},
    {"D-Pad Left", "keybind_dpad_left"},
    {"D-Pad Right", "keybind_dpad_right"},
    {"Start", "keybind_start"},
    {"Back", "keybind_back"},
    {"Look Up", "keybind_rstick_up"},
    {"Look Down", "keybind_rstick_down"},
    {"Look Left", "keybind_rstick_left"},
    {"Look Right", "keybind_rstick_right"},
};
#else
constexpr const char* kMouseSensitivityCvar = "ge_mouse_sens";
constexpr const char* kMouseEnableCvar = "ge_mouselook_enable";
constexpr float kMouseSensitivityMin = 0.05f;
constexpr float kMouseSensitivityMax = 20.0f;

constexpr KeyBind kBinds[] = {
    {"Move Forward", "ge_key_mv_up"},
    {"Move Back", "ge_key_mv_down"},
    {"Move Left", "ge_key_mv_left"},
    {"Move Right", "ge_key_mv_right"},
    {"A", "ge_key_a"},
    {"B", "ge_key_b"},
    {"X", "ge_key_x"},
    {"Y", "ge_key_y"},
    {"Left Trigger", "ge_key_lt"},
    {"Right Trigger", "ge_key_rt"},
    {"Left Bumper", "ge_key_lb"},
    {"Toggle Original/Remastered (RB)", "ge_key_rb"},
    {"Left Stick (L3)", "ge_key_l3"},
    {"Right Stick (R3)", "ge_key_r3"},
    {"D-Pad Up", "ge_key_dup"},
    {"D-Pad Down", "ge_key_ddown"},
    {"D-Pad Left", "ge_key_dleft"},
    {"D-Pad Right", "ge_key_dright"},
    {"Start", "ge_key_start"},
    {"Back", "ge_key_back"},
    {"Look Up", "ge_key_look_up"},
    {"Look Down", "ge_key_look_down"},
    {"Look Left", "ge_key_look_left"},
    {"Look Right", "ge_key_look_right"},
};
#endif

constexpr const char* kMenuMouseSensitivityCvar = "ge_menu_sensitivity";
constexpr float kMenuMouseSensitivityMin = 0.05f;
constexpr float kMenuMouseSensitivityMax = 10.0f;

// Map an ImGui key to a Windows virtual-key code (== rex VirtualKey value).
int ImGuiKeyToVk(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z)
    return 'A' + (k - ImGuiKey_A);
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9)
    return '0' + (k - ImGuiKey_0);
  if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9)
    return 0x60 + (k - ImGuiKey_Keypad0);
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12)
    return 0x70 + (k - ImGuiKey_F1);
  switch (k) {
    case ImGuiKey_Space:
      return 0x20;
    case ImGuiKey_Enter:
      return 0x0D;
    case ImGuiKey_Tab:
      return 0x09;
    case ImGuiKey_Backspace:
      return 0x08;
    case ImGuiKey_Delete:
      return 0x2E;
    case ImGuiKey_Insert:
      return 0x2D;
    case ImGuiKey_Home:
      return 0x24;
    case ImGuiKey_End:
      return 0x23;
    case ImGuiKey_PageUp:
      return 0x21;
    case ImGuiKey_PageDown:
      return 0x22;
    case ImGuiKey_LeftArrow:
      return 0x25;
    case ImGuiKey_RightArrow:
      return 0x27;
    case ImGuiKey_UpArrow:
      return 0x26;
    case ImGuiKey_DownArrow:
      return 0x28;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
      return 0x10;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
      return 0x11;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
      return 0x12;
    case ImGuiKey_GraveAccent:
      return 0xC0;
    case ImGuiKey_Minus:
      return 0xBD;
    case ImGuiKey_Equal:
      return 0xBB;
    case ImGuiKey_Comma:
      return 0xBC;
    case ImGuiKey_Period:
      return 0xBE;
    case ImGuiKey_Semicolon:
      return 0xBA;
    case ImGuiKey_Slash:
      return 0xBF;
    case ImGuiKey_Backslash:
      return 0xDC;
    case ImGuiKey_LeftBracket:
      return 0xDB;
    case ImGuiKey_RightBracket:
      return 0xDD;
    case ImGuiKey_Apostrophe:
      return 0xDE;
    case ImGuiKey_CapsLock:
      return 0x14;
    case ImGuiKey_KeypadEnter:
      return 0x0D;
    case ImGuiKey_KeypadAdd:
      return 0x6B;
    case ImGuiKey_KeypadSubtract:
      return 0x6D;
    case ImGuiKey_KeypadMultiply:
      return 0x6A;
    case ImGuiKey_KeypadDivide:
      return 0x6F;
    default:
      return 0;
  }
}

}  // namespace

GeMenuDialog::GeMenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks)
    : rex::ui::ImGuiDialog(drawer), callbacks_(std::move(callbacks)) {
  testing_pending_toggles_.fill(-1);
}

GeMenuDialog::~GeMenuDialog() {
  // If the menu is torn down mid-capture, restore ImGui nav + unblock game input
  // so neither stays disabled.
  if (saved_nav_flags_ != 0xFFFFFFFFu) {
    ImGui::GetIO().ConfigFlags |= static_cast<ImGuiConfigFlags>(saved_nav_flags_);
    saved_nav_flags_ = 0xFFFFFFFFu;
  }
  ge::SetRebindCapturing(false);
}

void GeMenuDialog::RequestClose() {
  Close();
}

void GeMenuDialog::OnClose() {
  // Flush once on close as a final safety net for live controls. ImGui may not
  // report IsItemDeactivatedAfterEdit when Escape or shutdown closes the menu
  // while a slider or colour editor is still active.
  if (callbacks_.persist_config)
    callbacks_.persist_config();
  if (callbacks_.on_closed)
    callbacks_.on_closed();
  if (performance_report_pending_ && !quit_requested_)
    rex::perf::StartMetalPerformanceReport(60);
  if (quit_requested_ && callbacks_.on_quit)
    callbacks_.on_quit();
}

void GeMenuDialog::OnDraw(ImGuiIO& io) {
  UpdateControllerSnapshot();

  // Keyboard tab navigation -- suppressed while a rebind is capturing, so the
  // arrow keys you press to bind are listened for, not used to switch tabs.
  if (!rebinding_cvar_) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
      selected_tab_ = (selected_tab_ - 1 + kTabCount) % kTabCount;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
      selected_tab_ = (selected_tab_ + 1) % kTabCount;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1))
      selected_tab_ = (selected_tab_ - 1 + kTabCount) % kTabCount;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1))
      selected_tab_ = (selected_tab_ + 1) % kTabCount;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
      Close();
      return;
    }
  }

  const ImVec2 disp = io.DisplaySize;
  if (!(disp.x > 0.0f) || !(disp.y > 0.0f)) {
    return;
  }

  // --- Centered portrait panel (folder body + right-edge tab strip) ---
  // Use the available logical size, with a modest maximum at 1080p/1440p so
  // controls, explicit draw-list text and the folder stay in proportion. The
  // width cap also keeps the tab strip on-screen in narrow windows.
  const float margin = std::clamp(std::min(disp.x, disp.y) * 0.04f, 12.0f, 40.0f);
  const float available_h = std::max(1.0f, disp.y - margin * 2.0f);
  const float folder_h =
      std::max(1.0f, std::floor(std::min({disp.y * 0.82f, available_h, kMaximumFolderHeight})));
  const float max_folder_w =
      std::max(1.0f, (disp.x - margin * 2.0f) / (1.0f + kTabWidthToFolderWidth));
  const float folder_w =
      std::max(1.0f, std::floor(std::min(folder_h * kFolderWidthToHeight, max_folder_w)));
  tab_w_ = std::max(1.0f, std::floor(folder_w * kTabWidthToFolderWidth));
  const float total_w = folder_w + tab_w_;
  const ImVec2 origin(std::floor((disp.x - total_w) * 0.5f),
                      std::floor((disp.y - folder_h) * 0.5f));
  f0_ = origin;
  f1_ = ImVec2(origin.x + folder_w, origin.y + folder_h);

  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize(ImVec2(total_w, folder_h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!ImGui::Begin("##ge_pause_menu", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }

  DrawFolder(io);
  DrawTabs(io);
  DrawContent(io);

  ImGui::End();
  ImGui::PopStyleVar();
}

void GeMenuDialog::UpdateControllerSnapshot() {
  controller_snapshot_ = {};
  controller_snapshot_valid_ = false;
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime && runtime->input_system()
                    ? static_cast<rex::input::InputSystem*>(runtime->input_system())
                    : nullptr;
  if (input) {
    controller_snapshot_valid_ = input->GetControllerSnapshot(0, &controller_snapshot_);
  }
}

void GeMenuDialog::DrawControllerTest() {
  if (!controller_snapshot_valid_ || !controller_snapshot_.connected) {
    TextWrappedColored(kInkDim,
                       "No physical controller detected. Connect one over USB or "
                       "Bluetooth; this panel updates automatically.");
    return;
  }

  ImGui::TextColored(ImColor(kTitle), "%s", controller_snapshot_.name.c_str());
  const auto& gamepad = controller_snapshot_.gamepad;
  const auto& raw = controller_snapshot_.raw_gamepad;
  auto axis = [](const rex::be<int16_t>& value) {
    return rex::input::controller::AxisToUnit(static_cast<int16_t>(value));
  };
  auto axis_bar = [&](const char* label, float value) {
    char display[32];
    std::snprintf(display, sizeof(display), "%s %+0.2f", label, value);
    ImGui::ProgressBar((value + 1.0f) * 0.5f, ImVec2(-1.0f, 0.0f), display);
  };
  axis_bar("Move X", axis(gamepad.thumb_lx));
  axis_bar("Move Y", axis(gamepad.thumb_ly));
  axis_bar("Look X", axis(gamepad.thumb_rx));
  axis_bar("Look Y", axis(gamepad.thumb_ry));

  char triggers[96];
  std::snprintf(triggers, sizeof(triggers), "Guest triggers  L %.2f   R %.2f",
                float(gamepad.left_trigger) / 255.0f, float(gamepad.right_trigger) / 255.0f);
  ImGui::ProgressBar((float(gamepad.left_trigger) + float(gamepad.right_trigger)) / 510.0f,
                     ImVec2(-1.0f, 0.0f), triggers);

  auto pressed_buttons = [](uint16_t buttons) {
    std::string pressed;
    auto append = [&](uint16_t mask, const char* name) {
      if (!(buttons & mask))
        return;
      if (!pressed.empty())
        pressed += "  ";
      pressed += name;
    };
    append(rex::input::X_INPUT_GAMEPAD_A, "A");
    append(rex::input::X_INPUT_GAMEPAD_B, "B");
    append(rex::input::X_INPUT_GAMEPAD_X, "X");
    append(rex::input::X_INPUT_GAMEPAD_Y, "Y");
    append(rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, "LB");
    append(rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, "RB");
    append(rex::input::X_INPUT_GAMEPAD_START, "START");
    append(rex::input::X_INPUT_GAMEPAD_BACK, "BACK");
    append(rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, "L3");
    append(rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, "R3");
    append(rex::input::X_INPUT_GAMEPAD_DPAD_UP, "UP");
    append(rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, "DOWN");
    append(rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, "LEFT");
    append(rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, "RIGHT");
    return pressed;
  };
  const std::string guest_pressed = pressed_buttons(static_cast<uint16_t>(gamepad.buttons));
  const std::string physical_pressed = pressed_buttons(static_cast<uint16_t>(raw.buttons));
  ImGui::TextWrapped("Guest buttons: %s", guest_pressed.empty() ? "(none)" : guest_pressed.c_str());
  if (guest_pressed != physical_pressed) {
    std::string physical_text = "Physical buttons: ";
    physical_text += physical_pressed.empty() ? "(none)" : physical_pressed;
    TextWrappedColored(kInkDim, physical_text.c_str());
  }
}

void GeMenuDialog::DrawFolder(ImGuiIO& /*io*/) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = f0_, f1 = f1_;
  const float fw = f1.x - f0.x, fh = f1.y - f0.y;
  const float ui_scale = GetMenuUiScale(fh);

  // Drop shadow.
  dl->AddRectFilled(ImVec2(f0.x + 10.0f * ui_scale, f0.y + 12.0f * ui_scale),
                    ImVec2(f1.x + 10.0f * ui_scale, f1.y + 12.0f * ui_scale), IM_COL32(0, 0, 0, 90),
                    6.0f * ui_scale);
  // TEXTURE: manila folder body.
  dl->AddRectFilled(f0, f1, kFolder, 6.0f * ui_scale);
  dl->AddRect(f0, f1, kFolderEdge, 6.0f * ui_scale, 0, 2.0f * ui_scale);

  // TEXTURE: diagonal CLASSIFIED stamp (behind body content).
  {
    const char* stamp = "CLASSIFIED";
    const float ssize = std::floor(fw * 0.16f);
    ImVec2 ssz = ImGui::GetFont()->CalcTextSizeA(ssize, FLT_MAX, 0.0f, stamp);
    ImVec2 spos((f0.x + f1.x) * 0.5f - ssz.x * 0.5f, f0.y + fh * 0.62f - ssz.y * 0.5f);
    AddTextRotated(dl, ImGui::GetFont(), ssize, spos, kStamp, stamp, -0.5f,
                   ImVec2((f0.x + f1.x) * 0.5f, f0.y + fh * 0.62f));
  }
}

void GeMenuDialog::DrawTabs(ImGuiIO& /*io*/) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = f0_, f1 = f1_;
  const float fh = f1.y - f0.y;
  const float folder_right = f1.x;
  const float ui_scale = GetMenuUiScale(fh);

  const float strip_top = f0.y + std::floor(fh * 0.08f);
  const float strip_bottom = f1.y - std::floor(fh * 0.04f);
  const float gap = std::max(1.0f, std::floor(fh * 0.015f));
  const float tab_h =
      std::max(1.0f, std::floor((strip_bottom - strip_top - gap * (kTabCount - 1)) / kTabCount));

  // SYSTEM (and ONLINE where available) is the longest label at six stacked
  // glyphs. The previous size was
  // based only on tab width, so it exceeded a tab's height at every resolution
  // (about 108 px of text in a 95 px tab at 720p). Size all labels uniformly to
  // the longest one and retain vertical breathing room.
  constexpr float kLongestLabelGlyphCount = 6.0f;
  const float label_vertical_padding = std::max(3.0f, 6.0f * ui_scale);
  const float label_size_from_height =
      std::max(1.0f, (tab_h - label_vertical_padding * 2.0f) / kLongestLabelGlyphCount - 1.0f);
  const float label_size =
      std::max(1.0f, std::floor(std::min(tab_w_ * 0.28f, label_size_from_height)));

  for (int i = 0; i < kTabCount; ++i) {
    const float ty0 = strip_top + i * (tab_h + gap);
    const float ty1 = ty0 + tab_h;
    const bool sel = (i == selected_tab_);
    // Selected tab overlaps the folder edge so it reads as the open page.
    const float left = sel ? folder_right - 4.0f * ui_scale : folder_right + 3.0f * ui_scale;
    const float right = folder_right + tab_w_;
    const ImVec2 t0(left, ty0), t1(right, ty1);

    // Don't switch tabs on click while capturing -- a mouse click is being read
    // as the bind (LMB/RMB/MMB) instead.
    if (!rebinding_cvar_ && ImGui::IsMouseHoveringRect(t0, t1) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      selected_tab_ = i;

    // TEXTURE: tab (selected vs unselected).
    dl->AddRectFilled(t0, t1, sel ? kTabSel : kTab, 5.0f * ui_scale, ImDrawFlags_RoundCornersRight);
    dl->AddRect(t0, t1, kFolderEdge, 5.0f * ui_scale, ImDrawFlags_RoundCornersRight,
                1.5f * ui_scale);

    AddVerticalLabel(
        dl, ImGui::GetFont(), label_size,
        ImVec2((left + right) * 0.5f + (sel ? 2.0f * ui_scale : 0.0f), (ty0 + ty1) * 0.5f), kInk,
        kTabs[i].label);

    // TEXTURE: red reticle next to the selected tab.
    if (sel) {
      const ImVec2 c(folder_right - 16.0f * ui_scale, (ty0 + ty1) * 0.5f);
      const float r = 8.0f * ui_scale;
      dl->AddCircle(c, r, kReticle, 16, 2.0f * ui_scale);
      dl->AddLine(ImVec2(c.x - r - 4.0f * ui_scale, c.y), ImVec2(c.x + r + 4.0f * ui_scale, c.y),
                  kReticle, 2.0f * ui_scale);
      dl->AddLine(ImVec2(c.x, c.y - r - 4.0f * ui_scale), ImVec2(c.x, c.y + r + 4.0f * ui_scale),
                  kReticle, 2.0f * ui_scale);
    }
  }
}

void GeMenuDialog::DrawContent(ImGuiIO& /*io*/) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = f0_, f1 = f1_;
  const float fw = f1.x - f0.x, fh = f1.y - f0.y;
  const float ui_scale = GetMenuUiScale(fh);

  const float pad = std::floor(fw * 0.08f);
  const float title_size = std::floor(38.0f * ui_scale);

  // TEXTURE: big serif section title (top-left).
  dl->AddText(ImGui::GetFont(), title_size, ImVec2(f0.x + pad, f0.y + pad * 0.7f), kTitle,
              kTabs[selected_tab_].title);
  const ge::host_pause::Snapshot pause = ge::host_pause::GetSnapshot();
  if (pause.requested && (pause.gameplay_paused || !pause.request_applied)) {
    const char* pause_label = pause.gameplay_paused ? "GAMEPLAY PAUSED" : "PAUSING...";
    const float pause_size = std::floor(13.0f * ui_scale);
    const ImVec2 label_size =
        ImGui::GetFont()->CalcTextSizeA(pause_size, FLT_MAX, 0.0f, pause_label);
    dl->AddText(
        ImGui::GetFont(), pause_size,
        ImVec2(f1.x - pad - label_size.x, f0.y + pad * 0.7f + (title_size - label_size.y) * 0.5f),
        kReticle, pause_label);
  }
  const float rule_y = f0.y + pad * 0.7f + title_size + 6.0f * ui_scale;
  dl->AddLine(ImVec2(f0.x + pad, rule_y), ImVec2(f1.x - pad, rule_y), kInkDim, 2.0f * ui_scale);

  // --- Interactive content per tab ---
  ImGui::PushStyleColor(ImGuiCol_Text, kInk);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(180, 168, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(190, 178, 142, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(170, 158, 122, 255));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kReticle);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(220, 60, 50, 255));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kReticle);
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 168, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(198, 186, 150, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(166, 154, 118, 255));
  // Dropdown popups: a panel slightly darker than the folder, so the dark ink
  // text stays readable (ImGui's default popup background is near-black).
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(190, 178, 140, 255));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(168, 150, 104, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(184, 162, 112, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(160, 138, 96, 255));
  // Scrollbars (in the dropdown popups and the content panel) -- dark on tan so
  // they're actually visible and grabbable.
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(172, 160, 124, 200));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(118, 102, 70, 255));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(140, 122, 84, 255));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(96, 82, 56, 255));

  // Scale widgets gently with the logical panel size. Retina backing pixels do
  // not enter this calculation, preventing 2x-sized controls on high-DPI Macs.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * ui_scale, 8.0f * ui_scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * ui_scale, 12.0f * ui_scale));

  const ImVec2 content_pos(f0.x + pad, rule_y + std::floor(fh * 0.04f));
  const float content_w = (f1.x - pad) - content_pos.x;
  const float content_h = (f1.y - std::floor(fh * 0.11f)) - content_pos.y;
  ImGui::SetCursorScreenPos(content_pos);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  // Scrollable so longer tabs (e.g. Post-FX) fit the portrait folder.
  ImGui::BeginChild("##gecontent", ImVec2(content_w, content_h), false,
                    ImGuiWindowFlags_NoBackground);
  ImGui::SetWindowFontScale(1.45f * ui_scale);
  // Narrow 720p folders need room for labels to the right of sliders/combos;
  // larger panels can devote more width to the control itself.
  ImGui::PushItemWidth(content_w * (content_w < 440.0f ? 0.52f : 0.58f));

  switch (selected_tab_) {
    case kAudioTab: {
      // Master volume -- live: the SDL mixer reads master_volume each callback,
      // so the slider is audible immediately. Persisted to ge.toml on release
      // (mouse-up) so it survives a restart.
      float vol = GetCvarF("master_volume");
      if (ImGui::SliderFloat("Master Volume", &vol, 0.0f, 1.0f, "%.2f")) {
        if (vol < 0.0f)
          vol = 0.0f;
        if (vol > 1.0f)
          vol = 1.0f;
        SetCvarF("master_volume", vol);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
      ImGui::Spacing();
      ImGui::TextColored(ImColor(kInkDim), "(applies instantly)");
      break;
    }
    case kVideoTab: {
#if defined(__APPLE__)
      // These presets change only renderer quality controls. They deliberately
      // leave V-Sync, the stability throttle and the player's colour grade
      // alone, so applying a preset never changes timing or a saved look.
      static const struct {
        const char* label;
        const char* scaler;
        const char* anisotropy;
        const char* anti_aliasing;
      } kPerformancePresets[] = {
          {"Performance", "bilinear", "2", "none"},
          {"Balanced", "sharp", "3", "fxaa"},
          {"Quality", "metalfx", "5", "fxaa_extreme"},
      };
      const std::string current_scaler = rex::cvar::GetFlagByName("metal_output_scaler");
      const std::string current_anisotropy = rex::cvar::GetFlagByName("anisotropic_override");
      const std::string current_anti_aliasing = rex::cvar::GetFlagByName("swap_post_effect");
      int performance_preset = -1;
      for (int i = 0; i < IM_ARRAYSIZE(kPerformancePresets); ++i) {
        if (current_scaler == kPerformancePresets[i].scaler &&
            current_anisotropy == kPerformancePresets[i].anisotropy &&
            current_anti_aliasing == kPerformancePresets[i].anti_aliasing) {
          performance_preset = i;
          break;
        }
      }
      const char* performance_preview =
          performance_preset >= 0 ? kPerformancePresets[performance_preset].label : "Custom";
      ImGui::TextColored(ImColor(kTitle), "PERFORMANCE");
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 5.0f));
      if (ImGui::BeginCombo("Quality Preset", performance_preview)) {
        for (int i = 0; i < IM_ARRAYSIZE(kPerformancePresets); ++i) {
          const bool selected = performance_preset == i;
          if (ImGui::Selectable(kPerformancePresets[i].label, selected)) {
            rex::cvar::SetFlagByName("metal_output_scaler", kPerformancePresets[i].scaler);
            rex::cvar::SetFlagByName("anisotropic_override", kPerformancePresets[i].anisotropy);
            rex::cvar::SetFlagByName("swap_post_effect", kPerformancePresets[i].anti_aliasing);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim,
                         "(live renderer presets; timing, stability and Post-FX stay unchanged)");
      ImGui::Spacing();
#endif

      // --- Fullscreen (live; the request is applied deferred, off the paint
      //     thread, so the surface is never torn down mid-frame) ---
      bool fs = callbacks_.get_fullscreen ? callbacks_.get_fullscreen()
                                          : (rex::cvar::GetFlagByName("fullscreen") == "true");
      if (ImGui::Checkbox("Fullscreen", &fs)) {
        if (callbacks_.request_fullscreen)
          callbacks_.request_fullscreen(fs);
        rex::cvar::SetFlagByName("fullscreen", fs ? "true" : "false");
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }

      // --- V-Sync. Guest timing changes live; the callback lets the active
      //     presenter update host display synchronization at the same time. ---
      bool vsync = rex::cvar::GetFlagByName("vsync") == "true";
      if (ImGui::Checkbox("V-Sync", &vsync)) {
        rex::cvar::SetFlagByName("vsync", vsync ? "true" : "false");
        if (callbacks_.on_vsync_changed)
          callbacks_.on_vsync_changed(vsync);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
#if defined(__APPLE__)
      TextWrappedColored(kInkDim, "(applies instantly)");
#endif

#if defined(__APPLE__)
      // The Metal presenter reads these live for both its direct GPU overlay
      // and CPU fallback, so the overlay changes without restarting.
      bool show_fps = GetCvarB("metal_show_fps");
      bool detailed_performance = GetCvarB("metal_show_detailed_performance");
      int overlay_mode = !show_fps ? 0 : (detailed_performance ? 2 : 1);
      constexpr const char* kOverlayModes[] = {"Off", "FPS", "Detailed"};
      if (ImGui::BeginCombo("Performance Overlay", kOverlayModes[overlay_mode])) {
        for (int i = 0; i < IM_ARRAYSIZE(kOverlayModes); ++i) {
          const bool selected = overlay_mode == i;
          if (ImGui::Selectable(kOverlayModes[i], selected)) {
            SetCvarB("metal_show_fps", i != 0);
            SetCvarB("metal_show_detailed_performance", i == 2);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }

      const rex::perf::MetalPerformanceSnapshot performance =
          rex::perf::GetMetalPerformanceSnapshot();
      if (performance.report_active) {
        const float progress = performance.report_duration_seconds > 0.0
                                   ? float(std::clamp(performance.report_elapsed_seconds /
                                                          performance.report_duration_seconds,
                                                      0.0, 1.0))
                                   : 0.0f;
        char report_progress[48];
        std::snprintf(report_progress, sizeof(report_progress), "Performance report %.0f / %.0f s",
                      performance.report_elapsed_seconds, performance.report_duration_seconds);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), report_progress);
        if (performance.report_paused) {
          TextWrappedColored(kInk,
                             "Report paused while settings is open; close settings to resume.");
        }
        if (ImGui::Button("Cancel Performance Report")) {
          rex::perf::CancelMetalPerformanceReport();
        }
      } else if (performance_report_pending_) {
        TextWrappedColored(kInk, "The 60-second report will begin when you close settings.");
        if (ImGui::Button("Cancel Pending Report")) {
          performance_report_pending_ = false;
        }
      } else if (ImGui::Button("Record 60-Second Performance Report")) {
        performance_report_pending_ = true;
      }
      const char* performance_report_hint =
          "(plays normally while measuring; completed results are added to diagnostics)";
      if (performance_report_pending_) {
        performance_report_hint =
            "(close settings and play normally; the report stops automatically)";
      } else if (performance.report_active) {
        performance_report_hint = "(host-menu frames are excluded from the report)";
      } else if (performance.last_report_available) {
        performance_report_hint =
            "(last completed report will be included when diagnostics are exported)";
      }
      TextWrappedColored(kInkDim, performance_report_hint);
#endif

#if !defined(__APPLE__)
      // --- Frame limit. GoldenEye's game clock runs at 60Hz. ---
      static const struct {
        const char* label;
        const char* value;
      } kFps[] = {
          {"30 FPS", "30"},   {"60 FPS", "60"},   {"90 FPS", "90"},   {"120 FPS", "120"},
          {"144 FPS", "144"}, {"165 FPS", "165"}, {"180 FPS", "180"}, {"240 FPS", "240"},
          {"300 FPS", "300"}, {"Uncapped", "0"},
      };
      const std::string cur_fps = rex::cvar::GetFlagByName("max_fps");
      int fps_idx = -1;
      for (int i = 0; i < IM_ARRAYSIZE(kFps); ++i)
        if (cur_fps == kFps[i].value)
          fps_idx = i;
      if (fps_idx < 0)
        fps_idx = 0;
      ImGui::BeginDisabled(vsync);
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      const char* fps_preview = vsync ? "V-Synced" : kFps[fps_idx].label;
      if (ImGui::BeginCombo("Frame Limit", fps_preview)) {
        for (int i = 0; i < IM_ARRAYSIZE(kFps); ++i) {
          bool sel = (i == fps_idx);
          if (ImGui::Selectable(kFps[i].label, sel)) {
            rex::cvar::SetFlagByName("max_fps", kFps[i].value);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
#endif

      // --- GPU throttle. Pauses the emulated GPU command worker after each ring
      //     drain so it can't outrun the render thread -- the cause of the
      //     intermittent picture freeze. Higher = fewer freezes but more input
      //     latency; lower = snappier but more freeze risk; 0 = off. Applied
      //     live (the CP worker reads it each drain); saved to ge.toml on
      //     release. This is the supported way to tune it -- editing ge.toml by
      //     hand is fragile because the game rewrites the file on every save. ---
      int throttle_us = std::atoi(rex::cvar::GetFlagByName("ge_gpu_throttle_us").c_str());
      if (ImGui::SliderInt("GPU Throttle (us)", &throttle_us, 0, 500)) {
        if (throttle_us < 0)
          throttle_us = 0;
        rex::cvar::SetFlagByName("ge_gpu_throttle_us", std::to_string(throttle_us));
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
      TextWrappedColored(kInkDim, "(stability tuning; higher = fewer freezes, more lag)");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

#if defined(__APPLE__)
      // The current Metal render-target path is fixed at the game's native
      // internal resolution. Keep this honest and read-only until Metal
      // supersampling is implemented.
      ImGui::TextColored(ImColor(kInk), "Internal Resolution:");
      ImGui::SameLine();
      ImGui::TextColored(ImColor(kTitle), "720p (native)");
      TextWrappedColored(kInkDim, "(the game renders at its native 1280 x 720 resolution)");

      static const struct {
        const char* label;
        const char* value;
      } kOutputScalers[] = {
          {"Bilinear (fastest)", "bilinear"}, {"Sharp", "sharp"}, {"MetalFX Spatial", "metalfx"}};
      const std::string output_scaler = rex::cvar::GetFlagByName("metal_output_scaler");
      int output_scaler_index = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kOutputScalers); ++i) {
        if (output_scaler == kOutputScalers[i].value)
          output_scaler_index = i;
      }
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 5.0f));
      if (ImGui::BeginCombo("Output Scaling", kOutputScalers[output_scaler_index].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kOutputScalers); ++i) {
          const bool selected = output_scaler_index == i;
          if (ImGui::Selectable(kOutputScalers[i].label, selected)) {
            rex::cvar::SetFlagByName("metal_output_scaler", kOutputScalers[i].value);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim,
                         "(scales the native image to the window or Retina display; applies live)");
#else
      // --- Internal resolution (supersampling). This is the resolution the 3D
      //     scene is actually rendered at, independent of the window size: the
      //     game's native 1280x720 surfaces are scaled by an integer factor and
      //     downsampled to the window. Driven by the engine's resolution_scale
      //     cvar (1x/2x/3x/4x). It is kRequiresRestart, so the choice is written
      //     now and the render targets are rebuilt at this size on next launch.
      //     1080p is intentionally absent: 720p x 1.5 is not an integer scale,
      //     and only integer supersampling is guaranteed to change render res. ---
      static const struct {
        const char* label;
        const char* scale;
      } kIRes[] = {{"720p (native, 1x)", "1"},
                   {"1440p / 2K (2x)", "2"},
                   {"2160p / 4K (3x)", "3"},
                   {"2880p (4x)", "4"}};
      const std::string cur_scale = rex::cvar::GetFlagByName("resolution_scale");
      int ires_idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kIRes); ++i)
        if (cur_scale == kIRes[i].scale)
          ires_idx = i;
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Internal Resolution", kIRes[ires_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kIRes); ++i) {
          bool sel = (i == ires_idx);
          if (ImGui::Selectable(kIRes[i].label, sel)) {
            rex::cvar::SetFlagByName("resolution_scale", kIRes[i].scale);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim, "(renders the 3D scene at this resolution; needs a restart)");
      // Internal resolution can't change live without a full GPU device reset
      // (the EDRAM buffer + render-target/texture caches are sized at init --
      // changing them mid-run corrupts in-flight GPU state). So instead of a
      // manual save+restart, apply it in one click: persist + relaunch now.
      if (ImGui::Button("Apply Resolution (quick restart)")) {
        if (callbacks_.persist_config)
          callbacks_.persist_config();
        if (callbacks_.request_restart)
          callbacks_.request_restart();
      }
#endif

      ImGui::Spacing();

      // macOS has one renderer: native Metal. Don't expose the Windows/Linux
      // backend selector in a build where those choices cannot be used.
#if defined(__APPLE__)
      ImGui::TextColored(ImColor(kInk), "Graphics API:");
      ImGui::SameLine();
      ImGui::TextColored(ImColor(kTitle), "Metal");
      TextWrappedColored(kInkDim, "(native macOS renderer; always enabled)");
#else
      // "Auto" detects the GPU at boot: AMD -> Vulkan (the D3D12 path times
      // out / black-screens on AMD for this title), all others -> Direct3D 12.
      // Override here if needed; applied on restart.
      static const struct {
        const char* label;
        const char* value;
      } kApi[] = {{"Auto (recommended)", "auto"}, {"Direct3D 12", "d3d12"}, {"Vulkan", "vulkan"}};
      const std::string cur_api = rex::cvar::GetFlagByName("gpu");
      int api_idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kApi); ++i)
        if (cur_api == kApi[i].value)
          api_idx = i;
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Graphics API", kApi[api_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kApi); ++i) {
          bool sel = (i == api_idx);
          if (ImGui::Selectable(kApi[i].label, sel)) {
            rex::cvar::SetFlagByName("gpu", kApi[i].value);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim, "(AMD uses Vulkan automatically; applies after restart)");
#endif

      ImGui::Spacing();

      // --- Texture filtering. Drives the engine's anisotropic_override cvar,
      //     which is hot-reload: the sampler cache re-reads it per draw, so this
      //     applies instantly. Expose every legal override so a config loaded
      //     with a less common value is never displayed as a different one. ---
      static const struct {
        const char* label;
        const char* value;
      } kAniso[] = {{"Game default", "-1"}, {"Off", "0"}, {"1x", "1"}, {"2x", "2"},
                    {"4x (default)", "3"},  {"8x", "4"},  {"16x", "5"}};
      const std::string cur_aniso = rex::cvar::GetFlagByName("anisotropic_override");
      int aniso_idx = 4;  // default 4x (engine default = 3)
      for (int i = 0; i < IM_ARRAYSIZE(kAniso); ++i)
        if (cur_aniso == kAniso[i].value)
          aniso_idx = i;
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Texture Filtering", kAniso[aniso_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kAniso); ++i) {
          bool sel = (i == aniso_idx);
          if (ImGui::Selectable(kAniso[i].label, sel)) {
            rex::cvar::SetFlagByName("anisotropic_override", kAniso[i].value);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim, "(sharpens angled textures; applies instantly)");

      ImGui::Spacing();

      // --- Anti-aliasing. Drives swap_post_effect (FXAA post-process). Now
      //     hot-reload: the GPU vsync worker re-applies it each tick, so it
      //     switches live. MSAA isn't user-selectable (it's guest-driven). ---
      static const struct {
        const char* label;
        const char* value;
      } kAA[] = {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA (Extreme)", "fxaa_extreme"}};
      const std::string cur_aa = rex::cvar::GetFlagByName("swap_post_effect");
      int aa_idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kAA); ++i)
        if (cur_aa == kAA[i].value)
          aa_idx = i;
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Anti-Aliasing", kAA[aa_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kAA); ++i) {
          bool sel = (i == aa_idx);
          if (ImGui::Selectable(kAA[i].label, sel)) {
            rex::cvar::SetFlagByName("swap_post_effect", kAA[i].value);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim, "(post-process FXAA; applies instantly)");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      // --- Post-FX (live full-screen filter; see ge_postfx) ---
      ImGui::TextColored(ImColor(kTitle), "POST-FX");
      bool pfx_on = GetCvarB("postfx_enabled");
      if (ImGui::Checkbox("Enable Post-FX", &pfx_on)) {
        SetCvarB("postfx_enabled", pfx_on);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }

      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Preset", "Apply preset...")) {
        for (int i = 0; i < ge::PostFxPresetCount(); ++i) {
          if (ImGui::Selectable(ge::PostFxPresetName(i))) {
            ge::ApplyPostFxPreset(i);
            if (callbacks_.persist_config)
              callbacks_.persist_config();
          }
        }
        ImGui::EndCombo();
      }
      TextWrappedColored(kInkDim, "(GPU colour grade, vignette and scanlines)");

      ImGui::BeginDisabled(!pfx_on);
      {
        auto persist_after_edit = [this]() {
          if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
            callbacks_.persist_config();
        };

        float b = GetCvarF("postfx_brightness");
        if (ImGui::SliderFloat("Brightness", &b, -1.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_brightness", b);
        persist_after_edit();

        float con = GetCvarF("postfx_contrast");
        if (ImGui::SliderFloat("Contrast", &con, 0.0f, 2.0f, "%.2f"))
          SetCvarF("postfx_contrast", con);
        persist_after_edit();

        float sat = GetCvarF("postfx_saturation");
        if (ImGui::SliderFloat("Saturation", &sat, 0.0f, 2.0f, "%.2f"))
          SetCvarF("postfx_saturation", sat);
        persist_after_edit();

        float vib = GetCvarF("postfx_vibrance");
        if (ImGui::SliderFloat("Vibrance", &vib, -1.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_vibrance", vib);
        persist_after_edit();

        float temp = GetCvarF("postfx_temperature");
        if (ImGui::SliderFloat("Temperature", &temp, -1.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_temperature", temp);
        persist_after_edit();

        float gam = GetCvarF("postfx_gamma");
        if (ImGui::SliderFloat("Gamma", &gam, 0.3f, 3.0f, "%.2f"))
          SetCvarF("postfx_gamma", gam);
        persist_after_edit();

        float tint[3] = {GetCvarF("postfx_tint_r"), GetCvarF("postfx_tint_g"),
                         GetCvarF("postfx_tint_b")};
        if (ImGui::ColorEdit3("Tint Colour", tint, ImGuiColorEditFlags_NoInputs)) {
          SetCvarF("postfx_tint_r", tint[0]);
          SetCvarF("postfx_tint_g", tint[1]);
          SetCvarF("postfx_tint_b", tint[2]);
        }
        persist_after_edit();

        float ts = GetCvarF("postfx_tint");
        if (ImGui::SliderFloat("Tint Strength", &ts, 0.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_tint", ts);
        persist_after_edit();

        float vig = GetCvarF("postfx_vignette");
        if (ImGui::SliderFloat("Vignette", &vig, 0.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_vignette", vig);
        persist_after_edit();

        float scan = GetCvarF("postfx_scanlines");
        if (ImGui::SliderFloat("Scanlines", &scan, 0.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_scanlines", scan);
        persist_after_edit();
      }
      ImGui::EndDisabled();

      ImGui::Spacing();
      if (ImGui::Button("Save Look")) {
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset to Default")) {
        ge::ResetPostFx();
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
      break;
    }
    case kInputTab: {
#if defined(__APPLE__)
      ImGui::TextColored(ImColor(kTitle), "CONTROLLER");
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, kInkDim);
      ImGui::TextWrapped(
          "PS4, PS5, Xbox One and Xbox Series controllers are detected automatically through "
          "SDL over USB or Bluetooth where supported. Hot-plug is supported and keyboard and "
          "mouse stay active.");
      ImGui::TextWrapped(
          "Use the controller's south face button as A, east as B, west as X, north as Y, and "
          "Options/Menu as Start. RB switches original/remastered graphics. In these settings, "
          "A selects, B closes, and LB/RB changes tabs.");
      ImGui::PopStyleColor();
      ImGui::Spacing();

      rex::input::controller::Layout controller_layout = rex::input::controller::Layout::kModern;
      rex::input::controller::ParseLayout(GetCvarS(rex::input::kControllerLayoutCvar),
                                          &controller_layout);
      int controller_layout_index = static_cast<int>(controller_layout);
      const char* controller_layout_labels[] = {"Modern", "Classic", "Southpaw"};
      if (ImGui::Combo("Controller Layout", &controller_layout_index, controller_layout_labels,
                       3)) {
        controller_layout = static_cast<rex::input::controller::Layout>(controller_layout_index);
        SetCvarS(rex::input::kControllerLayoutCvar,
                 rex::input::controller::LayoutName(controller_layout));
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
      switch (controller_layout) {
        case rex::input::controller::Layout::kClassic:
          TextWrappedColored(kInkDim,
                             "Classic: left stick moves forward/back and turns; right stick "
                             "strafes and looks up/down.");
          break;
        case rex::input::controller::Layout::kSouthpaw:
          TextWrappedColored(kInkDim,
                             "Southpaw: right stick moves and left stick looks. L3/R3 follow "
                             "their sticks unless explicitly remapped below.");
          break;
        case rex::input::controller::Layout::kModern:
        default:
          TextWrappedColored(kInkDim, "Modern: left stick moves and right stick looks.");
          break;
      }
      TextWrappedColored(kInkDim,
                         "Changing layout keeps your custom button remaps, sensitivity and "
                         "deadzones.");
      ImGui::Spacing();

      float controller_sensitivity = GetCvarF(rex::input::kControllerLookSensitivityCvar);
      if (ImGui::SliderFloat("Look Sensitivity", &controller_sensitivity,
                             float(rex::input::kControllerLookSensitivityMin),
                             float(rex::input::kControllerLookSensitivityMax), "%.2f")) {
        SetCvarF(rex::input::kControllerLookSensitivityCvar, controller_sensitivity);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();

      float move_deadzone = GetCvarF(rex::input::kControllerMoveDeadzoneCvar);
      if (ImGui::SliderFloat("Move Deadzone", &move_deadzone,
                             float(rex::input::kControllerDeadzoneMin),
                             float(rex::input::kControllerDeadzoneMax), "%.2f")) {
        SetCvarF(rex::input::kControllerMoveDeadzoneCvar, move_deadzone);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();

      float aim_deadzone = GetCvarF(rex::input::kControllerAimDeadzoneCvar);
      if (ImGui::SliderFloat("Aim Deadzone", &aim_deadzone,
                             float(rex::input::kControllerDeadzoneMin),
                             float(rex::input::kControllerDeadzoneMax), "%.2f")) {
        SetCvarF(rex::input::kControllerAimDeadzoneCvar, aim_deadzone);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();

      bool controller_invert_y = GetCvarB(rex::input::kControllerInvertYCvar);
      if (ImGui::Checkbox("Invert Controller Y", &controller_invert_y)) {
        SetCvarB(rex::input::kControllerInvertYCvar, controller_invert_y);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }

      bool rumble_enabled = GetCvarB(rex::input::kControllerRumbleEnabledCvar);
      if (ImGui::Checkbox("Controller Rumble", &rumble_enabled)) {
        SetCvarB(rex::input::kControllerRumbleEnabledCvar, rumble_enabled);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
      float rumble_intensity = GetCvarF(rex::input::kControllerRumbleIntensityCvar);
      ImGui::BeginDisabled(!rumble_enabled);
      if (ImGui::SliderFloat("Rumble Intensity", &rumble_intensity,
                             float(rex::input::kControllerRumbleIntensityMin),
                             float(rex::input::kControllerRumbleIntensityMax), "%.2f")) {
        SetCvarF(rex::input::kControllerRumbleIntensityCvar, rumble_intensity);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
      ImGui::EndDisabled();

      auto* runtime = rex::Runtime::instance();
      auto* input = runtime && runtime->input_system()
                        ? static_cast<rex::input::InputSystem*>(runtime->input_system())
                        : nullptr;
      const bool can_test_rumble = input && controller_snapshot_valid_ &&
                                   controller_snapshot_.rumble_supported && rumble_enabled &&
                                   rumble_intensity > 0.0f;
      ImGui::BeginDisabled(!can_test_rumble);
      if (ImGui::Button("Test Rumble") && input) {
        input->PlayControllerTestRumble(0);
      }
      ImGui::EndDisabled();
      if (controller_snapshot_valid_ && !controller_snapshot_.rumble_supported) {
        TextWrappedColored(kInkDim, "(this controller does not report rumble support)");
      }

      ImGui::Spacing();
      if (ImGui::CollapsingHeader("BUTTON REMAPPING")) {
        TextWrappedColored(
            kInkDim,
            "Choose which physical control feeds each GoldenEye/Xbox input. Default follows "
            "the selected layout; Unbound disables that input. Choosing a physical control "
            "already in use swaps the two assignments. Remaps do not affect the controller "
            "controls used to navigate this settings screen.");
        ImGui::Spacing();

        const std::string saved_button_map = GetCvarS(rex::input::kControllerButtonMapCvar);
        rex::input::controller::ButtonBindings button_bindings;
        rex::input::controller::ParseButtonBindings(saved_button_map, &button_bindings);
        bool button_map_changed = false;
        for (size_t i = 0; i < rex::input::controller::kControlCount; ++i) {
          const auto target = static_cast<rex::input::controller::Control>(i);
          auto& selected_source = button_bindings.sources[i];
          const auto default_source =
              rex::input::controller::DefaultSource(controller_layout, target);

          std::string preview;
          if (selected_source == rex::input::controller::Control::kDefault) {
            preview = "Default (";
            preview += rex::input::controller::ControlDisplayName(default_source);
            preview += ')';
          } else {
            preview = rex::input::controller::ControlDisplayName(selected_source);
          }

          ImGui::PushID(static_cast<int>(i));
          const std::string target_label(rex::input::controller::ControlDisplayName(target));
          if (ImGui::BeginCombo(target_label.c_str(), preview.c_str())) {
            std::string default_label = "Default (";
            default_label += rex::input::controller::ControlDisplayName(default_source);
            default_label += ')';
            if (ImGui::Selectable(default_label.c_str(),
                                  selected_source == rex::input::controller::Control::kDefault)) {
              button_map_changed |= rex::input::controller::AssignSourceWithSwap(
                  controller_layout, &button_bindings, target,
                  rex::input::controller::Control::kDefault);
            }
            if (ImGui::Selectable("Unbound",
                                  selected_source == rex::input::controller::Control::kNone)) {
              button_map_changed |= rex::input::controller::AssignSourceWithSwap(
                  controller_layout, &button_bindings, target,
                  rex::input::controller::Control::kNone);
            }
            ImGui::Separator();
            for (size_t source_index = 0; source_index < rex::input::controller::kControlCount;
                 ++source_index) {
              const auto source = static_cast<rex::input::controller::Control>(source_index);
              const std::string source_label(rex::input::controller::ControlDisplayName(source));
              if (ImGui::Selectable(source_label.c_str(), selected_source == source)) {
                button_map_changed |= rex::input::controller::AssignSourceWithSwap(
                    controller_layout, &button_bindings, target, source);
              }
            }
            ImGui::EndCombo();
          }
          ImGui::PopID();
        }

        if (button_map_changed) {
          SetCvarS(rex::input::kControllerButtonMapCvar,
                   rex::input::controller::SerializeButtonBindings(button_bindings));
          if (callbacks_.persist_config)
            callbacks_.persist_config();
        }

        ImGui::BeginDisabled(saved_button_map.empty() && !button_map_changed);
        if (ImGui::Button("Reset Buttons")) {
          SetCvarS(rex::input::kControllerButtonMapCvar, "");
          if (callbacks_.persist_config)
            callbacks_.persist_config();
        }
        ImGui::EndDisabled();
        TextWrappedColored(kInkDim,
                           "Resetting button remaps keeps the selected layout, sensitivity, "
                           "deadzones and rumble settings.");
      }

      ImGui::Spacing();
      ImGui::TextColored(ImColor(kTitle), "LIVE CONTROLLER TEST");
      DrawControllerTest();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
#endif

      ImGui::TextColored(ImColor(kTitle), "MOUSE LOOK");
      ImGui::Spacing();

      // Sensitivity is live and persisted on release. GoldenEye consumes
      // native mouse distance directly for camera movement; it is never turned
      // into analog-stick velocity by the macOS app.
      float sens = GetCvarF(kMouseSensitivityCvar);
      if (ImGui::SliderFloat("Gameplay Mouse Sensitivity", &sens, kMouseSensitivityMin,
                             kMouseSensitivityMax, "%.2f")) {
        if (sens < kMouseSensitivityMin)
          sens = kMouseSensitivityMin;
        if (sens > kMouseSensitivityMax)
          sens = kMouseSensitivityMax;
        SetCvarF(kMouseSensitivityCvar, sens);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();

      float menu_sens = GetCvarF(kMenuMouseSensitivityCvar);
      if (ImGui::SliderFloat(
#if defined(__APPLE__)
              "GoldenEye Menu Cursor Speed",
#else
              "Menu Cursor Speed",
#endif
              &menu_sens, kMenuMouseSensitivityMin, kMenuMouseSensitivityMax, "%.2f")) {
        if (menu_sens < kMenuMouseSensitivityMin)
          menu_sens = kMenuMouseSensitivityMin;
        if (menu_sens > kMenuMouseSensitivityMax)
          menu_sens = kMenuMouseSensitivityMax;
        SetCvarF(kMenuMouseSensitivityCvar, menu_sens);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
#if defined(__APPLE__)
      TextWrappedColored(kInkDim, "(affects GoldenEye's menus, not these host settings)");
#endif

      ImGui::Spacing();

      // Mouse-look toggle. ON: the mouse looks (on top of the controller -- both
      // work at once) and the cursor is captured in-game. OFF: controller only,
      // cursor free.
      bool ml = GetCvarB(kMouseEnableCvar);
      if (ImGui::Checkbox(
#if defined(__APPLE__)
              "Mouse look and mouse buttons",
#else
              "Mouse look",
#endif
              &ml)) {
        SetCvarB(kMouseEnableCvar, ml);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
      TextWrappedColored(kInkDim,
#if defined(__APPLE__)
                         "(turning this off also disables mouse-button actions; controller still "
                         "works)"
#else
                         "(controller still works; cursor is captured in-game, freed in this menu)"
#endif
      );

      bool invert_y = GetCvarB("ge_invert_y");
      if (ImGui::Checkbox("Invert vertical look", &invert_y)) {
        SetCvarB("ge_invert_y", invert_y);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      ImGui::TextColored(ImColor(kTitle), "DEBUG");
      ImGui::Spacing();
      // Hidden debug menu (TCRF). Live: the enable gate reads this cvar each
      // frame, so no restart -- toggle here, then press LB in-game to open it.
      bool dbg = GetCvarB("ge_debug_menu");
      if (ImGui::Checkbox("Debug Menu", &dbg)) {
        SetCvarB("ge_debug_menu", dbg);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
      }
      TextWrappedColored(kInkDim, "(press LB in-game to open it; the game freezes while it's up)");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kTitle), "REBIND KEYS");
#if defined(__APPLE__)
      TextWrappedColored(kInkDim,
                         "(keyboard plus left, right and middle mouse buttons only; click a "
                         "binding, then press a key or button; Esc cancels; right-click unbinds)");
#else
      TextWrappedColored(
          kInkDim, "(click a binding, then press the new key; Esc cancels; right-click to unbind)");
#endif
      TextWrappedColored(
          kInkDim,
          "(tip: bind multiple keys to one action by comma-separating them in the config file, "
          "e.g. W,Up)");
      ImGui::Spacing();

      const float btn_w = content_w * 0.34f;
      const float label_x = content_w * 0.44f;
      for (const auto& b : kBinds) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(b.label);
        ImGui::SameLine(label_x);
        const bool capturing = (rebinding_cvar_ == b.cvar);
        std::string cur = capturing ? "press a key..." : rex::cvar::GetFlagByName(b.cvar);
        if (cur.empty())
          cur = "(unbound)";
        ImGui::PushID(b.cvar);
        if (ImGui::Button(cur.c_str(), ImVec2(btn_w, 0))) {
          rebinding_cvar_ = b.cvar;
          rebind_skip_ = 1;  // ignore the click that started this rebind
        }
        // Right-click clears the binding (unbind).
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          rex::cvar::SetFlagByName(b.cvar, "");
          if (callbacks_.persist_config)
            callbacks_.persist_config();
          if (rebinding_cvar_ == b.cvar)
            rebinding_cvar_ = nullptr;
        }
        ImGui::PopID();
      }

      // Capture the next key / mouse button for the pending rebind.
      if (rebinding_cvar_) {
        if (rebind_skip_ > 0) {
          --rebind_skip_;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
          rebinding_cvar_ = nullptr;  // cancel
        } else {
          int vk = 0;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            vk = 0x01;  // LMB
          else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            vk = 0x02;  // RMB
          else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            vk = 0x04;  // MMB
          else {
            for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
                 k = static_cast<ImGuiKey>(k + 1)) {
              if (ImGui::IsKeyPressed(k, false)) {
                vk = ImGuiKeyToVk(k);
                if (vk)
                  break;
              }
            }
          }
          if (vk) {
            std::string name = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
            if (!name.empty()) {
              rex::cvar::SetFlagByName(rebinding_cvar_, name);
              if (callbacks_.persist_config)
                callbacks_.persist_config();
            }
            rebinding_cvar_ = nullptr;
          }
        }
      }
      // Swallow all game input while a capture is pending (cleared the frame the
      // key is captured / cancelled). ge_inject_keyboard reads this flag.
      ge::SetRebindCapturing(rebinding_cvar_ != nullptr);
      // Also disable ImGui keyboard/gamepad navigation while capturing, so the
      // arrow / tab keys pressed to bind are captured here instead of switching
      // tabs (which was cancelling the rebind). Restored when capture ends.
      {
        ImGuiIO& io = ImGui::GetIO();
        const ImGuiConfigFlags nav =
            ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        if (rebinding_cvar_) {
          if (saved_nav_flags_ == 0xFFFFFFFFu)
            saved_nav_flags_ = static_cast<unsigned>(io.ConfigFlags & nav);
          io.ConfigFlags &= ~nav;
        } else if (saved_nav_flags_ != 0xFFFFFFFFu) {
          io.ConfigFlags |= static_cast<ImGuiConfigFlags>(saved_nav_flags_);
          saved_nav_flags_ = 0xFFFFFFFFu;
        }
      }
      break;
    }
#if !defined(__APPLE__)
    case kOnlineTab: {
      // Load the cvars into the edit buffers once, the first time the tab opens
      // (so typing doesn't fight a per-frame reload).
      if (!online_loaded_) {
        online_loaded_ = true;
        std::snprintf(username_buf_, sizeof(username_buf_), "%s",
                      rex::cvar::GetFlagByName("ge_username").c_str());
        std::snprintf(server_buf_, sizeof(server_buf_), "%s",
                      rex::cvar::GetFlagByName("ge_online_server").c_str());
        online_enable_ = GetCvarB("ge_online_enable");
        online_port_ = std::atoi(rex::cvar::GetFlagByName("ge_online_port").c_str());
        if (online_port_ <= 0 || online_port_ > 65535)
          online_port_ = 31000;
      }

      ImGui::TextWrapped(
          "Play with friends over a shared server. One person runs the server "
          "(GoldeneyeServer -- see DEPLOY.md) and shares its address; everyone "
          "enters that same address here.");
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kInk), "Username");
      ImGui::SetNextItemWidth(content_w * 0.85f);
      ImGui::InputText("##ge_user", username_buf_, sizeof(username_buf_));
      ImGui::TextColored(ImColor(kInkDim), "(shown to other players, max 15 characters)");
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kInk), "Server address");
      ImGui::SetNextItemWidth(content_w * 0.85f);
      ImGui::InputText("##ge_server", server_buf_, sizeof(server_buf_));
      TextWrappedColored(kInkDim,
                         "(IP or hostname -- your own server, or a playit.gg / Hamachi address)");
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kInk), "Server port");
      ImGui::SetNextItemWidth(content_w * 0.85f);
      ImGui::InputInt("##ge_port", &online_port_, 0, 0);  // 0,0 = no +/- step buttons
      TextWrappedColored(kInkDim, "(must match the port the host started the server with)");
      ImGui::Spacing();

      ImGui::Checkbox("Enable online play", &online_enable_);
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      const ImVec2 obsize(content_w * 0.85f, std::floor(fh * 0.07f));
      const bool empty_name = username_buf_[0] == '\0';
      if (empty_name)
        ImGui::BeginDisabled();
      if (ImGui::Button("SAVE & RESTART", obsize)) {
        const int sp = (online_port_ > 0 && online_port_ <= 65535) ? online_port_ : 31000;
        rex::cvar::SetFlagByName("ge_username", username_buf_);
        rex::cvar::SetFlagByName("ge_online_server", server_buf_[0] ? server_buf_ : "127.0.0.1");
        rex::cvar::SetFlagByName("ge_online_port", std::to_string(sp));
        SetCvarB("ge_online_enable", online_enable_);
        if (callbacks_.persist_config)
          callbacks_.persist_config();
        if (callbacks_.request_restart)
          callbacks_.request_restart();
      }
      if (empty_name)
        ImGui::EndDisabled();
      TextWrappedColored(kInkDim, "(the game restarts to apply name, server & port changes)");
      break;
    }
#endif
    case kTestingTab: {
      TextWrappedColored(
          kReticle,
          "TESTING CONTROLS CAN CHANGE GAMEPLAY AND MISSION RESULTS. Use a separate save or "
          "profile if you want clean progression.");
      ImGui::Spacing();
      TextWrappedColored(
          kInkDim,
          "These controls call the retail game's own verified functions. They are locked each "
          "time settings opens so they cannot be enabled accidentally.");
      TextWrappedColored(
          kReticle,
          "ACTIVE SINGLE-PLAYER GAMEPLAY IS PAUSED WHILE SETTINGS IS OPEN. Testing changes "
          "still run through the game's own safe game-thread routines.");
      ImGui::Spacing();

      if (!testing_tools_unlocked_) {
        const ImVec2 unlock_size(content_w * 0.85f, std::floor(fh * 0.07f));
        if (ImGui::Button("UNLOCK TESTING CONTROLS", unlock_size)) {
          testing_tools_unlocked_ = true;
          ge::testing::RequestRefresh();
        }
      } else {
        struct TestingToggle {
          ge::testing::Tool tool;
          const char* label;
          const char* warning;
        };
        struct TestingGroup {
          const char* title;
          const TestingToggle* toggles;
          size_t count;
        };

        static constexpr std::array<TestingToggle, 4> kPlayerToggles = {{
            {ge::testing::Tool::kGodMode, "God Mode", nullptr},
            {ge::testing::Tool::kInvisible, "Invisible", nullptr},
            {ge::testing::Tool::kTinyBond, "Tiny Bond", nullptr},
            {ge::testing::Tool::kTurboMode, "Turbo Mode",
             "Changes movement and gameplay timing."},
        }};
        static constexpr std::array<TestingToggle, 3> kWeaponToggles = {{
            {ge::testing::Tool::kAllGuns, "All Weapons", nullptr},
            {ge::testing::Tool::kInfiniteAmmo, "Infinite Ammo", nullptr},
            {ge::testing::Tool::kPaintballMode, "Paintball Mode", nullptr},
        }};
        static constexpr std::array<TestingToggle, 4> kWorldToggles = {{
            {ge::testing::Tool::kInvulnerableCharacters, "Invulnerable Characters",
             "Warning: may block objectives or mission completion."},
            {ge::testing::Tool::kNoRadar, "No Radar", nullptr},
            {ge::testing::Tool::kSlowMotion, "Slow Motion",
             "Changes simulation and mission timing."},
            {ge::testing::Tool::kStickInsects, "Stick Insects", nullptr},
        }};
        static constexpr std::array<TestingToggle, 3> kVisualToggles = {{
            {ge::testing::Tool::kBigHeads, "Big Heads", nullptr},
            {ge::testing::Tool::kFrescoMode, "Fresco Mode",
             "Visual effect may reduce scene readability."},
            {ge::testing::Tool::kVaselineVision, "Vaseline-o-vision",
             "Visual effect may reduce scene readability."},
        }};
        static constexpr TestingGroup kGroups[] = {
            {"PLAYER", kPlayerToggles.data(), kPlayerToggles.size()},
            {"WEAPONS", kWeaponToggles.data(), kWeaponToggles.size()},
            {"WORLD", kWorldToggles.data(), kWorldToggles.size()},
            {"VISUAL", kVisualToggles.data(), kVisualToggles.size()},
        };
        static_assert(kPlayerToggles.size() + kWeaponToggles.size() + kWorldToggles.size() +
                          kVisualToggles.size() ==
                      kTestingToggleCount);

        auto draw_testing_toggle = [&](const TestingToggle& toggle, int& pending_value) {
          const ge::testing::Tool tool = toggle.tool;
          const ge::testing::ToolState state = ge::testing::GetToolState(tool);
          const bool usable = state.supported && state.available && state.active_known;
          if (pending_value >= 0 &&
              (state.request_rejected || (!state.supported || !state.available) ||
               (state.active_known && state.active == (pending_value != 0)))) {
            pending_value = -1;
          }
          bool enabled =
              pending_value >= 0 ? pending_value != 0 : state.active_known && state.active;
          ImGui::BeginDisabled(!usable || pending_value >= 0);
          if (ImGui::Checkbox(toggle.label, &enabled)) {
            if (ge::testing::RequestSetEnabled(tool, enabled)) {
              pending_value = enabled ? 1 : 0;
              ge::testing::RequestRefresh();
            }
          }
          ImGui::EndDisabled();
          if (pending_value >= 0) {
            TextWrappedColored(kInkDim, "Applying on the game thread...");
          } else if (state.request_rejected) {
            TextWrappedColored(kReticle, "The game rejected this change.");
          } else if (!state.supported || !state.available) {
            TextWrappedColored(kInkDim, state.unavailable_reason);
          } else if (!state.active_known) {
            TextWrappedColored(kInkDim, "Reading current game state...");
          }
          if (toggle.warning != nullptr) {
            TextWrappedColored(kReticle, toggle.warning);
          }
        };

        size_t toggle_index = 0;
        for (const TestingGroup& group : kGroups) {
          ImGui::TextColored(ImColor(kTitle), "%s", group.title);
          ImGui::Spacing();
          for (size_t i = 0; i < group.count; ++i) {
            draw_testing_toggle(group.toggles[i], testing_pending_toggles_[toggle_index++]);
          }
          ImGui::Spacing();
          ImGui::Separator();
          ImGui::Spacing();
        }

        ImGui::TextColored(ImColor(kTitle), "GRAPHICS MODE");
        const ge::testing::ToolState graphics_state =
            ge::testing::GetToolState(ge::testing::Tool::kOriginalRemastered);
        if (testing_pending_graphics_mode_ >= 0 &&
            (!graphics_state.supported || !graphics_state.available ||
             !graphics_state.active_known ||
             graphics_state.active == (testing_pending_graphics_mode_ != 0))) {
          testing_pending_graphics_mode_ = -1;
        }
        const bool graphics_usable =
            graphics_state.supported && graphics_state.available && graphics_state.active_known;
        const char* graphics_button =
            graphics_state.active ? "SWITCH TO REMASTERED" : "SWITCH TO ORIGINAL";
        ImGui::BeginDisabled(!graphics_usable || testing_pending_graphics_mode_ >= 0);
        if (ImGui::Button(graphics_button, ImVec2(content_w * 0.85f, 0.0f))) {
          testing_pending_graphics_mode_ = graphics_state.active ? 0 : 1;
          ge::testing::RequestAction(ge::testing::Tool::kOriginalRemastered);
          ge::testing::RequestRefresh();
        }
        ImGui::EndDisabled();
        if (testing_pending_graphics_mode_ >= 0) {
          TextWrappedColored(kInkDim, "Switching on the game thread...");
        } else if (graphics_state.available && graphics_state.active_known) {
          ImGui::TextColored(ImColor(kInkDim), "Current: %s",
                             graphics_state.active ? "Original" : "Remastered");
        } else if (!graphics_state.supported || !graphics_state.available) {
          TextWrappedColored(kInkDim, graphics_state.unavailable_reason);
        } else {
          TextWrappedColored(kInkDim, "Reading current graphics mode...");
        }
        TextWrappedColored(kInkDim, "(F on keyboard or RB on controller also switches modes)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImColor(kTitle), "MISSION");
        const ge::testing::ToolState restart_state =
            ge::testing::GetToolState(ge::testing::Tool::kRestartMission);
        ImGui::BeginDisabled(true);
        ImGui::Button("RESTART MISSION", ImVec2(content_w * 0.85f, 0.0f));
        ImGui::EndDisabled();
        TextWrappedColored(kInkDim, restart_state.unavailable_reason);
      }
      break;
    }
    case kSystemTab:
    default: {
      const ImVec2 bsize(content_w * 0.85f, std::floor(fh * 0.07f));
      if (ImGui::Button("RESUME GAME", bsize)) {
        Close();
      }
      ImGui::Spacing();
      if (ImGui::Button("QUIT TO DESKTOP", bsize)) {
        quit_requested_ = true;
        Close();
      }
      break;
    }
  }

  ImGui::PopItemWidth();
  ImGui::SetWindowFontScale(1.0f);
  ImGui::EndChild();
  ImGui::PopStyleColor();  // ChildBg
  ImGui::PopStyleVar(2);

  // Footer hint.
  ImGui::SetWindowFontScale(1.15f * ui_scale);
  ImGui::SetCursorScreenPos(ImVec2(f0.x + pad, f1.y - std::floor(fh * 0.06f)));
  ImGui::TextColored(ImColor(kInkDim),
                     "Up/Down or click tabs  -  Pad LB/RB changes tabs  -  ESC/Pad B closes");
  ImGui::SetWindowFontScale(1.0f);

  ImGui::PopStyleColor(18);
}

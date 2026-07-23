// ge - ReXGlue Recompiled Project
//
// In-game pause menu, styled after the classic GoldenEye mission-briefing
// folder: a manila folder body with selectable tabs running down the right
// edge. Opened/closed with ESC; opening pauses the guest, closing resumes it.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/input/controller.h>
#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

#include <array>
#include <functional>

// Briefing-folder pause / settings menu.
//
// Lifetime: created with `new` and owned by the drawer's dialog list (it is a
// rex::ui::ImGuiDialog). It self-deletes through the base Draw() close path, so
// the owner (GeApp) keeps only a non-owning pointer and relies on the
// `on_closed` callback to clear it.
class GeMenuDialog : public rex::ui::ImGuiDialog {
 public:
  struct Callbacks {
    // Invoked exactly once as the dialog closes (clear owner ptr).
    std::function<void()> on_closed;
    // Invoked when the user picks "Quit to desktop".
    std::function<void()> on_quit;
    // Live window fullscreen state (read) and a request to change it (applied
    // deferred, off the paint thread, so it never tears down the live surface).
    std::function<bool()> get_fullscreen;
    std::function<void(bool)> request_fullscreen;
    // Notify the active presenter after the V-Sync cvar changes so the host
    // presentation mode can update without restarting the game.
    std::function<void(bool)> on_vsync_changed;
    // Persist current cvar values to the config file.
    std::function<void()> persist_config;
    // Save & relaunch. The ONLINE tab calls this after writing the username /
    // server / online-enable cvars so they take effect on a clean reboot (they
    // are read at startup, not live).
    std::function<void()> request_restart;
  };

  GeMenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks);
  ~GeMenuDialog() override;

  // Request a close from outside (e.g. the ESC keybind). Deferred to the end of
  // the current Draw(); on_closed fires then.
  void RequestClose();

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void DrawFolder(ImGuiIO& io);
  void DrawTabs(ImGuiIO& io);
  void DrawContent(ImGuiIO& io);
  void UpdateControllerSnapshot();
  void DrawControllerTest();

  Callbacks callbacks_;
  int selected_tab_ = 0;
  bool quit_requested_ = false;

  // Panel geometry, recomputed each frame in OnDraw and shared by the helpers.
  ImVec2 f0_{};      // folder body top-left (screen coords)
  ImVec2 f1_{};      // folder body bottom-right
  float tab_w_ = 0;  // width of the right-edge tab strip

  // INPUT tab: which keybind cvar is currently capturing a key (nullptr = none),
  // and a one-frame guard so the click that starts a rebind isn't itself bound.
  const char* rebinding_cvar_ = nullptr;
  int rebind_skip_ = 0;
  // ImGui nav flags saved while a rebind capture disables menu navigation (so
  // arrow/tab keys are captured for the bind, not used to switch tabs). Sentinel
  // 0xFFFFFFFF = nothing saved / not currently capturing.
  unsigned saved_nav_flags_ = 0xFFFFFFFFu;

  // Physical-controller state is sampled independently of guest input, so the
  // controller can navigate and test itself while this modal host menu keeps
  // the game input suppressed.
  rex::input::ControllerSnapshot controller_snapshot_;
  bool controller_snapshot_valid_ = false;
  bool testing_tools_unlocked_ = false;
  static constexpr unsigned kTestingToggleCount = 14;
  // One pending desired state per entry in the data-driven Cheats list.
  // -1 means idle; 0/1 means a game-thread update is still being applied.
  std::array<int, kTestingToggleCount> testing_pending_toggles_{};
  int testing_pending_graphics_mode_ = -1;
  int testing_unlock_confirm_action_ = -1;
  bool testing_unlock_action_submitted_ = false;
  bool performance_report_pending_ = false;

  // ONLINE tab edit state, loaded from the cvars the first time the tab shows
  // (so typing doesn't fight a per-frame reload). Applied on Save & Restart.
  bool online_loaded_ = false;
  bool online_enable_ = false;
  int online_port_ = 31000;     // ge_online_port (must match the server)
  char username_buf_[16] = {};  // ge_username (max 15 + null)
  char server_buf_[64] = {};    // ge_online_server (IP or hostname)
};

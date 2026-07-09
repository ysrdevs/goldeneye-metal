// ge - ReXGlue Recompiled Project
//
// First-boot username prompt. A simple centered box (briefing-folder styling,
// like the ESC menu) with a text field + OK button. Shown once, the first time
// the exe boots with no username set; inactive afterwards.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace ge {

class UsernameDialog : public rex::ui::ImGuiDialog {
 public:
  // on_set(name) fires when the user confirms a non-empty name.
  UsernameDialog(rex::ui::ImGuiDrawer* drawer, std::function<void(const std::string&)> on_set);
  ~UsernameDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::function<void(const std::string&)> on_set_;
  bool done_ = false;
  bool checked_ = false;  // resolved "already has a name?" once, after boot settles
  int delay_ = 0;
  bool focus_ = true;
  char buf_[16] = {};
};

}  // namespace ge

// ge - ReXGlue Recompiled Project
//
// Post-processing filter overlay. A passive, always-on ImGui dialog that draws
// a full-screen color filter over the guest image every frame, driven by the
// postfx_* cvars. Lets the user restyle the look (tint / brightness / vignette
// / scanlines) live, pick presets, save, or reset to default.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>

namespace ge {

// Full-screen filter layer. Created once at startup and kept alive; it renders
// under the pause menu (added later) but over the guest image. Non-interactive.
class PostFxOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit PostFxOverlay(rex::ui::ImGuiDrawer* drawer);
  ~PostFxOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// Built-in presets (index 0 == "Off"/default). Setting a preset writes the
// postfx_* cvars; the overlay picks the new values up next frame.
int PostFxPresetCount();
const char* PostFxPresetName(int index);
void ApplyPostFxPreset(int index);
void ResetPostFx();  // equivalent to ApplyPostFxPreset(0)

}  // namespace ge

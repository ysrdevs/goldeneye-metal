// ge - ReXGlue Recompiled Project
//
// Post-processing spatial overlay. The Metal and D3D12 presentation paths
// handle the per-pixel colour grade; this passive ImGui dialog draws vignette
// and scanlines over the guest image. The shared postfx_* cvars update both live.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>

namespace ge {

// Full-screen spatial-effect layer. It renders under the pause menu (added
// later) but over the guest image and is non-interactive.
class PostFxOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit PostFxOverlay(rex::ui::ImGuiDrawer* drawer);
  ~PostFxOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// Built-in presets (index 0 == "Off"/default). Setting a preset writes the
// postfx_* cvars; the GPU grade and overlay pick them up on the next frame.
int PostFxPresetCount();
const char* PostFxPresetName(int index);
void ApplyPostFxPreset(int index);
void ResetPostFx();  // equivalent to ApplyPostFxPreset(0)

// Whether the passive ImGui layer has visible work. Pure colour grading is
// handled by the native presentation shader and must not keep UI-thread
// presentation active on macOS.
bool PostFxSpatialEffectsEnabled();

}  // namespace ge

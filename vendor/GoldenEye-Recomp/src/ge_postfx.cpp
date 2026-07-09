// ge - ReXGlue Recompiled Project
//
// Post-processing filter overlay implementation. See ge_postfx.h.
//
// The filter is built from standard alpha-blended full-screen primitives (the
// only blend mode ImGui exposes), so it covers tint / brightness / vignette /
// scanlines. Per-pixel curve ops (contrast / saturation / gamma) would need a
// dedicated shader pass and are intentionally out of scope here.

#include "ge_postfx.h"

#include <rex/cvar.h>

#include <imgui.h>

#include <string>

// --- Tunable cvars (category "PostFX"; persisted via the menu's Save) ---
REXCVAR_DEFINE_BOOL(postfx_enabled, false, "PostFX", "Enable the post-processing filter");
REXCVAR_DEFINE_DOUBLE(postfx_brightness, 0.0, "PostFX", "Brightness offset (-1 dark .. +1 bright)")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint_r, 1.0, "PostFX", "Tint colour red (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint_g, 1.0, "PostFX", "Tint colour green (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint_b, 1.0, "PostFX", "Tint colour blue (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint, 0.0, "PostFX", "Tint strength (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_vignette, 0.0, "PostFX", "Vignette strength (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_scanlines, 0.0, "PostFX", "Scanline strength (0..1)").range(0.0, 1.0);
// Shader colour-grade params (applied by the GPU grade pass in the D3D12 swap).
REXCVAR_DEFINE_DOUBLE(postfx_contrast, 1.0, "PostFX", "Contrast (1=none)").range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(postfx_saturation, 1.0, "PostFX", "Saturation (1=none)").range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(postfx_vibrance, 0.0, "PostFX", "Vibrance (-1..1)").range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_temperature, 0.0, "PostFX", "Temperature, warm/cool (-1..1)")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_gamma, 1.0, "PostFX", "Gamma (1=none)").range(0.3, 3.0);

namespace ge {

namespace {

struct Preset {
  const char* name;
  bool enabled;
  float brightness, contrast, saturation, vibrance, temperature, gamma;  // shader grade
  float r, g, b, tint;                                                   // shader tint
  float vignette, scanlines;                                            // overlay
};

// Index 0 must be the neutral "Off" default.
const Preset kPresets[] = {
    // name        en    bri    con   sat   vib   tmp   gam   r     g     b    tint  vig  scan
    {"Off", false, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f},
    {"Cinematic", true, -0.04f, 1.15f, 1.05f, 0.15f, -0.10f, 1.0f, 1.0f, 0.95f, 0.85f, 0.12f, 0.40f,
     0.f},
    {"Sepia", true, -0.02f, 1.05f, 0.20f, 0.f, 0.25f, 1.0f, 1.0f, 0.82f, 0.55f, 0.50f, 0.30f, 0.f},
    {"Noir", true, -0.03f, 1.35f, 0.0f, 0.f, 0.f, 1.0f, 1.f, 1.f, 1.f, 0.f, 0.55f, 0.f},
    {"Cold", true, 0.f, 1.05f, 1.0f, 0.10f, -0.55f, 1.0f, 0.70f, 0.85f, 1.0f, 0.15f, 0.18f, 0.f},
    {"Warm", true, 0.02f, 1.05f, 1.05f, 0.15f, 0.55f, 1.0f, 1.0f, 0.85f, 0.60f, 0.12f, 0.18f, 0.f},
    {"Vibrant", true, 0.f, 1.10f, 1.20f, 0.50f, 0.f, 1.0f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f},
    {"Matrix", true, -0.03f, 1.10f, 0.85f, 0.20f, 0.f, 1.0f, 0.55f, 1.0f, 0.60f, 0.30f, 0.30f,
     0.25f},
    {"CRT", true, -0.02f, 1.05f, 1.10f, 0.10f, 0.f, 1.0f, 0.90f, 1.0f, 0.90f, 0.10f, 0.20f, 0.50f},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

void SetD(const char* name, float v) { rex::cvar::SetFlagByName(name, std::to_string(v)); }

}  // namespace

int PostFxPresetCount() { return kPresetCount; }

const char* PostFxPresetName(int index) {
  if (index < 0 || index >= kPresetCount) return "";
  return kPresets[index].name;
}

void ApplyPostFxPreset(int index) {
  if (index < 0 || index >= kPresetCount) return;
  const Preset& p = kPresets[index];
  rex::cvar::SetFlagByName("postfx_enabled", p.enabled ? "true" : "false");
  SetD("postfx_brightness", p.brightness);
  SetD("postfx_contrast", p.contrast);
  SetD("postfx_saturation", p.saturation);
  SetD("postfx_vibrance", p.vibrance);
  SetD("postfx_temperature", p.temperature);
  SetD("postfx_gamma", p.gamma);
  SetD("postfx_tint_r", p.r);
  SetD("postfx_tint_g", p.g);
  SetD("postfx_tint_b", p.b);
  SetD("postfx_tint", p.tint);
  SetD("postfx_vignette", p.vignette);
  SetD("postfx_scanlines", p.scanlines);
}

void ResetPostFx() { ApplyPostFxPreset(0); }

PostFxOverlay::PostFxOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
PostFxOverlay::~PostFxOverlay() = default;

void PostFxOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(postfx_enabled)) return;

  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  if (W <= 0.0f || H <= 0.0f) return;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin("##ge_postfx", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0(0, 0), p1(W, H);

  auto a8 = [](float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return static_cast<int>(f * 255.0f + 0.5f);
  };

  // NOTE: brightness / contrast / saturation / vibrance / temperature / gamma /
  // tint are all done per-pixel by the GPU grade shader (ge_grade.cs.hlsl) in
  // the D3D12 swap. This overlay only adds the spatial effects ImGui can do:
  // vignette and scanlines.

  // Vignette: four edge gradients (transparent inner -> dark edge) stack at the
  // corners to approximate a radial darkening.
  const float vig = static_cast<float>(REXCVAR_GET(postfx_vignette));
  if (vig > 0.001f) {
    const ImU32 dark = IM_COL32(0, 0, 0, a8(vig));
    const ImU32 clear = IM_COL32(0, 0, 0, 0);
    const float ext = 0.42f;  // fraction of the screen each edge band covers
    const float bw = W * ext, bh = H * ext;
    // top (dark at top edge -> clear downward)
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, bh), dark, dark, clear, clear);
    // bottom
    dl->AddRectFilledMultiColor(ImVec2(0, H - bh), ImVec2(W, H), clear, clear, dark, dark);
    // left
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bw, H), dark, clear, clear, dark);
    // right
    dl->AddRectFilledMultiColor(ImVec2(W - bw, 0), ImVec2(W, H), clear, dark, dark, clear);
  }

  // Scanlines (CRT): thin dark horizontal lines every few pixels.
  const float scan = static_cast<float>(REXCVAR_GET(postfx_scanlines));
  if (scan > 0.001f) {
    const ImU32 line = IM_COL32(0, 0, 0, a8(scan * 0.7f));
    for (float y = 0.0f; y < H; y += 3.0f) {
      dl->AddRectFilled(ImVec2(0, y), ImVec2(W, y + 1.0f), line);
    }
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace ge

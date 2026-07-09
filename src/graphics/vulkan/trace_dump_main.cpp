/**
 * Headless Vulkan GPU trace dump entry point.
 *
 * Replays a captured ReXGlue GPU trace through the (complete, reference)
 * Vulkan backend with no window — MoltenVK renders offscreen, so the macOS
 * windowed-surface limitation does not apply — and writes the resulting guest
 * output frame to disk. This is the ground-truth oracle for bringing up the
 * native Metal backend: "this is what the frame is supposed to look like."
 *
 * Usage: trace_dump_vulkan <trace_file.xtr> [output_base] [frame_index]
 */

#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/trace_dump.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/ui/flags.h>

namespace rex::graphics::vulkan {

class VulkanTraceDump final : public TraceDump {
 protected:
  std::unique_ptr<GraphicsSystem> CreateGraphicsSystem() override {
    return std::make_unique<VulkanGraphicsSystem>();
  }

  void BeginHostCapture() override {}
  void EndHostCapture() override {}
};

}  // namespace rex::graphics::vulkan

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  // Apple GPUs (via MoltenVK) have no geometry shaders -- Metal doesn't expose
  // them. The Vulkan backend's geometry-shader requirement is a preference with
  // non-geometry-shader fallbacks, so relax it to allow device selection.
  REXCVAR_SET(vulkan_require_geometry_shader, false);

  rex::graphics::vulkan::VulkanTraceDump dump;
  return dump.Main(args);
}

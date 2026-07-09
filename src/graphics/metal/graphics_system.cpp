#include <rex/graphics/metal/graphics_system.h>

#include <rex/graphics/metal/command_processor.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/metal/provider.h>

namespace rex::graphics::metal {

MetalGraphicsSystem::MetalGraphicsSystem() = default;

MetalGraphicsSystem::~MetalGraphicsSystem() = default;

std::string MetalGraphicsSystem::name() const {
  return "Metal";
}

void MetalGraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = rex::ui::metal::MetalProvider::Create(true, with_presentation);
}

std::unique_ptr<CommandProcessor> MetalGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<MetalCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::metal

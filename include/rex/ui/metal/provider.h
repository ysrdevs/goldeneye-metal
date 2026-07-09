#pragma once

#include <memory>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::metal {

class MetalProvider final : public GraphicsProvider {
 public:
  static std::unique_ptr<MetalProvider> Create(bool with_gpu_emulation, bool with_presentation);

  ~MetalProvider() override;

  void* metal_device() const { return metal_device_; }

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  explicit MetalProvider() = default;

  void* metal_device_ = nullptr;
};

}  // namespace rex::ui::metal

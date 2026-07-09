#include <rex/ui/metal/provider.h>

#import <Metal/Metal.h>

#include <rex/logging.h>
#include <rex/ui/metal/presenter.h>

#include <utility>

namespace rex::ui::metal {

std::unique_ptr<MetalProvider> MetalProvider::Create(bool with_gpu_emulation,
                                                     bool with_presentation) {
  (void)with_gpu_emulation;
  (void)with_presentation;

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    REXLOG_ERROR("Metal is unavailable: MTLCreateSystemDefaultDevice returned nil");
    return nullptr;
  }

  std::unique_ptr<MetalProvider> provider(new MetalProvider());
  provider->metal_device_ = device;
  REXLOG_INFO("Metal device: {}", [[device name] UTF8String]);
  return provider;
}

MetalProvider::~MetalProvider() {
  if (metal_device_) {
    [(id)metal_device_ release];
    metal_device_ = nullptr;
  }
}

std::unique_ptr<Presenter> MetalProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return MetalPresenter::Create(metal_device_, std::move(host_gpu_loss_callback));
}

std::unique_ptr<ImmediateDrawer> MetalProvider::CreateImmediateDrawer() {
  return nullptr;
}

}  // namespace rex::ui::metal

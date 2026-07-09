#include <rex/ui/surface_macos.h>

#import <QuartzCore/CAMetalLayer.h>

namespace rex::ui {

MacOSMetalLayerSurface::MacOSMetalLayerSurface(void* metal_layer) : metal_layer_(metal_layer) {}

Surface::TypeIndex MacOSMetalLayerSurface::GetType() const {
  return kTypeIndex_CAMetalLayer;
}

bool MacOSMetalLayerSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  if (!metal_layer_) {
    width_out = 0;
    height_out = 0;
    return false;
  }

  CAMetalLayer* layer = (CAMetalLayer*)metal_layer_;
  CGSize drawable_size = [layer drawableSize];
  width_out = drawable_size.width > 0.0 ? static_cast<uint32_t>(drawable_size.width) : 0;
  height_out = drawable_size.height > 0.0 ? static_cast<uint32_t>(drawable_size.height) : 0;
  return width_out != 0 && height_out != 0;
}

}  // namespace rex::ui

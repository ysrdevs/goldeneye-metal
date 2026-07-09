#pragma once

#include <rex/ui/surface.h>

namespace rex::ui {

class MacOSMetalLayerSurface final : public Surface {
 public:
  explicit MacOSMetalLayerSurface(void* metal_layer);

  void* metal_layer() const { return metal_layer_; }

  TypeIndex GetType() const override;

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  void* metal_layer_ = nullptr;
};

}  // namespace rex::ui

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>

namespace rex::ui::metal {

class MetalImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<MetalImmediateDrawer> Create(void* metal_device);

  ~MetalImmediateDrawer() override;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  enum class SamplerIndex : size_t {
    kNearestClamp,
    kLinearClamp,
    kNearestRepeat,
    kLinearRepeat,

    kCount,
  };

  class MetalImmediateTexture final : public ImmediateTexture {
   public:
    MetalImmediateTexture(uint32_t width, uint32_t height, void* metal_texture,
                          SamplerIndex sampler_index, MetalImmediateDrawer* immediate_drawer,
                          size_t immediate_drawer_index);
    ~MetalImmediateTexture() override;

    void OnImmediateDrawerDestroyed();

   private:
    friend class MetalImmediateDrawer;

    void* metal_texture_ = nullptr;
    SamplerIndex sampler_index_ = SamplerIndex::kNearestClamp;
    MetalImmediateDrawer* immediate_drawer_ = nullptr;
    size_t immediate_drawer_index_ = 0;
  };

  explicit MetalImmediateDrawer(void* metal_device);

  bool Initialize();
  void OnImmediateTextureDestroyed(MetalImmediateTexture& texture);
  void ResetBatch();

  void* metal_device_ = nullptr;
  void* pipeline_state_ = nullptr;
  void* white_texture_ = nullptr;
  std::array<void*, size_t(SamplerIndex::kCount)> samplers_ = {};

  std::vector<MetalImmediateTexture*> textures_;

  // Borrowed from MetalUIDrawContext between Begin and End.
  void* render_command_encoder_ = nullptr;

  // A shared buffer containing the current batch's vertices followed by its
  // optional 16-bit indices. Normal Metal command buffers retain resources
  // referenced while encoding, so this CPU ownership may be released as soon
  // as the batch has been encoded.
  void* batch_buffer_ = nullptr;
  size_t batch_index_offset_ = 0;
  int batch_vertex_count_ = 0;
  int batch_index_count_ = 0;
  bool batch_has_index_buffer_ = false;
  bool batch_open_ = false;

  void* current_texture_ = nullptr;
  SamplerIndex current_sampler_index_ = SamplerIndex::kCount;
  uint32_t current_scissor_left_ = 0;
  uint32_t current_scissor_top_ = 0;
  uint32_t current_scissor_width_ = 0;
  uint32_t current_scissor_height_ = 0;
};

}  // namespace rex::ui::metal

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

class MetalShader : public SpirvShader {
 public:
  class MetalTranslation : public SpirvTranslation {
   public:
    static constexpr uint32_t kMslInterpolatorCount = 16;

    struct MslReflection {
      uint32_t shared_memory_buffer_index = UINT32_MAX;
      uint32_t float_constants_buffer_index = UINT32_MAX;
      uint32_t bool_loop_constants_buffer_index = UINT32_MAX;
      uint32_t fetch_constants_buffer_index = UINT32_MAX;
      std::vector<uint32_t> texture_fetch_constants_by_binding_index;
      std::array<uint32_t, kMslInterpolatorCount> pixel_interpolators_by_location = {
          0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
      bool writes_shared_memory = false;
      bool is_void_fragment = false;
    };

    explicit MetalTranslation(MetalShader& shader, uint64_t modification)
        : SpirvTranslation(shader, modification) {}
    ~MetalTranslation() override;

    bool TranslateMslFromSpirv();
    bool CompileMslLibrary(void* metal_device, std::string* error_out);
    const std::string& msl_source() const { return msl_source_; }
    const MslReflection& msl_reflection() const { return msl_reflection_; }
    void* metal_library() const { return metal_library_; }

   private:
    void ReflectMslSource();

    std::string msl_source_;
    MslReflection msl_reflection_;
    void* metal_library_ = nullptr;
  };

  explicit MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian = std::endian::big);

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;
};

}  // namespace rex::graphics::metal

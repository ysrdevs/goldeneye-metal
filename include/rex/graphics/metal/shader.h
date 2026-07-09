#pragma once

#include <cstdint>
#include <string>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

class MetalShader : public SpirvShader {
 public:
  class MetalTranslation : public SpirvTranslation {
   public:
    explicit MetalTranslation(MetalShader& shader, uint64_t modification)
        : SpirvTranslation(shader, modification) {}
    ~MetalTranslation() override;

    bool TranslateMslFromSpirv();
    bool CompileMslLibrary(void* metal_device, std::string* error_out);
    const std::string& msl_source() const { return msl_source_; }
    void* metal_library() const { return metal_library_; }

   private:
    std::string msl_source_;
    void* metal_library_ = nullptr;
  };

  explicit MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian = std::endian::big);

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;
};

}  // namespace rex::graphics::metal

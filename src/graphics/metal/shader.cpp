#include <rex/graphics/metal/shader.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <regex>
#include <string_view>

#include <rex/graphics/metal/msl_compiler.h>
#include <rex/logging.h>

#if REX_HAS_SPIRV_CROSS_MSL
#include <spirv_cross/spirv_msl.hpp>
#endif

namespace rex::graphics::metal {
namespace {

bool ShaderDumpEnabled() {
  const char* value = std::getenv("GOLDENEYE_METAL_DUMP_SHADERS");
  return value && value[0] && std::strcmp(value, "0") != 0;
}

uint32_t ReplaceAll(std::string& source, std::string_view needle, std::string_view replacement) {
  uint32_t replacement_count = 0;
  size_t position = 0;
  while ((position = source.find(needle, position)) != std::string::npos) {
    source.replace(position, needle.size(), replacement);
    position += replacement.size();
    ++replacement_count;
  }
  return replacement_count;
}

uint32_t ReplaceDerivativeBuiltinWithZero(std::string& source, std::string_view builtin) {
  uint32_t replacement_count = 0;
  std::string needle = std::string(builtin) + "(";
  size_t position = 0;
  while ((position = source.find(needle, position)) != std::string::npos) {
    size_t argument_begin = position + needle.size();
    size_t scan = argument_begin;
    uint32_t depth = 1;
    for (; scan < source.size(); ++scan) {
      char c = source[scan];
      if (c == '(') {
        ++depth;
      } else if (c == ')') {
        --depth;
        if (!depth) {
          break;
        }
      }
    }
    if (scan >= source.size() || depth) {
      position = argument_begin;
      continue;
    }
    std::string argument = source.substr(argument_begin, scan - argument_begin);
    std::string replacement = "((" + argument + ") * 0.0)";
    source.replace(position, scan + 1 - position, replacement);
    position += replacement.size();
    ++replacement_count;
  }
  return replacement_count;
}

void SanitizeVertexMsl(std::string& source, uint64_t shader_hash, uint64_t modification) {
  // SPIRV-Cross lowers Xbox 360 vertex kill to discard_fragment in some cases,
  // but Metal only permits discard_fragment in fragment functions.
  const uint32_t stripped_discards =
      ReplaceAll(source, "discard_fragment();", "/* vertex discard stripped */");
  uint32_t stripped_derivatives = 0;
  stripped_derivatives += ReplaceDerivativeBuiltinWithZero(source, "dfdx");
  stripped_derivatives += ReplaceDerivativeBuiltinWithZero(source, "dfdy");
  stripped_derivatives += ReplaceDerivativeBuiltinWithZero(source, "fwidth");
  if (stripped_discards || stripped_derivatives) {
    std::fprintf(stderr,
                 "[metal] sanitized vertex-only MSL builtins shader=%016llx "
                 "modification=%016llx discards=%u derivatives=%u\n",
                 static_cast<unsigned long long>(shader_hash),
                 static_cast<unsigned long long>(modification), stripped_discards,
                 stripped_derivatives);
    std::fflush(stderr);
  }

  if (source.find("spvUnsafeArray<_RESERVED_IDENTIFIER_FIXUP_gl_PerVertex") != std::string::npos) {
    uint32_t replacements = 0;
    replacements += ReplaceAll(source, "_RESERVED_IDENTIFIER_FIXUP_gl_PerVertex", "main0_out");
    replacements += ReplaceAll(source, ".out.gl_Position", ".gl_Position");
    const std::regex rectangle_store_regex(
        R"(xe_var_rectangle_per_vertex\[([^\]]+)\]\s*=\s*_[0-9]+;)");
    uint32_t rectangle_store_replacements = 0;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), rectangle_store_regex);
         it != std::sregex_iterator(); ++it) {
      ++rectangle_store_replacements;
    }
    if (rectangle_store_replacements) {
      source = std::regex_replace(source, rectangle_store_regex,
                                  "xe_var_rectangle_per_vertex[$1] = out;");
      replacements += rectangle_store_replacements;
    }
    if (replacements) {
      std::fprintf(stderr,
                   "[metal] sanitized rectangle-list gl_PerVertex MSL "
                   "shader=%016llx modification=%016llx replacements=%u\n",
                   static_cast<unsigned long long>(shader_hash),
                   static_cast<unsigned long long>(modification), replacements);
      std::fflush(stderr);
    }
  }
}

void SanitizeDiagnosticPragmas(std::string& source) {
  constexpr std::string_view kUnusedVariablePragma =
      "#pragma clang diagnostic ignored \"-Wunused-variable\"\n";
  if (source.find(kUnusedVariablePragma) != std::string::npos) {
    return;
  }

  constexpr std::string_view kInsertAfter =
      "#pragma clang diagnostic ignored \"-Wmissing-braces\"\n";
  size_t insertion_position = source.find(kInsertAfter);
  if (insertion_position != std::string::npos) {
    insertion_position += kInsertAfter.size();
    source.insert(insertion_position, kUnusedVariablePragma);
    return;
  }

  source.insert(0, kUnusedVariablePragma);
}

void SanitizeSharedMemoryMsl(std::string& source, uint64_t shader_hash, uint64_t modification) {
  uint32_t replacement_count = 0;
  replacement_count += ReplaceAll(source, "const device XeSharedMemory& xe_shared_memory",
                                  "const device uint* xe_shared_memory");
  replacement_count += ReplaceAll(source, "device XeSharedMemory& xe_shared_memory",
                                  "device uint* xe_shared_memory");
  replacement_count += ReplaceAll(source, "xe_shared_memory.shared_memory[", "xe_shared_memory[");
  if (replacement_count) {
    std::fprintf(stderr,
                 "[metal] sanitized %u shared-memory MSL binding/access occurrence(s) "
                 "shader=%016llx modification=%016llx\n",
                 replacement_count, static_cast<unsigned long long>(shader_hash),
                 static_cast<unsigned long long>(modification));
    std::fflush(stderr);
  }
}

void DumpTranslatedMsl(const MetalShader& shader, uint64_t modification,
                       const std::string& source) {
  if (!ShaderDumpEnabled()) {
    return;
  }
  static std::atomic<uint32_t> dump_count{0};
  uint32_t dump_index = dump_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (dump_index > 64) {
    return;
  }
  const char* stage = shader.type() == xenos::ShaderType::kVertex ? "vert" : "frag";
  char path[256] = {};
  std::snprintf(path, sizeof(path), "/tmp/goldeneye_metal_msl_%s_%016llx_%016llx.metal", stage,
                static_cast<unsigned long long>(shader.ucode_data_hash()),
                static_cast<unsigned long long>(modification));
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    return;
  }
  std::fwrite(source.data(), 1, source.size(), file);
  std::fclose(file);
  if (dump_index <= 16) {
    std::fprintf(stderr, "[metal] dumped translated MSL#%u %s\n", dump_index, path);
    std::fflush(stderr);
  }
}

}  // namespace

MetalShader::MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                         const uint32_t* ucode_dwords, size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian) {}

Shader::Translation* MetalShader::CreateTranslationInstance(uint64_t modification) {
  return new MetalTranslation(*this, modification);
}

MetalShader::MetalTranslation::~MetalTranslation() {
  ReleaseMslLibrary(metal_library_);
  metal_library_ = nullptr;
}

bool MetalShader::MetalTranslation::TranslateMslFromSpirv() {
#if REX_HAS_SPIRV_CROSS_MSL
  const std::vector<uint8_t>& spirv_bytes = translated_binary();
  if (spirv_bytes.empty() || (spirv_bytes.size() & 3)) {
    REXLOG_ERROR("Metal MSL translation failed: invalid SPIR-V byte size {}", spirv_bytes.size());
    return false;
  }

  std::vector<uint32_t> spirv_words(spirv_bytes.size() / sizeof(uint32_t));
  std::memcpy(spirv_words.data(), spirv_bytes.data(), spirv_bytes.size());

  try {
    spirv_cross::CompilerMSL compiler(spirv_words);
    spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(3, 2, 0);
    compiler.set_msl_options(options);
    msl_source_ = compiler.compile();
    SanitizeDiagnosticPragmas(msl_source_);
    if (shader().type() == xenos::ShaderType::kVertex) {
      SanitizeVertexMsl(msl_source_, shader().ucode_data_hash(), modification());
    }
    SanitizeSharedMemoryMsl(msl_source_, shader().ucode_data_hash(), modification());
    DumpTranslatedMsl(static_cast<const MetalShader&>(shader()), modification(), msl_source_);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[metal] SPIRV-Cross MSL exception: %s\n", e.what());
    std::fflush(stderr);
    REXLOG_ERROR("Metal MSL translation failed: {}", e.what());
    return false;
  }

  return !msl_source_.empty();
#else
  REXLOG_ERROR("Metal MSL translation unavailable: SPIRV-Cross MSL support was not linked");
  return false;
#endif
}

bool MetalShader::MetalTranslation::CompileMslLibrary(void* metal_device, std::string* error_out) {
  if (metal_library_) {
    return true;
  }
  metal_library_ = CreateMslLibrary(metal_device, msl_source_, error_out);
  return metal_library_ != nullptr;
}

}  // namespace rex::graphics::metal

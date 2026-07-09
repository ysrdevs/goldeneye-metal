# rexgraphics: GPU emulation core (ported from Xenia)
# Namespace: rex::graphics

# Core GPU types - minimal dependencies, fully ported
set(REXGRAPHICS_CORE_SOURCES
    flags.cpp
    xenos.cpp
    pipeline/texture/info.cpp
    pipeline/texture/info_formats.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp
)

# GPU system files - full backend
set(REXGRAPHICS_SYSTEM_SOURCES
    graphics_system.cpp
    command_processor.cpp
    trace_writer.cpp
    trace_reader.cpp
    trace_player.cpp
    trace_dump.cpp
    pipeline/texture/extent.cpp
    pipeline/texture/util.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/texture/conversion.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGRAPHICS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGRAPHICS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# Metal backend
if(REXGLUE_USE_METAL)
    set(REXGRAPHICS_METAL_SOURCES
        metal/graphics_system.cpp
        metal/command_processor.cpp
        metal/draw_renderer.mm
        metal/msl_compiler.mm
        metal/shared_memory.mm
        metal/shader.cpp
        metal/texture_cache.mm
    )
endif()

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGRAPHICS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGRAPHICS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGRAPHICS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

# Combine sources
set(REXGRAPHICS_SOURCES
    ${REXGRAPHICS_CORE_SOURCES}
    ${REXGRAPHICS_SYSTEM_SOURCES}
)

if(REXGLUE_USE_VULKAN OR REXGLUE_USE_METAL)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_SPIRV_SOURCES})
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_DXBC_THIRDPARTY_SOURCES} ${REXGRAPHICS_DXBC_SOURCES} ${REXGRAPHICS_D3D12_SOURCES})
endif()

if(REXGLUE_USE_METAL)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_METAL_SOURCES})
endif()

add_library(rexgraphics OBJECT ${REXGRAPHICS_SOURCES})
add_library(rex::graphics ALIAS rexgraphics)

target_include_directories(rexgraphics PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexgraphics PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgraphics PRIVATE -Wno-unused-variable)

target_link_libraries(rexgraphics PUBLIC
    rexcore
    rexui
    xxHash::xxhash
    snappy
)

if(REXGLUE_USE_VULKAN OR REXGLUE_USE_METAL)
    target_link_libraries(rexgraphics PUBLIC
        glslang::SPIRV
    )
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgraphics PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
    )
    target_link_libraries(rexgraphics PRIVATE
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
    target_compile_definitions(rexgraphics PUBLIC REX_HAS_VULKAN=1)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgraphics PUBLIC
        d3d12
        dxgi
        dxguid
    )
    target_compile_definitions(rexgraphics PUBLIC REX_HAS_D3D12=1)
endif()

if(REXGLUE_USE_METAL)
    target_compile_definitions(rexgraphics PUBLIC REX_HAS_METAL=1)
    find_path(SPIRV_CROSS_INCLUDE_DIR
        NAMES spirv_cross/spirv_msl.hpp
        PATHS /opt/homebrew/include /usr/local/include
    )
    find_library(SPIRV_CROSS_MSL_LIBRARY
        NAMES spirv-cross-msl
        PATHS /opt/homebrew/lib /usr/local/lib
    )
    find_library(SPIRV_CROSS_GLSL_LIBRARY
        NAMES spirv-cross-glsl
        PATHS /opt/homebrew/lib /usr/local/lib
    )
    find_library(SPIRV_CROSS_CORE_LIBRARY
        NAMES spirv-cross-core
        PATHS /opt/homebrew/lib /usr/local/lib
    )
    if(SPIRV_CROSS_INCLUDE_DIR AND SPIRV_CROSS_MSL_LIBRARY AND
       SPIRV_CROSS_GLSL_LIBRARY AND SPIRV_CROSS_CORE_LIBRARY)
        target_include_directories(rexgraphics PRIVATE ${SPIRV_CROSS_INCLUDE_DIR})
        target_link_libraries(rexgraphics PRIVATE
            ${SPIRV_CROSS_MSL_LIBRARY}
            ${SPIRV_CROSS_GLSL_LIBRARY}
            ${SPIRV_CROSS_CORE_LIBRARY}
        )
        target_compile_definitions(rexgraphics PRIVATE REX_HAS_SPIRV_CROSS_MSL=1)
    else()
        message(FATAL_ERROR
            "SPIRV-Cross with MSL support is required for the Metal backend. "
            "On macOS, install it with: brew install spirv-cross")
    endif()
endif()

# ---------------------------------------------------------------------------
# Phase 0 isolation test: native Metal EDRAM resolve-copy compute path.
# Standalone headless executable (no game, no draw path). Allocates EDRAM + dest
# MTLBuffers, dispatches the kFast 32bpp 1x/2xMSAA resolve kernel, and asserts
# the GPU output equals a CPU reference untile/copy. Gate = byte-for-byte match
# (exit 0 PASS / exit 1 FAIL). See metal/resolve_copy_test.mm.
# ---------------------------------------------------------------------------
if(REXGLUE_USE_METAL)
    add_executable(metal_resolve_test
        metal/resolve_copy_test.mm
    )
    target_include_directories(metal_resolve_test PRIVATE
        ${REXGLUE_ROOT}/include
    )
    # rexcore brings the rex/graphics/xenos.h transitive headers; the frameworks
    # provide the Metal runtime used directly by the test.
    target_link_libraries(metal_resolve_test PRIVATE
        rexcore
        "-framework Metal"
        "-framework Foundation"
    )
    set_target_properties(metal_resolve_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
    if(REXGLUE_BUILD_TESTS)
        add_test(NAME metal_resolve_test COMMAND metal_resolve_test)
    endif()
endif()

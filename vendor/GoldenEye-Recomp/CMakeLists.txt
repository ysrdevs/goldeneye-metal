
# ge - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ge LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED REXSDK_DIR OR REXSDK_DIR STREQUAL "")
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
        "Path to the GoldenEye Metal SDK source tree" FORCE)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR
        "generated/rexglue.cmake is missing. Put your legally-owned Xbox 360 XEX at "
        "assets/default.xex, then run: rexglue codegen ge_manifest.toml")
endif()
include(generated/rexglue.cmake)

# Sources
set(GE_SOURCES
    src/main.cpp
    src/ge_hooks.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_username.cpp
    src/ge_ce_patches.cpp
)

if(WIN32)
    add_executable(ge WIN32 ${GE_SOURCES})
else()
    add_executable(ge ${GE_SOURCES})
endif()

# Output a GoldenEye.exe (the in-Explorer/process name), independent of the
# internal target/app name "ge" (which still drives ge.toml / the user data dir).
set_target_properties(ge PROPERTIES OUTPUT_NAME "GoldenEye")

rexglue_setup_target(ge)

# ---------------------------------------------------------------------------
# Headless GPU trace-dump oracle (graphics bring-up tooling, not the game).
#
# Replays a captured ReXGlue GPU trace (.xtr) through a backend with no window
# and writes the resulting guest output frame to disk (BMP + raw RGBA). The
# Vulkan backend is complete and its CaptureGuestOutput is implemented, so this
# produces the ground-truth reference image used to diff the native Metal
# backend during bring-up. It links the SDK runtime directly (NOT the
# recompiled game sources from rexglue_setup_target).
# ---------------------------------------------------------------------------
option(GOLDENEYE_BUILD_VULKAN_TRACE_DUMP
    "Build the source-tree-only Vulkan trace dump utility" OFF)
if(GOLDENEYE_BUILD_VULKAN_TRACE_DUMP)
    if(NOT REXGLUE_USE_VULKAN)
        message(FATAL_ERROR
            "GOLDENEYE_BUILD_VULKAN_TRACE_DUMP requires REXGLUE_USE_VULKAN=ON")
    endif()
    add_executable(trace_dump_vulkan
        ${REXSDK_DIR}/src/graphics/vulkan/trace_dump_main.cpp
    )
    target_link_libraries(trace_dump_vulkan PRIVATE rex::runtime)
    # Headers: rex::runtime is linked PRIVATE and doesn't re-export rexcore's
    # interface includes, so add them directly (mirrors rexglue_configure_target).
    if(TARGET rexcore)
        target_include_directories(trace_dump_vulkan PRIVATE
            $<TARGET_PROPERTY:rexcore,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    # Apply platform settings without injecting the windowed-app entry point;
    # this console tool provides its own main().
    rexglue_apply_target_settings(trace_dump_vulkan)
endif()

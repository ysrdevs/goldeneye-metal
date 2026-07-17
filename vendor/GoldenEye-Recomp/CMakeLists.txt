
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
    src/ge_game_data.cpp
    src/ge_hooks.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_username.cpp
    src/ge_ce_patches.cpp
)

if(APPLE)
    list(APPEND GE_SOURCES src/ge_launcher_macos.mm)
else()
    list(APPEND GE_SOURCES src/ge_launcher_stub.cpp)
endif()

if(WIN32)
    add_executable(ge WIN32 ${GE_SOURCES})
else()
    add_executable(ge ${GE_SOURCES})
endif()

# Output a GoldenEye.exe (the in-Explorer/process name), independent of the
# internal target/app name "ge" (which still drives ge.toml / the user data dir).
set_target_properties(ge PROPERTIES OUTPUT_NAME "GoldenEye")

rexglue_setup_target(ge)
if(NOT MSVC)
    target_compile_options(ge PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Local-only importer smoke tool. It is excluded from normal builds and never
# packaged; developers may invoke it explicitly with their own package to test
# the same validation/extraction backend used by the launcher.
add_executable(goldeneye_game_data_import_smoke EXCLUDE_FROM_ALL
    tests/game_data_import_smoke.cpp
    src/ge_game_data.cpp
)
target_include_directories(goldeneye_game_data_import_smoke PRIVATE src)
target_link_libraries(goldeneye_game_data_import_smoke PRIVATE rex::runtime)
rexglue_apply_target_settings(goldeneye_game_data_import_smoke)
if(NOT MSVC)
    target_compile_options(goldeneye_game_data_import_smoke PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# ---------------------------------------------------------------------------
# Unsigned macOS application bundle.
#
# Keep the developer executable as a normal target (the repository's legacy
# Finder launcher and command-line diagnostics still use it), then stage a
# release-ready .app explicitly with `--target goldeneye_macos_app`.  The
# staged product contains only the host executable, its native runtime dylib,
# metadata, and the source notices.  Game data is deliberately never copied.
# ---------------------------------------------------------------------------
if(APPLE)
    set(GOLDENEYE_VERSION "0.1.1" CACHE STRING
        "GoldenEye Metal CFBundleShortVersionString")
    set(GOLDENEYE_BUILD_NUMBER "2" CACHE STRING
        "GoldenEye Metal numeric CFBundleVersion")
    set(GOLDENEYE_BUNDLE_IDENTIFIER "io.github.ysrdevs.goldeneye-metal" CACHE STRING
        "GoldenEye Metal reverse-DNS bundle identifier")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "The macOS app requires CMAKE_OSX_DEPLOYMENT_TARGET to be set. "
            "Use the macos-arm64-release preset (currently macOS 14.0).")
    endif()
    set(GOLDENEYE_MACOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")

    if(NOT GOLDENEYE_VERSION MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
        message(FATAL_ERROR
            "GOLDENEYE_VERSION must contain two or three numeric components "
            "(for example, 0.1.1)")
    endif()
    if(NOT GOLDENEYE_BUILD_NUMBER MATCHES "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
        message(FATAL_ERROR
            "GOLDENEYE_BUILD_NUMBER must contain one to three numeric components")
    endif()
    if(NOT GOLDENEYE_BUNDLE_IDENTIFIER MATCHES "^[A-Za-z0-9][A-Za-z0-9.-]+$")
        message(FATAL_ERROR "GOLDENEYE_BUNDLE_IDENTIFIER is not a valid bundle identifier")
    endif()

    # A copied build-tree executable must resolve the runtime from inside the
    # bundle, never from an absolute path on the developer's Mac.  The legacy
    # launcher supplies DYLD_LIBRARY_PATH when running the unbundled binary.
    set_target_properties(ge PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    set(GOLDENEYE_MACOS_APP_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/dist/GoldenEye Metal.app")
    set(GOLDENEYE_MACOS_INFO_PLIST
        "${CMAKE_CURRENT_BINARY_DIR}/GoldenEyeMetal-Info.plist")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${GOLDENEYE_MACOS_INFO_PLIST}"
        @ONLY
    )

    # SPIRV-Cross is currently supplied by the host package manager rather
    # than the source tree.  A binary distribution must carry its license too.
    get_filename_component(_spirv_cross_prefix "${SPIRV_CROSS_INCLUDE_DIR}" DIRECTORY)
    find_file(GOLDENEYE_SPIRV_CROSS_LICENSE
        NAMES LICENSE LICENSE.txt LICENSE.md
        HINTS
            "${_spirv_cross_prefix}/opt/spirv-cross"
            "${_spirv_cross_prefix}/share/doc/spirv-cross"
        NO_DEFAULT_PATH
    )
    if(NOT GOLDENEYE_SPIRV_CROSS_LICENSE)
        message(FATAL_ERROR
            "Could not locate the SPIRV-Cross license beside the installed "
            "package; refusing to create an incomplete binary distribution")
    endif()

    # Always recreate the staged bundle. A stamp-based incremental target can
    # leave an accidentally added file in a release after the source that
    # created it has gone away.
    add_custom_target(goldeneye_macos_app
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP_BUNDLE=${GOLDENEYE_MACOS_APP_DIR}"
            "-DGAME_EXECUTABLE=$<TARGET_FILE:ge>"
            "-DRUNTIME_LIBRARY=$<TARGET_FILE:rexruntime>"
            "-DINFO_PLIST=${GOLDENEYE_MACOS_INFO_PLIST}"
            "-DAPP_ICON=${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/GoldenEyeMetal.icns"
            "-DROOT_LICENSE=${REXSDK_DIR}/LICENSE"
            "-DROOT_NOTICES=${REXSDK_DIR}/THIRD_PARTY_NOTICES.md"
            "-DGAME_LICENSE=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "-DGAME_NOTICES=${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "-DTHIRDPARTY_DIR=${REXSDK_DIR}/thirdparty"
            "-DSPIRV_CROSS_LICENSE=${GOLDENEYE_SPIRV_CROSS_LICENSE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/stage_app.cmake"
        DEPENDS
            ge
            rexruntime
            "${GOLDENEYE_MACOS_INFO_PLIST}"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/GoldenEyeMetal.icns"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/stage_app.cmake"
            "${REXSDK_DIR}/LICENSE"
            "${REXSDK_DIR}/THIRD_PARTY_NOTICES.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${GOLDENEYE_SPIRV_CROSS_LICENSE}"
        COMMENT "Staging unsigned GoldenEye Metal.app"
        VERBATIM
    )

    add_custom_target(goldeneye_macos_app_verify
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/verify_app.sh"
            "${GOLDENEYE_MACOS_APP_DIR}"
        DEPENDS goldeneye_macos_app
        COMMENT "Verifying unsigned GoldenEye Metal.app layout and linkage"
        VERBATIM
    )

    message(STATUS
        "GoldenEye Metal app bundle target: goldeneye_macos_app "
        "(${GOLDENEYE_MACOS_APP_DIR})")
endif()

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

# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

if(APPLE)
    add_test(
        NAME launcher.macos_syntax
        COMMAND /bin/bash -n "${PROJECT_SOURCE_DIR}/Launch GoldenEye.command"
    )
    set_tests_properties(launcher.macos_syntax PROPERTIES
        LABELS "launcher"
        TIMEOUT 10
    )

    add_test(
        NAME launcher.macos_behavior
        COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/tests/launcher/macos_launcher_test.sh"
    )
    set_tests_properties(launcher.macos_behavior PROPERTIES
        LABELS "launcher"
        TIMEOUT 20
    )
endif()

# PPC instruction tests require a separately installed cross-binutils toolchain.
if(REXGLUE_BUILD_PPC_TESTS)
    add_subdirectory(ppc)
endif()

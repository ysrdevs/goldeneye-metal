# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

if(UNIX)
    # The automation tools use Python 3.10's PEP 604 union syntax.
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        foreach(_tool_test IN ITEMS
                metal_profile_parser_test
                render_regression_test
                stability_cycle_test)
            add_test(
                NAME tools.${_tool_test}
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/tools/${_tool_test}.py"
            )
            set_tests_properties(tools.${_tool_test} PROPERTIES
                LABELS "tools"
                TIMEOUT 60
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            )
        endforeach()

        if(APPLE)
            add_test(
                NAME tools.benchmark_dam_test
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/tools/benchmark_dam_test.py"
            )
            set_tests_properties(tools.benchmark_dam_test PROPERTIES
                LABELS "tools;macos"
                TIMEOUT 60
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            )
        endif()
    endif()
endif()

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

    add_test(
        NAME tools.macos_app_control_syntax
        COMMAND /usr/bin/xcrun clang
            -fobjc-arc
            -fmodules-cache-path=${CMAKE_BINARY_DIR}/clang-module-cache
            -fsyntax-only
            "${PROJECT_SOURCE_DIR}/tools/macos_app_control.m"
    )
    set_tests_properties(tools.macos_app_control_syntax PROPERTIES
        LABELS "tools;macos"
        TIMEOUT 30
    )
endif()

# PPC instruction tests require a separately installed cross-binutils toolchain.
if(REXGLUE_BUILD_PPC_TESTS)
    add_subdirectory(ppc)
endif()

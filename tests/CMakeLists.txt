# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests require a separately installed cross-binutils toolchain.
if(REXGLUE_BUILD_PPC_TESTS)
    add_subdirectory(ppc)
endif()

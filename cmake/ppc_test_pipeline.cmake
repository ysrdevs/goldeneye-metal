# cmake/ppc_test_pipeline.cmake
# PPC assembly test build pipeline
# Provides ppc_add_test_binary() for assembling, linking, and generating
# symbol maps from PPC assembly test files.

# Locate an externally installed PPC cross-binutils toolchain. Prebuilt GPL
# executables are intentionally not committed to this source repository.
set(REXGLUE_PPC_TOOL_PREFIX "powerpc-none-elf" CACHE STRING
    "Executable prefix for the PPC cross-binutils toolchain")
find_program(PPC_ASSEMBLER NAMES "${REXGLUE_PPC_TOOL_PREFIX}-as" REQUIRED)
find_program(PPC_LINKER NAMES "${REXGLUE_PPC_TOOL_PREFIX}-ld" REQUIRED)
find_program(PPC_NM NAMES "${REXGLUE_PPC_TOOL_PREFIX}-nm" REQUIRED)

message(STATUS "PowerPC assembler: ${PPC_ASSEMBLER}")
message(STATUS "PowerPC linker: ${PPC_LINKER}")

# Convert Windows paths to Cygwin paths for binutils (Windows only)
function(_ppc_convert_to_cygwin_path WINDOWS_PATH OUTPUT_VAR)
    if(WIN32 AND WINDOWS_PATH MATCHES "^([A-Za-z]):")
        string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" CYGWIN_PATH "${WINDOWS_PATH}")
        string(SUBSTRING "${CYGWIN_PATH}" 10 1 DRIVE_LETTER)
        string(TOLOWER "${DRIVE_LETTER}" DRIVE_LOWER)
        string(REGEX REPLACE "^/cygdrive/[A-Za-z]" "/cygdrive/${DRIVE_LOWER}" CYGWIN_PATH "${CYGWIN_PATH}")
        string(REPLACE "\\" "/" CYGWIN_PATH "${CYGWIN_PATH}")
        set(${OUTPUT_VAR} "${CYGWIN_PATH}" PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} "${WINDOWS_PATH}" PARENT_SCOPE)
    endif()
endfunction()

# Add a PPC test binary from an assembly file.
# Assembles .s -> .o, links .o -> .bin, generates .o -> .map.
# Appends output files to PPC_TEST_BINS, PPC_TEST_MAPS, PPC_TEST_OBJS
# global properties.
function(ppc_add_test_binary ASM_FILE OBJ_DIR BIN_DIR)
    get_filename_component(BASE_NAME ${ASM_FILE} NAME_WE)

    set(OBJ_FILE ${OBJ_DIR}/${BASE_NAME}.o)
    set(BIN_FILE ${BIN_DIR}/${BASE_NAME}.bin)
    set(MAP_FILE ${BIN_DIR}/${BASE_NAME}.map)

    _ppc_convert_to_cygwin_path("${OBJ_FILE}" OBJ_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${BIN_FILE}" BIN_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${MAP_FILE}" MAP_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${ASM_FILE}" ASM_FILE_CYGWIN)

    # Assemble .s to .o
    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${PPC_ASSEMBLER}
                -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 -R
                -o ${OBJ_FILE_CYGWIN}
                ${ASM_FILE_CYGWIN}
        DEPENDS ${ASM_FILE}
        COMMENT "Assembling ${BASE_NAME}.s"
        VERBATIM
    )

    # Link .o to .bin
    add_custom_command(
        OUTPUT ${BIN_FILE}
        COMMAND ${PPC_LINKER}
                -A powerpc:common32 -melf32ppc -EB -nostdlib
                --oformat=binary -Ttext=0x82010000 -e 0x82010000
                -o ${BIN_FILE_CYGWIN}
                ${OBJ_FILE_CYGWIN}
        DEPENDS ${OBJ_FILE}
        COMMENT "Linking ${BASE_NAME}.bin"
        VERBATIM
    )

    # Generate symbol map
    add_custom_command(
        OUTPUT ${MAP_FILE}
        COMMAND ${PPC_NM} --numeric-sort ${OBJ_FILE_CYGWIN} > ${MAP_FILE}
        DEPENDS ${OBJ_FILE}
        COMMENT "Generating symbol map for ${BASE_NAME}"
        VERBATIM
    )

    set_property(GLOBAL APPEND PROPERTY PPC_TEST_BINS ${BIN_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_MAPS ${MAP_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_OBJS ${OBJ_FILE})
endfunction()

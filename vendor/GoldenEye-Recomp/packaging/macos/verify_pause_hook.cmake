if(NOT DEFINED GAME_EXECUTABLE OR NOT EXISTS "${GAME_EXECUTABLE}")
    message(FATAL_ERROR "GAME_EXECUTABLE is missing: ${GAME_EXECUTABLE}")
endif()

execute_process(
    COMMAND /usr/bin/nm -m "${GAME_EXECUTABLE}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_OUTPUT
    ERROR_VARIABLE NM_ERROR
)
if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "nm failed for ${GAME_EXECUTABLE}: ${NM_ERROR}")
endif()

# Generated guest functions are weak on macOS so title-specific source may
# provide strong wrappers. Proper pause relies on every retail setter call
# resolving to the strong sub_8209F578 wrapper, with the generated body retained
# separately as __imp__sub_8209F578 for host-owned writes.
string(FIND "${NM_OUTPUT}" " weak external _sub_8209F578" WEAK_SETTER)
string(FIND "${NM_OUTPUT}" " external _sub_8209F578" STRONG_SETTER)
string(FIND "${NM_OUTPUT}" " external ___imp__sub_8209F578" ORIGINAL_SETTER)
if(NOT WEAK_SETTER EQUAL -1 OR STRONG_SETTER EQUAL -1 OR ORIGINAL_SETTER EQUAL -1)
    message(FATAL_ERROR
        "GoldenEye pause setter override is not strongly linked in ${GAME_EXECUTABLE}")
endif()

message(STATUS "GoldenEye pause setter override linkage verified")

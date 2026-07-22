if(NOT DEFINED GENERATED_DIRECTORY OR NOT IS_DIRECTORY "${GENERATED_DIRECTORY}")
    message(FATAL_ERROR
        "Generated GoldenEye directory is missing; rerun rexglue codegen before packaging")
endif()

file(GLOB generated_sources "${GENERATED_DIRECTORY}/ge_recomp.*.cpp")
set(function_start -1)
foreach(generated_path IN LISTS generated_sources)
    file(READ "${generated_path}" candidate_source)
    string(FIND "${candidate_source}" "DEFINE_REX_FUNC(sub_823DFB70)" candidate_start)
    if(NOT candidate_start LESS 0)
        set(generated_source "${candidate_source}")
        set(function_start ${candidate_start})
        break()
    endif()
endforeach()

if(function_start LESS 0)
    message(FATAL_ERROR "Generated source does not contain sub_823DFB70")
endif()

string(LENGTH "${generated_source}" generated_length)
math(EXPR tail_length "${generated_length} - ${function_start}")
string(SUBSTRING "${generated_source}" ${function_start} ${tail_length} function_tail)
string(FIND "${function_tail}" "\nDEFINE_REX_FUNC(" next_function)
if(next_function LESS 0)
    set(function_body "${function_tail}")
else()
    string(SUBSTRING "${function_tail}" 0 ${next_function} function_body)
endif()

string(REGEX MATCHALL
    "ge_skip_packed_data_purecall_(header|value)\\("
    guard_calls "${function_body}")
list(LENGTH guard_calls guard_call_count)
if(NOT guard_call_count EQUAL 2)
    message(FATAL_ERROR
        "sub_823DFB70 must contain exactly two pre-dispatch purecall guards; "
        "rerun rexglue codegen")
endif()

foreach(guard IN ITEMS header value)
    if(guard STREQUAL "header")
        set(return_address "0x823DFBAC")
        set(expected_call
            "ge_skip_packed_data_purecall_header(ctx.r11, ctx.r3, ctx.r31, ctx.r1)")
        set(post_call_guard "ge_guard_packed_data_header(")
    else()
        set(return_address "0x823DFBD4")
        set(expected_call
            "ge_skip_packed_data_purecall_value(ctx.r11, ctx.r3, ctx.r30, ctx.r31, ctx.r1)")
        set(post_call_guard "ge_guard_packed_data_value(")
    endif()

    string(FIND "${function_body}" "${expected_call}" guard_position)
    string(FIND "${function_body}"
        "ctx.lr = ${return_address};" dispatch_position)
    string(FIND "${function_body}" "${post_call_guard}" post_call_guard_position)
    if(guard_position LESS 0 OR dispatch_position LESS 0 OR
       post_call_guard_position LESS 0 OR
       NOT guard_position LESS dispatch_position OR
       NOT dispatch_position LESS post_call_guard_position)
        message(FATAL_ERROR
            "The ${guard} purecall guard has incorrect register wiring or is not "
            "before its original virtual dispatch")
    endif()

    math(EXPR guarded_region_length "${dispatch_position} - ${guard_position}")
    string(SUBSTRING "${function_body}" ${guard_position}
        ${guarded_region_length} guarded_region)
    string(FIND "${guarded_region}" "goto loc_823DFBDC;" epilogue_jump)
    if(epilogue_jump LESS 0)
        message(FATAL_ERROR
            "The ${guard} purecall guard does not use sub_823DFB70's normal epilogue")
    endif()

    math(EXPR dispatch_region_length
        "${post_call_guard_position} - ${dispatch_position}")
    string(SUBSTRING "${function_body}" ${dispatch_position}
        ${dispatch_region_length} dispatch_region)
    set(indirect_call "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);")
    string(LENGTH "${dispatch_region}" dispatch_region_size)
    string(LENGTH "${indirect_call}" indirect_call_size)
    string(REPLACE "${indirect_call}" "" dispatch_without_indirect
        "${dispatch_region}")
    string(LENGTH "${dispatch_without_indirect}" dispatch_without_indirect_size)
    math(EXPR removed_indirect_size
        "${dispatch_region_size} - ${dispatch_without_indirect_size}")
    if(NOT removed_indirect_size EQUAL indirect_call_size)
        message(FATAL_ERROR
            "The ${guard} purecall guard must retain exactly one original "
            "indirect dispatch")
    endif()
endforeach()

message(STATUS "Verified both sub_823DFB70 pre-dispatch purecall guards")

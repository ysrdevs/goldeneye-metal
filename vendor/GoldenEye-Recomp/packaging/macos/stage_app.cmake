cmake_minimum_required(VERSION 3.25)

foreach(required_var IN ITEMS
        APP_BUNDLE GAME_EXECUTABLE RUNTIME_LIBRARY INFO_PLIST APP_ICON
        ROOT_LICENSE ROOT_NOTICES GAME_LICENSE GAME_NOTICES THIRDPARTY_DIR
        SPIRV_CROSS_LICENSE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "stage_app.cmake requires -D${required_var}=...")
    endif()
endforeach()

foreach(required_file IN ITEMS
        GAME_EXECUTABLE RUNTIME_LIBRARY INFO_PLIST APP_ICON
        ROOT_LICENSE ROOT_NOTICES GAME_LICENSE GAME_NOTICES SPIRV_CROSS_LICENSE)
    if(NOT EXISTS "${${required_file}}")
        message(FATAL_ERROR "Required bundle input is missing: ${${required_file}}")
    endif()
endforeach()

get_filename_component(runtime_name "${RUNTIME_LIBRARY}" NAME)
set(contents_dir "${APP_BUNDLE}/Contents")
set(macos_dir "${contents_dir}/MacOS")
set(frameworks_dir "${contents_dir}/Frameworks")
set(resources_dir "${contents_dir}/Resources")
set(licenses_dir "${resources_dir}/Licenses")

# Recreate the output so a removed dependency or resource cannot linger in a
# release.  This directory lives entirely inside the ignored build tree.
file(REMOVE_RECURSE "${APP_BUNDLE}")
file(MAKE_DIRECTORY "${macos_dir}" "${frameworks_dir}" "${licenses_dir}")

file(COPY_FILE "${GAME_EXECUTABLE}" "${macos_dir}/GoldenEye")
file(COPY_FILE "${RUNTIME_LIBRARY}" "${frameworks_dir}/${runtime_name}")
file(COPY_FILE "${INFO_PLIST}" "${contents_dir}/Info.plist")
file(COPY_FILE "${APP_ICON}" "${resources_dir}/GoldenEyeMetal.icns")
file(COPY_FILE "${ROOT_LICENSE}" "${licenses_dir}/ReXGlue-LICENSE.txt")
file(COPY_FILE "${ROOT_NOTICES}" "${licenses_dir}/ReXGlue-THIRD_PARTY_NOTICES.md")
file(COPY_FILE "${GAME_LICENSE}" "${licenses_dir}/GoldenEye-Metal-LICENSE.txt")
file(COPY_FILE "${GAME_NOTICES}" "${licenses_dir}/GoldenEye-Metal-THIRD_PARTY_NOTICES.md")
file(COPY_FILE "${SPIRV_CROSS_LICENSE}" "${licenses_dir}/SPIRV-Cross-LICENSE.txt")

# Preserve the authoritative notices from source dependencies linked into the
# application.  Copy all top-level LICENSE/COPYING/NOTICE variants found in
# dependency trees: including an unused dependency's notice is harmless,
# whereas omitting a required notice from a static binary is not.
file(GLOB_RECURSE thirdparty_license_files LIST_DIRECTORIES FALSE
    "${THIRDPARTY_DIR}/LICENSE"
    "${THIRDPARTY_DIR}/LICENSE.*"
    "${THIRDPARTY_DIR}/COPYING"
    "${THIRDPARTY_DIR}/COPYING.*"
    "${THIRDPARTY_DIR}/NOTICE"
    "${THIRDPARTY_DIR}/NOTICE.*")
foreach(license_file IN LISTS thirdparty_license_files)
    file(RELATIVE_PATH license_relative "${THIRDPARTY_DIR}" "${license_file}")
    string(REPLACE "/" "__" license_name "${license_relative}")
    file(COPY_FILE "${license_file}" "${licenses_dir}/${license_name}")
endforeach()

# Preserve executable bits even when the build tree is copied through a file
# system whose defaults are restrictive.
file(CHMOD "${macos_dir}/GoldenEye"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(CHMOD "${frameworks_dir}/${runtime_name}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

message(STATUS "Staged unsigned application: ${APP_BUNDLE}")

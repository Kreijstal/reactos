#reactos.dff

if(ARCH STREQUAL "amd64")
    set(_wine_gecko_filename "wine_gecko-2.40-x86_64.msi")
    set(_wine_gecko_url "https://dl.winehq.org/wine/wine-gecko/2.40/${_wine_gecko_filename}")
    set(_wine_gecko_sha1_expected "356c414527b08b015b6dc53dbbf78d752320ae90")
else()
    set(_wine_gecko_filename "wine_gecko-2.40-x86.msi")
    set(_wine_gecko_url "https://download.reactos.org/reactos/Wine-Gecko/${_wine_gecko_filename}")
    set(_wine_gecko_sha1_expected "8a3adedf3707973d1ed4ac3b2e791486abf814bd")
endif()
set(WINE_GECKO_CAB_SOURCE "modules/optional/${_wine_gecko_filename}")
if(DOWNLOAD_WINE_GECKO)
    set(_wine_gecko_dir "${CMAKE_CURRENT_BINARY_DIR}/downloads")
    set(WINE_GECKO_CAB_SOURCE "${_wine_gecko_dir}/${_wine_gecko_filename}")
    file(MAKE_DIRECTORY "${_wine_gecko_dir}")
    if(NOT EXISTS "${WINE_GECKO_CAB_SOURCE}")
        message(STATUS "Downloading Wine Gecko 2.40 (x86)")
        file(DOWNLOAD
            "${_wine_gecko_url}"
            "${WINE_GECKO_CAB_SOURCE}"
            EXPECTED_HASH SHA1=${_wine_gecko_sha1_expected}
            TLS_VERIFY ON
            SHOW_PROGRESS)
    else()
        file(SHA1 "${WINE_GECKO_CAB_SOURCE}" _wine_gecko_sha1)
        if(NOT _wine_gecko_sha1 STREQUAL "${_wine_gecko_sha1_expected}")
            message(FATAL_ERROR "Cached Wine Gecko MSI has an unexpected SHA-1")
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY REACTOS_CAB_DEPENDS "${WINE_GECKO_CAB_SOURCE}")
    # LiveCD cannot install into its read-only system directory.  Expand Gecko
    # while assembling the image and run it directly from the boot medium.
    find_program(MSIEXTRACT_EXECUTABLE msiextract REQUIRED)
    set(_wine_gecko_image_dir "${_wine_gecko_dir}/image")
    file(REMOVE_RECURSE "${_wine_gecko_image_dir}")
    file(MAKE_DIRECTORY "${_wine_gecko_image_dir}")
    execute_process(
        COMMAND "${MSIEXTRACT_EXECUTABLE}" -C "${_wine_gecko_image_dir}" "${WINE_GECKO_CAB_SOURCE}"
        RESULT_VARIABLE _wine_gecko_extract_result
        OUTPUT_QUIET)
    if(NOT _wine_gecko_extract_result EQUAL 0)
        message(FATAL_ERROR "Unable to extract Wine Gecko MSI")
    endif()
    file(GLOB_RECURSE _wine_gecko_image_files LIST_DIRECTORIES FALSE
         "${_wine_gecko_image_dir}/*")
    foreach(_wine_gecko_file IN LISTS _wine_gecko_image_files)
        file(RELATIVE_PATH _wine_gecko_relative "${_wine_gecko_image_dir}" "${_wine_gecko_file}")
        get_filename_component(_wine_gecko_relative_dir "${_wine_gecko_relative}" DIRECTORY)
        add_cd_file(FILE "${_wine_gecko_file}"
                    DESTINATION "reactos/${_wine_gecko_relative_dir}"
                    NO_CAB FOR livecd)
    endforeach()
endif()

configure_file(reactos.dff.in reactos.dff.static @ONLY)

# reactos.dff is the concatenation of two files:
# - reactos.dff.in, which is a static one and can be altered to
#   add custom modules/files to reactos.cab
# - reactos.dff.dyn (dyn as in dynamic) which is generated at generation
#   time by our cmake scripts (from reactos.dff.cmake, which contains
#   generator expressions)
# If you want to slip-stream anything into the bootcd, then you want to alter reactos.dff.in

# Idea taken from there : http://www.cmake.org/pipermail/cmake/2010-July/038028.html
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/concat.cmake "
    file(READ \${SRC1} S1)
    file(READ \${SRC2} S2)
    file(WRITE \${DST} \"\${S1}\${S2}\")
")

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.cmake "")

# This generates reactos.dff.dyn by processing the generator expressions
file(GENERATE
     OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.$<CONFIG>.dyn
     INPUT ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.cmake)

# This finalizes reactos.dff by concat-ing the two files: one generated and one static containing the optional file.
# please keep it this way as it permits to add files to reactos.dff.in without having to run cmake again
# and also avoids rebuilding reactos.cab in case nothing changes after a cmake run
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff
    COMMAND ${CMAKE_COMMAND} -D SRC1=${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.static
                             -D SRC2=${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.$<CONFIG>.dyn
                             -D DST=${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.maydiff
                             -P ${CMAKE_CURRENT_BINARY_DIR}/concat.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                             ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.maydiff
                             ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.static
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff.$<CONFIG>.dyn)

add_custom_command(
    COMMAND native-cabman -C ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff -L ${CMAKE_CURRENT_BINARY_DIR} -I -P ${REACTOS_SOURCE_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/reactos.dff native-cabman
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/reactos.inf)

add_custom_target(
    reactos_cab_inf DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/reactos.inf)

add_cd_file(
    TARGET reactos_cab_inf
    FILE ${CMAKE_CURRENT_BINARY_DIR}/reactos.inf
    DESTINATION reactos
    NO_CAB FOR bootcd regtest)

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

# ---------------------------------------------------------------------------
# Intel Wireless (iwlwifi) firmware
#
# Intel wireless parts from family 7000 on run signed device firmware that
# does the PHY bring-up, calibration, scanning and association the host
# driver would otherwise have to program register by register.  The blob is
# therefore not an optional extra - without it the NIC does nothing at all.
#
# Fetched from linux-firmware at configure time and pinned by SHA-256,
# exactly like DOWNLOAD_WINE_GECKO above.  Nothing is committed to this
# tree.  Intel's linux-firmware licence permits verbatim binary
# redistribution, so shipping what we download is fine; pinning the hash
# is what makes the download reproducible rather than "whatever is on the
# server today".
#
# NOTE ON THE PATH: linux-firmware moved these blobs out of the repository
# root into intel/iwlwifi/ - the old root-level URLs now 404.
#
# One .ucode per (MAC, RF) pair at the newest API revision this driver
# claims in hw/devices.c, plus a .pnvm for every part flagged
# IWL_CFG_NEEDS_PNVM.  iwlwifi walks DOWN from UcodeApiMax, so a
# missing newest blob is not fatal as long as an older one is present.
if(DOWNLOAD_IWLWIFI_UCODE)
    set(_iwlwifi_base
        "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/intel/iwlwifi")
    set(_iwlwifi_dir "${CMAKE_CURRENT_BINARY_DIR}/downloads/iwlwifi")

    # filename|sha256 - "|" and not ";", because CMake would flatten a
    # semicolon into two separate list elements and silently divorce every
    # pin from its file.  Keep in step with UcodeApiMax in
    # drivers/network/dd/iwlwifi/hw/devices.c.
    set(_iwlwifi_blobs
        "iwlwifi-5000-5.ucode|da259ae4b436f4d166aad2139f23694c2737e25288095140a1809d60fe676247"
        "iwlwifi-6000g2a-6.ucode|9c435c7f413ff2ff1320604c68beb6e8239bb93522dc4b7c02c8cc94a393d88a"
        "iwlwifi-7265D-29.ucode|c52bcaa0595f64d5fb32b7db3dd4b832ea7bf78f1037bcafc19d9c0f5c5c617e"
        "iwlwifi-3168-29.ucode|3afae7bf4bb5fbb83a895ec7ad097b9fd20b7d8794f405c2823cbea5284e65c3"
        "iwlwifi-8265-36.ucode|1336afcd028ed094d1fe33893c84c273bb5711be52970040344a75a12f276d56"
        "iwlwifi-9260-th-b0-jf-b0-46.ucode|05f4c12514fddfd56fea660b3d2eb6b6e6f4a87a324d5d40464050312ddca911"
        "iwlwifi-9000-pu-b0-jf-b0-46.ucode|9a0c5d938646942f9798e6212579c25771e4bb44301cd409b82abe361ca2c3e0"
        "iwlwifi-cc-a0-77.ucode|94f5fb915f2074f98059365e11cca313e25a6fa3b7cd6772e80b3ba5edfb966b"
        "iwlwifi-Qu-b0-hr-b0-77.ucode|2d3dd82853e1f5859de168fc4c3e2078703cb5c62c38ae39658c28ca5b664443"
        "iwlwifi-ty-a0-gf-a0-89.ucode|cd4f8fe2242c115cf8687878c2534fbb436fbaa7172f49a1f69952a23c686ac9"
        "iwlwifi-so-a0-gf-a0-89.ucode|698c913f938d9a8adce7ab9c049b1cd37daaa7473d8185f93e98f22733092f6b"
        "iwlwifi-gl-c0-fm-c0-101.ucode|6769d9ce002bb404fbd0c39e28f3c68af4ec1583343ac66a98ca957710c3473b"
        "iwlwifi-bz-b0-fm-c0-101.ucode|f40a5186977d84cb6ef2834e4ab87fd915ea77899b8347c56f1f95e776359a41"

        # Platform NVM.  AX210 and later only - the earlier families carry
        # the equivalent data on-die.  Same base name as the ucode but with
        # no API revision, and pushed to the device only AFTER the firmware
        # reports ALIVE, because the SKU ID that picks the right block out
        # of it comes from that response.
        "iwlwifi-so-a0-gf-a0.pnvm|776606c7b5d0f582b5ec78a591fb1e5e621a6efa670e8261f29114bbf1b0f8d1"
        "iwlwifi-ty-a0-gf-a0.pnvm|cc4c0de4593292490459f1aa8c1606ea370a0307f5be9089ef99a0aa8e36be7a"
        "iwlwifi-gl-c0-fm-c0.pnvm|f481e3b0403ca43a0a444b01c04fa4d1582868afcb026b1e697905f0039ccfa3"
        "iwlwifi-bz-b0-fm-c0.pnvm|2da92f02418fbde8d8210eb582d9697faa222f8ee2da229e29b8a108ea5c4c5c")

    file(MAKE_DIRECTORY "${_iwlwifi_dir}")

    foreach(_iwlwifi_entry IN LISTS _iwlwifi_blobs)
        string(REGEX MATCH "^[^|]+"  _iwlwifi_name "${_iwlwifi_entry}")
        string(REGEX MATCH "[^|]+$"  _iwlwifi_sha  "${_iwlwifi_entry}")
        set(_iwlwifi_path "${_iwlwifi_dir}/${_iwlwifi_name}")

        if(NOT EXISTS "${_iwlwifi_path}")
            message(STATUS "Downloading Intel Wireless firmware ${_iwlwifi_name}")
            file(DOWNLOAD
                "${_iwlwifi_base}/${_iwlwifi_name}"
                "${_iwlwifi_path}"
                EXPECTED_HASH SHA256=${_iwlwifi_sha}
                TLS_VERIFY ON
                SHOW_PROGRESS)
        else()
            file(SHA256 "${_iwlwifi_path}" _iwlwifi_cached_sha)
            if(NOT _iwlwifi_cached_sha STREQUAL "${_iwlwifi_sha}")
                message(FATAL_ERROR
                    "Cached ${_iwlwifi_name} has an unexpected SHA-256. "
                    "Delete ${_iwlwifi_path} and re-configure.")
            endif()
        endif()

        # Into reactos.cab, so the blob reaches an installed system and not
        # just the boot medium.  The driver looks for it in
        # \SystemRoot\System32\drivers\iwlwifi\ - see IWL_FW_DIRECTORY in
        # drivers/network/dd/iwlwifi/fw/ucode_load.c.
        add_cd_file(FILE "${_iwlwifi_path}"
                    DESTINATION "reactos/system32/drivers/iwlwifi"
                    FOR all)
    endforeach()

    if(NOT ENABLE_IWLWIFI)
        message(STATUS
            "DOWNLOAD_IWLWIFI_UCODE is on but ENABLE_IWLWIFI is off: "
            "the firmware will be packaged with no driver to consume it.")
    endif()
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

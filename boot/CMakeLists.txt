
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    set(FREELDR_HAS_BIOS_BOOT TRUE)
else()
    set(FREELDR_HAS_BIOS_BOOT FALSE)
endif()

include(boot_images.cmake)

add_subdirectory(bootdata)
add_subdirectory(freeldr)
add_subdirectory(rtl)

add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.cmd DESTINATION loader NO_CAB FOR bootcd)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.sh DESTINATION loader NO_CAB FOR bootcd)

# WoW64 guest file set (32-bit system binaries for 64-bit hosts).
#
# WoW64 inherently needs a second, 32-bit world that a single (one-ARCH) build
# tree cannot produce, so the i386/SysWOW64 binaries come from a separate
# ARCH=i386 SARCH=wow64 build. Point REACTOS_WOW64_GUEST_DIR at that build's
# top directory: we read its generated LiveCD file list and lay the user-mode
# binaries out under reactos/SysWOW64 (plus the x86 winsxs assemblies)
# ourselves -- no external staging step is required. Kernel-mode and
# boot-environment modules are filtered out, since SysWOW64 only hosts the
# 32-bit user-mode system image. As a fallback, a directory that already
# mirrors the CD layout below reactos/ is grafted verbatim.
#
# The files are added to the LiveCD file set, which the BootCD image inherits,
# so 32-bit programs can run on the live system.
if(ARCH STREQUAL "amd64" AND REACTOS_WOW64_GUEST_DIR)
    file(GLOB _wow64_lst "${REACTOS_WOW64_GUEST_DIR}/boot/livecd.*.lst")
    if(_wow64_lst)
        # A guest build directory: derive SysWOW64 from its LiveCD file list
        # (each line is "<cd-destination>=<source-path>").
        list(GET _wow64_lst 0 _wow64_lst)
        file(STRINGS "${_wow64_lst}" _wow64_lines)
        foreach(_wow64_line ${_wow64_lines})
            string(FIND "${_wow64_line}" "=" _wow64_eq)
            if(_wow64_eq LESS 1)
                continue()
            endif()
            string(SUBSTRING "${_wow64_line}" 0 ${_wow64_eq} _wow64_cddest)
            math(EXPR _wow64_srcpos "${_wow64_eq} + 1")
            string(SUBSTRING "${_wow64_line}" ${_wow64_srcpos} -1 _wow64_src)
            if(_wow64_cddest MATCHES "^reactos/system32/([^/]+)$")
                # Direct children of system32 become SysWOW64 entries; nested
                # paths (config, drivers, wbem, ...) are not part of the
                # 32-bit user image and are skipped.
                string(TOLOWER "${CMAKE_MATCH_1}" _wow64_name)
                if(_wow64_name MATCHES "\\.sys$" OR
                   _wow64_name MATCHES "^ntoskrnl\\.exe$" OR
                   _wow64_name MATCHES "^ntkrnl.*\\.exe$" OR
                   _wow64_name MATCHES "^hal.*\\.dll$" OR
                   _wow64_name MATCHES "^kd.*\\.dll$" OR
                   _wow64_name STREQUAL "bootvid.dll" OR
                   _wow64_name STREQUAL "drvmain.sdb")
                    continue()
                endif()
                add_cd_file(FILE "${_wow64_src}" DESTINATION reactos/SysWOW64 FOR livecd)
            elseif(_wow64_cddest MATCHES "^reactos/winsxs/(.+)/[^/]+$")
                add_cd_file(FILE "${_wow64_src}"
                            DESTINATION "reactos/winsxs/${CMAKE_MATCH_1}" FOR livecd)
            elseif(_wow64_cddest MATCHES "^reactos/winsxs/[^/]+$")
                add_cd_file(FILE "${_wow64_src}" DESTINATION reactos/winsxs FOR livecd)
            endif()
        endforeach()
    else()
        # Pre-staged overlay tree mirroring the CD layout below reactos/.
        file(GLOB_RECURSE _wow64_guest_files
             RELATIVE "${REACTOS_WOW64_GUEST_DIR}"
             "${REACTOS_WOW64_GUEST_DIR}/*")
        foreach(_wow64_rel ${_wow64_guest_files})
            get_filename_component(_wow64_subdir "${_wow64_rel}" DIRECTORY)
            if(_wow64_subdir)
                set(_wow64_dest "reactos/${_wow64_subdir}")
            else()
                set(_wow64_dest "reactos")
            endif()
            add_cd_file(FILE "${REACTOS_WOW64_GUEST_DIR}/${_wow64_rel}"
                        DESTINATION "${_wow64_dest}" FOR livecd)
        endforeach()
    endif()
endif()

if(REACTOS_LUAGENT_DIR)
    set(_luagent_bin_dir "${REACTOS_LUAGENT_DIR}")
    if(EXISTS "${REACTOS_LUAGENT_DIR}/build-wine/luagent.exe")
        set(_luagent_bin_dir "${REACTOS_LUAGENT_DIR}/build-wine")
    endif()

    foreach(_luagent_exe hello.exe luagent.exe)
        if(EXISTS "${_luagent_bin_dir}/${_luagent_exe}")
            add_cd_file(FILE "${_luagent_bin_dir}/${_luagent_exe}"
                        DESTINATION reactos/system32 FOR bootcd)
        elseif(EXISTS "${REACTOS_LUAGENT_DIR}/${_luagent_exe}")
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/${_luagent_exe}"
                        DESTINATION reactos/system32 FOR bootcd)
        endif()
    endforeach()
    foreach(_luagent_dll lua54.dll lua55.dll libuv-1.dll)
        if(EXISTS "${_luagent_bin_dir}/${_luagent_dll}")
            add_cd_file(FILE "${_luagent_bin_dir}/${_luagent_dll}"
                        DESTINATION reactos/system32 FOR bootcd)
        elseif(EXISTS "${REACTOS_LUAGENT_DIR}/${_luagent_dll}")
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/${_luagent_dll}"
                        DESTINATION reactos/system32 FOR bootcd)
        endif()
    endforeach()
    if(IS_DIRECTORY "${REACTOS_LUAGENT_DIR}/lua")
        file(GLOB_RECURSE _luagent_lua_files
             RELATIVE "${REACTOS_LUAGENT_DIR}/lua"
             "${REACTOS_LUAGENT_DIR}/lua/*")
        foreach(_lua_rel ${_luagent_lua_files})
            get_filename_component(_lua_subdir "${_lua_rel}" DIRECTORY)
            if(_lua_subdir)
                set(_lua_dest "reactos/system32/lua/${_lua_subdir}")
            else()
                set(_lua_dest "reactos/system32/lua")
            endif()
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/lua/${_lua_rel}"
                        DESTINATION "${_lua_dest}" FOR bootcd)
        endforeach()
    endif()
endif()

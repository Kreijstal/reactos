
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
# so 32-bit programs can run on the live system. They are also added to the
# BootCD set so they end up inside reactos.cab and the text-mode installer
# deploys them to the installed system's SysWOW64 (and x86 winsxs) directory;
# without this the installer would create an empty SysWOW64 and no WoW64
# process could launch from a clean install. The reactos/SysWOW64 and x86
# winsxs destinations map to cab directory IDs in sdk/cmake/CMakeMacros.cmake
# (dir_to_num) and boot/bootdata/packages/reactos.dff.in ([Directories]).
if(ARCH STREQUAL "amd64" AND REACTOS_WOW64_GUEST_DIR)
    file(GLOB _wow64_lst "${REACTOS_WOW64_GUEST_DIR}/boot/livecd.*.lst")
    if(_wow64_lst)
        # A guest build directory: derive the 32-bit user image from its LiveCD
        # file list (each line is "<cd-destination>=<source-path>").
        list(GET _wow64_lst 0 _wow64_lst)
        file(STRINGS "${_wow64_lst}" _wow64_lines)

        # The text-mode installer copies cabinet contents through a
        # filename-keyed INF, so the 32-bit guest binaries (whose names collide
        # with their 64-bit system32 counterparts: ntdll.dll, kernel32.dll, ...)
        # cannot share the flat reactos.cab. They are packed into a dedicated
        # wow64.cab whose own INF namespace maps them to SysWOW64 and the x86
        # winsxs assemblies, with no collision against reactos.cab. The same
        # files are also laid out as a loose tree on the LiveCD (FOR livecd),
        # where WoW64 runs directly off the CD media.
        set(_wow64_dff_files "")            # accumulated "<src>" <dirnum> lines
        set(_wow64_dep_files "")            # source files the cab depends on
        set(_wow64_dir_defs "82=SysWOW64")  # <num>=<inf-relative-path> entries
        set(_wow64_dir_paths "SysWOW64")    # parallel: known inf-relative paths
        set(_wow64_dir_nums "82")           # parallel: their dir numbers
        set(_wow64_next_num 100)            # next free winsxs dir number

        foreach(_wow64_line ${_wow64_lines})
            string(FIND "${_wow64_line}" "=" _wow64_eq)
            if(_wow64_eq LESS 1)
                continue()
            endif()
            string(SUBSTRING "${_wow64_line}" 0 ${_wow64_eq} _wow64_cddest)
            math(EXPR _wow64_srcpos "${_wow64_eq} + 1")
            string(SUBSTRING "${_wow64_line}" ${_wow64_srcpos} -1 _wow64_src)

            set(_wow64_cd_dest "")
            set(_wow64_relpath "")
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
                set(_wow64_cd_dest "reactos/SysWOW64")
                set(_wow64_relpath "SysWOW64")
            elseif(_wow64_cddest MATCHES "^reactos/winsxs/(.+)/[^/]+$")
                set(_wow64_cd_dest "reactos/winsxs/${CMAKE_MATCH_1}")
                string(REPLACE "/" "\\" _wow64_relpath "winsxs/${CMAKE_MATCH_1}")
            elseif(_wow64_cddest MATCHES "^reactos/winsxs/[^/]+$")
                set(_wow64_cd_dest "reactos/winsxs")
                set(_wow64_relpath "winsxs")
            else()
                continue()
            endif()

            # LiveCD: loose directory tree (WoW64 runs off the CD directly).
            add_cd_file(FILE "${_wow64_src}" DESTINATION "${_wow64_cd_dest}" FOR livecd)

            # Installer cabinet: resolve (or assign) the wow64.inf dir number
            # for this destination, then queue the file under it.
            list(FIND _wow64_dir_paths "${_wow64_relpath}" _wow64_idx)
            if(_wow64_idx EQUAL -1)
                set(_wow64_dir_num ${_wow64_next_num})
                list(APPEND _wow64_dir_paths "${_wow64_relpath}")
                list(APPEND _wow64_dir_nums ${_wow64_next_num})
                list(APPEND _wow64_dir_defs "${_wow64_next_num}=${_wow64_relpath}")
                math(EXPR _wow64_next_num "${_wow64_next_num} + 1")
            else()
                list(GET _wow64_dir_nums ${_wow64_idx} _wow64_dir_num)
            endif()

            string(APPEND _wow64_dff_files "\"${_wow64_src}\" ${_wow64_dir_num}\n")
            list(APPEND _wow64_dep_files "${_wow64_src}")
        endforeach()

        if(_wow64_dff_files)
            # Emit the wow64.cab directive file: header + [Directories] derived
            # from the destinations seen above + an (initially empty)
            # [SourceFiles] section that native-cabman fills as it adds files.
            set(_wow64_dir_section "")
            foreach(_def ${_wow64_dir_defs})
                string(REPLACE "=" " = " _def_line "${_def}")
                string(APPEND _wow64_dir_section "${_def_line}\n")
            endforeach()

            set(_wow64_dff "${CMAKE_CURRENT_BINARY_DIR}/wow64.dff")
            file(WRITE "${_wow64_dff}"
                 ".Set DiskLabelTemplate=\"ReactOS\"\n"
                 ".Set CabinetNameTemplate=\"wow64.cab\"\n"
                 ".Set InfFileName=\"wow64.inf\"\n"
                 "\n"
                 ".InfBegin\n"
                 "[Version]\n"
                 "Signature = \"$Windows NT$\"\n"
                 "\n"
                 "[Directories]\n"
                 "${_wow64_dir_section}"
                 ".InfEnd\n"
                 "\n"
                 ".InfBegin\n"
                 "[SourceFiles]\n"
                 ".InfEnd\n"
                 "\n"
                 "${_wow64_dff_files}"
                 "\n")

            # Build wow64.cab and (without -N) its companion loose wow64.inf,
            # both consumed by the text-mode installer's [Cabinets] handling.
            add_custom_command(
                OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wow64.cab
                       ${CMAKE_CURRENT_BINARY_DIR}/wow64.inf
                COMMAND native-cabman -C ${_wow64_dff} -P ${REACTOS_SOURCE_DIR}
                DEPENDS native-cabman ${_wow64_dff} ${_wow64_dep_files})
            add_custom_target(wow64_cab
                DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wow64.cab
                        ${CMAKE_CURRENT_BINARY_DIR}/wow64.inf)

            # Ship both onto the BootCD/RegTest media next to reactos.cab; the
            # installer locates wow64.inf by replacing the cab's extension.
            add_cd_file(TARGET wow64_cab
                FILE ${CMAKE_CURRENT_BINARY_DIR}/wow64.cab
                DESTINATION reactos NO_CAB FOR bootcd regtest)
            add_cd_file(TARGET wow64_cab
                FILE ${CMAKE_CURRENT_BINARY_DIR}/wow64.inf
                DESTINATION reactos NO_CAB FOR bootcd regtest)
        endif()
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
                        DESTINATION reactos/system32 FOR all)
        elseif(EXISTS "${REACTOS_LUAGENT_DIR}/${_luagent_exe}")
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/${_luagent_exe}"
                        DESTINATION reactos/system32 FOR all)
        endif()
    endforeach()
    foreach(_luagent_dll lua54.dll lua55.dll libuv-1.dll)
        if(EXISTS "${_luagent_bin_dir}/${_luagent_dll}")
            add_cd_file(FILE "${_luagent_bin_dir}/${_luagent_dll}"
                        DESTINATION reactos/system32 FOR all)
        elseif(EXISTS "${REACTOS_LUAGENT_DIR}/${_luagent_dll}")
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/${_luagent_dll}"
                        DESTINATION reactos/system32 FOR all)
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
                        DESTINATION "${_lua_dest}" FOR all)
        endforeach()
    endif()
endif()

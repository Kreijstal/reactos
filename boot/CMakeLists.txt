
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
# Point REACTOS_WOW64_GUEST_DIR at a staged directory tree that mirrors the
# CD layout below the reactos/ directory -- typically SysWOW64/* plus the
# x86 winsxs assemblies -- collected from a separate ARCH=i386 SARCH=wow64
# build. The files are grafted into the LiveCD file set, which the BootCD
# image inherits, so 32-bit programs can run on the live system.
if(ARCH STREQUAL "amd64" AND REACTOS_WOW64_GUEST_DIR)
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

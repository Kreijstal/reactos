
include(boot_images.cmake)

add_subdirectory(bootdata)
add_subdirectory(freeldr)
add_subdirectory(rtl)

add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.cmd DESTINATION loader NO_CAB FOR bootcd)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.sh DESTINATION loader NO_CAB FOR bootcd)

if(REACTOS_LUAGENT_DIR)
    set(_luagent_bin_dir "${REACTOS_LUAGENT_DIR}")
    if(EXISTS "${REACTOS_LUAGENT_DIR}/build-wine/luagent.exe")
        set(_luagent_bin_dir "${REACTOS_LUAGENT_DIR}/build-wine")
    endif()

    foreach(_luagent_exe hello.exe luagent.exe)
        if(EXISTS "${_luagent_bin_dir}/${_luagent_exe}")
            add_cd_file(FILE "${_luagent_bin_dir}/${_luagent_exe}"
                        DESTINATION reactos/system32 NO_CAB FOR bootcd livecd hybridcd)
        elseif(EXISTS "${REACTOS_LUAGENT_DIR}/${_luagent_exe}")
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/${_luagent_exe}"
                        DESTINATION reactos/system32 NO_CAB FOR bootcd livecd hybridcd)
        endif()
    endforeach()
    foreach(_luagent_dll lua54.dll lua55.dll libuv-1.dll)
        if(EXISTS "${_luagent_bin_dir}/${_luagent_dll}")
            add_cd_file(FILE "${_luagent_bin_dir}/${_luagent_dll}"
                        DESTINATION reactos/system32 NO_CAB FOR bootcd livecd hybridcd)
        elseif(EXISTS "${REACTOS_LUAGENT_DIR}/${_luagent_dll}")
            add_cd_file(FILE "${REACTOS_LUAGENT_DIR}/${_luagent_dll}"
                        DESTINATION reactos/system32 NO_CAB FOR bootcd livecd hybridcd)
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
                        DESTINATION "${_lua_dest}" NO_CAB FOR bootcd livecd hybridcd)
        endforeach()
    endif()
endif()

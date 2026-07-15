

# wlansvc is a Vista-era service: it consumes Vista+ APIs (CancelIoEx via
# kernel32_vista on downlevel export configurations)
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT} -DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl)
add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlansvc.idl)

list(APPEND SOURCE
    wlansvc.c
    rpcserver.c
    core.c
    scan.c
    connect.c
    profile.c
    notify.c
    nwifi_ioctl.c
    precomp.h
    wlansvc.h
    ${CMAKE_CURRENT_BINARY_DIR}/wlansvc_s.c)

add_executable(wlansvc ${SOURCE} wlansvc.rc)

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(wlansvc PRIVATE "-Wno-cast-calling-convention")
endif()

if(NOT MSVC)
    target_link_libraries(wlansvc ${PSEH_LIB})
endif()

set_module_type(wlansvc win32cui UNICODE)
add_importlibs(wlansvc advapi32 rpcrt4 iphlpapi kernel32_vista msvcrt kernel32 ntdll)
add_pch(wlansvc precomp.h SOURCE)
add_cd_file(TARGET wlansvc DESTINATION reactos/system32 FOR all)

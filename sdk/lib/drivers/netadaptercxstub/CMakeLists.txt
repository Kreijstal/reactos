
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT})

# One stub per NetAdapterCx version. A client links the one it targets and
# inherits the matching NETADAPTER_VERSION_* definitions and include paths.
function(add_netadaptercx_stub _minor)
    set(_target netadaptercxstub_2_${_minor})
    add_library(${_target} STATIC netadaptercxstub.c)
    add_dependencies(${_target} xdk)

    target_compile_definitions(${_target}
        PUBLIC NETADAPTER_VERSION_MAJOR=2 NETADAPTER_VERSION_MINOR=${_minor}
        PRIVATE WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

    target_include_directories(${_target}
        PUBLIC ${REACTOS_SOURCE_DIR}/sdk/include/netcx/kmdf/adapter
               ${REACTOS_SOURCE_DIR}/sdk/include/netcx/shared/1.0
        PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/wdf/kmdf/1.17)
endfunction()

add_netadaptercx_stub(0)
add_netadaptercx_stub(1)

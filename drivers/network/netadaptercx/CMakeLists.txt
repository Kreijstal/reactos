
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT})

list(APPEND SOURCE
    adapter.c
    device.c
    driver.c
    miniport.c
    queue.c
    stubs.c
    nxapi.h
    nxprivate.h)

add_library(netadaptercx MODULE ${SOURCE} netadaptercx.rc)
set_module_type(netadaptercx kmdfdriver)

# The class extension implements NetAdapterCx 2.1, which includes 2.0
target_compile_definitions(netadaptercx PRIVATE
    NETADAPTER_VERSION_MAJOR=2
    NETADAPTER_VERSION_MINOR=1
    NDIS620_MINIPORT
    NDIS_MINIPORT_DRIVER
    NDIS_LEGACY_DRIVER=1
    NDIS_LEGACY_MINIPORT=1
    WDF_DEVICE_NO_WDMSEC_H
    _WIN32_WINNT=0x603
    NTDDI_VERSION=0x06030000)

target_include_directories(netadaptercx PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/include/netcx/kmdf/adapter
    ${REACTOS_SOURCE_DIR}/sdk/include/netcx/shared/1.0
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)

target_link_libraries(netadaptercx ntoskrnl_vista libcntpr)
add_importlibs(netadaptercx ndis ntoskrnl hal)
add_cd_file(TARGET netadaptercx DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(netadaptercx_reg.inf)

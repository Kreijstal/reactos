
include_directories(../include)

list(APPEND NETIO_WSK_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    NetioWsk_drv.c)

add_library(netiowsk_drv MODULE ${NETIO_WSK_DRV_SOURCE})
set_module_type(netiowsk_drv kernelmodedriver)
target_link_libraries(netiowsk_drv kmtest_printf uuid ${PSEH_LIB})
add_importlibs(netiowsk_drv netio ntoskrnl hal)
target_compile_definitions(netiowsk_drv PRIVATE KMT_STANDALONE_DRIVER)
# The test exercises the Windows 8 WSK dispatch table, including WskSendMessages.
target_compile_options(netiowsk_drv PRIVATE
    -U_WIN32_WINNT -D_WIN32_WINNT=0x0602
    -UNTDDI_VERSION -DNTDDI_VERSION=0x06020000)
add_rostests_file(TARGET netiowsk_drv)

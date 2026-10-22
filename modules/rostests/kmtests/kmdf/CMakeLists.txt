
include_directories(../include)

#
# Test class extension, loaded by WDFLDR when kmdf133_drv binds to it
#
add_library(kmdfcx_drv MODULE KmtCx_drv.c)
set_module_type(kmdfcx_drv kmdfdriver)
target_include_directories(kmdfcx_drv PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)
target_link_libraries(kmdfcx_drv ntoskrnl_vista)
add_importlibs(kmdfcx_drv ntoskrnl hal)
add_rostests_file(TARGET kmdfcx_drv)

#
# KMDF 1.33 client
#
list(APPEND KMDF133_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    Kmdf133_drv.c)

add_library(kmdf133_drv MODULE ${KMDF133_DRV_SOURCE})
set_module_type(kmdf133_drv kmdfdriver)
target_include_directories(kmdf133_drv PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)
target_link_libraries(kmdf133_drv kmtest_printf memcmp ${PSEH_LIB})
add_importlibs(kmdf133_drv ntoskrnl hal)
target_compile_definitions(kmdf133_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET kmdf133_drv)

#
# UCX client, binds to ucx01000
#
list(APPEND KMDFUCX_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    KmdfUcx_drv.c)

add_library(kmdfucx_drv MODULE ${KMDFUCX_DRV_SOURCE})
set_module_type(kmdfucx_drv kmdfdriver)
target_include_directories(kmdfucx_drv PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/include/ucx/1.6
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/shared/inc/private/common
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/wdf/kmdf/inc/private)
target_link_libraries(kmdfucx_drv kmtest_printf memcmp ${PSEH_LIB})
add_importlibs(kmdfucx_drv ntoskrnl hal)
target_compile_definitions(kmdfucx_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET kmdfucx_drv)

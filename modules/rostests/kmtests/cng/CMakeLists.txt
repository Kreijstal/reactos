
include_directories(../include)

list(APPEND CNG_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    Cng_drv.c)

add_library(cng_drv MODULE ${CNG_DRV_SOURCE})
set_module_type(cng_drv kernelmodedriver)
target_link_libraries(cng_drv kmtest_printf memcmp ${PSEH_LIB})
add_importlibs(cng_drv cng ntoskrnl hal)
target_compile_definitions(cng_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET cng_drv)

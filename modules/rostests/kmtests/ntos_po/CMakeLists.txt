
include_directories(../include)

#
# PoIrp
#
list(APPEND POIRP_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    PoIrp_drv.c)

add_library(poirp_drv MODULE ${POIRP_DRV_SOURCE})
set_module_type(poirp_drv kernelmodedriver)
target_link_libraries(poirp_drv kmtest_printf ${PSEH_LIB})
add_importlibs(poirp_drv ntoskrnl hal)
target_compile_definitions(poirp_drv PRIVATE KMT_STANDALONE_DRIVER)
#add_pch(poirp_drv ../include/kmt_test.h)
add_rostests_file(TARGET poirp_drv)

#
# PoIdle
#
list(APPEND POIDLE_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    PoIdle_drv.c)

add_library(poidle_drv MODULE ${POIDLE_DRV_SOURCE})
set_module_type(poidle_drv kernelmodedriver)
target_link_libraries(poidle_drv kmtest_printf ${PSEH_LIB})
add_importlibs(poidle_drv ntoskrnl hal)
target_compile_definitions(poidle_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET poidle_drv)

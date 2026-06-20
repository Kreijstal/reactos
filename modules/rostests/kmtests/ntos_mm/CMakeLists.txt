
include_directories(../include)

#
# NtCreateSection
#
list(APPEND NTCREATESECTION_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    NtCreateSection_drv.c)

add_library(ntcreatesection_drv MODULE ${NTCREATESECTION_DRV_SOURCE})
set_module_type(ntcreatesection_drv kernelmodedriver)
target_link_libraries(ntcreatesection_drv kmtest_printf ${PSEH_LIB})
add_importlibs(ntcreatesection_drv ntoskrnl hal)
target_compile_definitions(ntcreatesection_drv PRIVATE KMT_STANDALONE_DRIVER)
#add_pch(ntcreatesection_drv ../include/kmt_test.h)
add_rostests_file(TARGET ntcreatesection_drv)

#
# MmMapLockedPagesSpecifyCache
#
list(APPEND MMMAPLOCKEDPAGESSPECIFYCACHE_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    MmMapLockedPagesSpecifyCache_drv.c)

add_library(mmmaplockedpagesspecifycache_drv MODULE ${MMMAPLOCKEDPAGESSPECIFYCACHE_DRV_SOURCE})
set_module_type(mmmaplockedpagesspecifycache_drv kernelmodedriver)
target_link_libraries(mmmaplockedpagesspecifycache_drv kmtest_printf ${PSEH_LIB})
add_importlibs(mmmaplockedpagesspecifycache_drv ntoskrnl hal)
target_compile_definitions(mmmaplockedpagesspecifycache_drv PRIVATE KMT_STANDALONE_DRIVER)
#add_pch(mmmaplockedpagesspecifycache_drv ../include/kmt_test.h)
add_rostests_file(TARGET mmmaplockedpagesspecifycache_drv)

#
# MmPageoutSysView
#
list(APPEND MMPAGEOUTSYSVIEW_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    MmPageoutSysView_drv.c)

add_library(mmpageoutsysview_drv MODULE ${MMPAGEOUTSYSVIEW_DRV_SOURCE})
set_module_type(mmpageoutsysview_drv kernelmodedriver)
target_link_libraries(mmpageoutsysview_drv kmtest_printf ${PSEH_LIB})
add_importlibs(mmpageoutsysview_drv ntoskrnl hal)
target_compile_definitions(mmpageoutsysview_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET mmpageoutsysview_drv)

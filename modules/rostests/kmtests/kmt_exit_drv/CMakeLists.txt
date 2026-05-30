
add_library(kmt_exit_drv MODULE kmt_exit_drv.c)
set_module_type(kmt_exit_drv kernelmodedriver)
add_importlibs(kmt_exit_drv ntoskrnl hal)
add_rostests_file(TARGET kmt_exit_drv)

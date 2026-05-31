
include_directories(
    ${REACTOS_SOURCE_DIR}/modules/rostests/kmtests/include
    ${REACTOS_SOURCE_DIR}/modules/rostests/kmtests/kmt_exit_drv)

add_executable(kmtestrunner kmtestrunner.c kmtestrunner.rc)
set_module_type(kmtestrunner nativecui)
target_link_libraries(kmtestrunner nt chkstk)
add_importlibs(kmtestrunner ntdll)
add_cd_file(TARGET kmtestrunner DESTINATION reactos/system32 FOR kmtestcd)

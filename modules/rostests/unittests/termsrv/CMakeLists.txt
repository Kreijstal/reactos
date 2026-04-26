include_directories(
    ${REACTOS_SOURCE_DIR}/base/services/termsrv
    ${REACTOS_SOURCE_DIR}/modules/rostests/apitests/include)

list(APPEND SOURCE
    rdppeer.c
    testlist.c)

add_executable(termsrv_rdp_unittest ${SOURCE})
set_module_type(termsrv_rdp_unittest win32cui)
target_link_libraries(termsrv_rdp_unittest termsrv_rdp)
add_importlibs(termsrv_rdp_unittest user32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET termsrv_rdp_unittest)

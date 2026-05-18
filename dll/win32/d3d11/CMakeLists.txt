
spec2def(d3d11.dll d3d11.spec)

add_library(d3d11 MODULE
    d3d11_main.c
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/d3d11_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/d3d11.def)

set_module_type(d3d11 win32dll UNICODE)
target_link_libraries(d3d11 wine)
add_importlibs(d3d11 msvcrt kernel32 ntdll)
add_cd_file(TARGET d3d11 DESTINATION reactos/system32 FOR all)


spec2def(dxgi.dll dxgi.spec)

add_library(dxgi MODULE
    dxgi_main.c
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dxgi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/dxgi.def)

set_module_type(dxgi win32dll UNICODE)
target_link_libraries(dxgi wine)
add_importlibs(dxgi msvcrt kernel32 ntdll)
add_cd_file(TARGET dxgi DESTINATION reactos/system32 FOR all)

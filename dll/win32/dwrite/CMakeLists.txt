
spec2def(dwrite.dll dwrite.spec)

list(APPEND SOURCE
    dwrite_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/dwrite_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/dwrite.def)

add_library(dwrite MODULE ${SOURCE} version.rc)
set_module_type(dwrite win32dll UNICODE)
target_link_libraries(dwrite wine)
add_importlibs(dwrite msvcrt kernel32 ntdll)
add_cd_file(TARGET dwrite DESTINATION reactos/system32 FOR all)

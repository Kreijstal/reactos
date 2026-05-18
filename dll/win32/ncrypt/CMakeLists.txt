
spec2def(ncrypt.dll ncrypt.spec)

list(APPEND SOURCE
    ncrypt_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/ncrypt_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/ncrypt.def)

add_library(ncrypt MODULE ${SOURCE} version.rc)
set_module_type(ncrypt win32dll UNICODE)
target_link_libraries(ncrypt wine)
add_importlibs(ncrypt msvcrt kernel32 ntdll)
add_cd_file(TARGET ncrypt DESTINATION reactos/system32 FOR all)

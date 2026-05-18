
add_definitions(-D_SHCORE_)
spec2def(shcore.dll shcore.spec)

list(APPEND SOURCE
    shcore_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/shcore_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/shcore.def)

add_library(shcore MODULE ${SOURCE} version.rc)
set_module_type(shcore win32dll UNICODE)
target_link_libraries(shcore wine)
add_importlibs(shcore shell32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET shcore DESTINATION reactos/system32 FOR all)

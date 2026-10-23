
spec2def(fwpuclnt.dll fwpuclnt.spec)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/fwpuclnt_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/fwpuclnt.def)

add_library(fwpuclnt MODULE ${SOURCE})
set_module_type(fwpuclnt win32dll)
target_link_libraries(fwpuclnt wine)
add_importlibs(fwpuclnt msvcrt kernel32 ntdll)
add_cd_file(TARGET fwpuclnt DESTINATION reactos/system32 FOR all)
set_wine_module(fwpuclnt)

spec2def(wow64cpu.dll def/wow64cpu.spec ADD_IMPORTLIB)

add_library(wow64cpu MODULE
    wow64cpu.c
    ${CMAKE_CURRENT_BINARY_DIR}/wow64cpu.def
    ${CMAKE_CURRENT_BINARY_DIR}/wow64cpu_stubs.c)

set_module_type(wow64cpu win32dll UNICODE ENTRYPOINT DllMain 12)
set_subsystem(wow64cpu console)
set_image_base(wow64cpu 0xFE000000)
add_importlibs(wow64cpu ntdll)

add_cd_file(TARGET wow64cpu DESTINATION reactos/system32 FOR all)

spec2def(bthprops.cpl bthprops.cpl.spec)

file(GLOB bthprops_rc_deps resources/*.*)
add_rc_deps(bthprops.rc ${bthprops_rc_deps})

list(APPEND SOURCE
    bthprops.c
    resource.h)

add_library(bthprops MODULE
    ${SOURCE}
    bthprops.rc
    ${CMAKE_CURRENT_BINARY_DIR}/bthprops_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/bthprops.def)

set_module_type(bthprops cpl UNICODE)
target_link_libraries(bthprops wine wine_dll_canunload)
add_importlibs(bthprops bluetoothapis comctl32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET bthprops DESTINATION reactos/system32 FOR all)
set_wine_module(bthprops)

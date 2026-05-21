spec2def(netprofm.dll netprofm.spec)

list(APPEND SOURCE
    list.c
    main.c
    precomp.h)

add_library(netprofm MODULE
    ${SOURCE}
    netprofm.rc
    ${CMAKE_CURRENT_BINARY_DIR}/netprofm.def)

set_source_files_properties(netprofm.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/netprofm.rgs")

set_module_type(netprofm win32dll)
target_link_libraries(netprofm wine uuid)
add_importlibs(netprofm iphlpapi ole32 oleaut32 advapi32 user32 msvcrt kernel32 ntdll)
add_pch(netprofm precomp.h SOURCE)
add_cd_file(TARGET netprofm DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(netprofm) # CORE-5743: No ARRAY_SIZE and CONST_VTABLE

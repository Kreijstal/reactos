
spec2def(nci.dll nci.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    nci.c
    ${CMAKE_CURRENT_BINARY_DIR}/nci_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/nci.def)

add_library(nci MODULE ${SOURCE} nci.rc)
set_module_type(nci win32dll UNICODE)
add_importlibs(nci advapi32 ole32 msvcrt kernel32 ntdll)
add_cd_file(TARGET nci DESTINATION reactos/system32 FOR all)


list(APPEND SOURCE
    NciConnectionName.c
    testlist.c)

add_executable(nci_apitest ${SOURCE})
set_module_type(nci_apitest win32cui)
add_importlibs(nci_apitest iphlpapi advapi32 ole32 user32 msvcrt kernel32)
add_rostests_file(TARGET nci_apitest)

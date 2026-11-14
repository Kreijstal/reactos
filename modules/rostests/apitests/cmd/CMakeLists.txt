
list(APPEND SOURCE
    attrib_link.c
    cmd.c
    fc.c
    robocopy.c
    testlist.c)

add_executable(cmd_apitest ${SOURCE})
target_link_libraries(cmd_apitest wine ${PSEH_LIB})
set_module_type(cmd_apitest win32cui)
add_importlibs(cmd_apitest shlwapi msvcrt kernel32 ntdll)
add_rostests_file(TARGET cmd_apitest)

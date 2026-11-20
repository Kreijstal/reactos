
list(APPEND SOURCE
    InitializeSecurityContext.c
    testlist.c)

add_executable(schannel_apitest ${SOURCE})
target_link_libraries(schannel_apitest wine ${PSEH_LIB})
set_module_type(schannel_apitest win32cui)
add_importlibs(schannel_apitest secur32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET schannel_apitest)

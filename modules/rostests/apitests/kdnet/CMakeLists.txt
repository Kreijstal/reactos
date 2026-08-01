add_executable(kdnet_apitest
    protocol.c
    testlist.c)

target_link_libraries(kdnet_apitest kdnetprotocol wine ${PSEH_LIB})
set_module_type(kdnet_apitest win32cui)
add_importlibs(kdnet_apitest msvcrt kernel32 ntdll)
add_rostests_file(TARGET kdnet_apitest)


add_executable(wlanapi_apitest acmfallback.c devrestart.c profileedit.c profileorder.c profilestore.c svclife.c wlanapi.c testlist.c)
set_module_type(wlanapi_apitest win32cui)
add_importlibs(wlanapi_apitest wlanapi setupapi iphlpapi advapi32 msvcrt kernel32)
add_rostests_file(TARGET wlanapi_apitest)

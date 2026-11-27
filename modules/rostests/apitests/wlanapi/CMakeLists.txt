
add_executable(wlanapi_apitest acmfallback.c profileorder.c profilestore.c wlanapi.c testlist.c)
set_module_type(wlanapi_apitest win32cui)
add_importlibs(wlanapi_apitest wlanapi advapi32 msvcrt kernel32)
add_rostests_file(TARGET wlanapi_apitest)

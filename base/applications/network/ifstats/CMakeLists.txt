add_executable(ifstats ifstats.c)
set_module_type(ifstats win32cui UNICODE)
add_importlibs(ifstats iphlpapi msvcrt kernel32)
add_cd_file(TARGET ifstats DESTINATION reactos/system32 FOR all)

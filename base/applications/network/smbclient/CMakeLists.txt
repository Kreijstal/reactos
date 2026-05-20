
add_executable(smbclient smbclient.c smbclient.rc)
set_module_type(smbclient win32cui)
target_link_libraries(smbclient libsmb2)
add_importlibs(smbclient ws2_32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET smbclient DESTINATION reactos/system32 FOR all)

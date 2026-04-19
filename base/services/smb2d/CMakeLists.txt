
list(APPEND SOURCE
    service.c
    daemon.c
    smb2d.h)

add_executable(smb2d ${SOURCE})

set_module_type(smb2d win32cui)
target_link_libraries(smb2d libsmb2)
add_importlibs(smb2d ws2_32 advapi32 kernel32 msvcrt ntdll)
add_cd_file(TARGET smb2d DESTINATION reactos/system32 FOR all)
add_registry_inf(smb2d_reg.inf)

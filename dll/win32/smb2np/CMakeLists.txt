
spec2def(smb2np.dll smb2np.spec)

list(APPEND SOURCE
    smb2np.c
    smb2np.rc
    ${CMAKE_CURRENT_BINARY_DIR}/smb2np_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/smb2np.def)

add_library(smb2np MODULE ${SOURCE})
set_module_type(smb2np win32dll UNICODE)
target_link_libraries(smb2np libsmb2 wine)
add_importlibs(smb2np ws2_32 advapi32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET smb2np DESTINATION reactos/system32 FOR all)

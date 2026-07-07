
add_executable(wlanui wlanui.c wlanui.rc)
set_module_type(wlanui win32gui UNICODE)
add_importlibs(wlanui wlanapi comctl32 user32 gdi32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET wlanui DESTINATION reactos/system32 FOR all)

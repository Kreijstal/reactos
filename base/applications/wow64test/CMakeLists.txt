
list(APPEND SOURCE
    test.c)

add_executable(wow64test ${SOURCE})
set_module_type(wow64test win32cui UNICODE)
add_importlibs(wow64test ntdll kernel32 user32 msvcrt gdi32 comctl32)
add_cd_file(TARGET wow64test DESTINATION reactos/system32 FOR all)

# 32-bit WoW64 test with CRT (printf via msvcrt.dll)
add_executable(hello32 hello32.c)
set_module_type(hello32 win32cui)
add_importlibs(hello32 msvcrt kernel32 ntdll)
add_cd_file(TARGET hello32 DESTINATION reactos/system32 FOR all)


add_executable(fltmc fltmc.c fltmc.rc)
set_module_type(fltmc win32cui UNICODE)
add_importlibs(fltmc fltlib msvcrt kernel32 user32 advapi32)
add_cd_file(TARGET fltmc DESTINATION reactos/system32 FOR all)

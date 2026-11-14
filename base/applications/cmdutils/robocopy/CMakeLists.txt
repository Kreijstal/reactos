
add_executable(robocopy robocopy.c robocopy.rc)
set_module_type(robocopy win32cui UNICODE)
target_link_libraries(robocopy conutils)
add_importlibs(robocopy msvcrt kernel32)
add_cd_file(TARGET robocopy DESTINATION reactos/system32 FOR all)

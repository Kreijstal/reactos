
add_executable(rospoke_cli main.c service.c rospoke.rc)
set_target_properties(rospoke_cli PROPERTIES OUTPUT_NAME "rospoke")
set_module_type(rospoke_cli win32cui)
add_importlibs(rospoke_cli advapi32 msvcrt kernel32)
add_cd_file(TARGET rospoke_cli DESTINATION reactos/system32 FOR all)

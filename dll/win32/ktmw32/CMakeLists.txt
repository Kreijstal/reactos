spec2def(ktmw32.dll ktmw32.spec ADD_IMPORTLIB)

add_library(ktmw32 MODULE
    ktmw32.c
    ktmw32.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ktmw32.def)
set_module_type(ktmw32 win32dll ENTRYPOINT DllMain 12)
add_importlibs(ktmw32 kernel32)
add_cd_file(TARGET ktmw32 DESTINATION reactos/system32 FOR all)

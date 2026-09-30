
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT} -DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

spec2def(normaliz.dll normaliz.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    IdnToAscii.c
    IdnToUnicode.c
    nameprep.c
    ${CMAKE_CURRENT_BINARY_DIR}/normaliz.def)

# NT6 kernel32 implements NormalizeString/IsNormalizedString and normaliz
# only forwards them there, as on Windows.
if(DLL_EXPORT_VERSION LESS 0x600)
    list(APPEND SOURCE normalize.c)
endif()

add_library(normaliz MODULE ${SOURCE})
set_module_type(normaliz win32dll ENTRYPOINT 0)
add_importlibs(normaliz msvcrt kernel32 ntdll)
add_cd_file(TARGET normaliz DESTINATION reactos/system32 FOR all)

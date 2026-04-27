
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT} -DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x602 -DWINVER=0x602)

spec2def(kernelbase.dll kernelbase.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    kernelbase.c
    kernelbase.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase.def)

add_library(kernelbase MODULE ${SOURCE})
set_module_type(kernelbase win32dll ENTRYPOINT DllMain 12)
add_importlibs(kernelbase kernel32 ntdll)
add_dependencies(kernelbase psdk)
add_cd_file(TARGET kernelbase DESTINATION reactos/system32 FOR all)

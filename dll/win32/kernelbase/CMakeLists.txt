
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT} -DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x602 -DWINVER=0x602)

spec2def(kernelbase.dll kernelbase.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    kernelbase.c
    kernelbase.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase.def)

add_subdirectory(wine)

spec2def(kernelbase_ros.dll kernelbase_ros.spec ADD_IMPORTLIB)

add_library(kernelbase_ros MODULE
    DllMain.c
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase_ros_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase_ros.def)

set_module_type(kernelbase_ros win32dll ENTRYPOINT DllMain 12)
target_link_libraries(kernelbase_ros psapi_static kernelbase_static wine chkstk)
add_importlibs(kernelbase_ros msvcrt advapi32 advapi32_vista kernel32 kernel32_vista ntdll ntdll_vista)
add_dependencies(kernelbase_ros psdk errcodes)
add_cd_file(TARGET kernelbase_ros DESTINATION reactos/system32 FOR all)

add_library(kernelbase MODULE ${SOURCE})
set_module_type(kernelbase win32dll ENTRYPOINT DllMain 12)
add_importlibs(kernelbase kernel32 ntdll)
add_dependencies(kernelbase psdk)
add_cd_file(TARGET kernelbase DESTINATION reactos/system32 FOR all)

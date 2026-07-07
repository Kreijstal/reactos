
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT} -DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x602 -DWINVER=0x602)

# NOTE: no ADD_IMPORTLIB here. This slim kernelbase.dll exists only so that the
# Cygwin/msys2 runtimes can LoadLibrary("kernelbase.dll") + GetProcAddress the
# few Win7+/Win8 entry points they probe; nothing link-imports it. The consumer
# facing "kernelbase" import library is the libkernelbase INTERFACE below, which
# forwards to the full kernelbase_ros.dll. Generating an import lib here as well
# would shadow that INTERFACE and starve consumers of the kernelbase_ros exports.
spec2def(kernelbase.dll kernelbase.spec)

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
target_link_libraries(kernelbase kernelbase_static wine chkstk)
add_importlibs(kernelbase msvcrt advapi32 kernel32 kernel32_vista ntdll)
add_dependencies(kernelbase psdk)
add_cd_file(TARGET kernelbase DESTINATION reactos/system32 FOR all)

# Fake import library, so consumers can link against it without needing to know
# about the actual implementation in kernelbase_ros.dll.
add_library(libkernelbase INTERFACE)
target_link_libraries(libkernelbase INTERFACE libkernelbase_ros)

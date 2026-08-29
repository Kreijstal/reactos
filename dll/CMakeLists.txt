
add_subdirectory(3rdparty)
add_subdirectory(appcompat)
add_subdirectory(cpl)
add_subdirectory(directx)
add_subdirectory(keyboard)
add_subdirectory(nls)
add_subdirectory(np)
add_subdirectory(ntdll)
add_subdirectory(opengl)
add_subdirectory(shellext)
add_subdirectory(win32)
# WoW64 is an NT6 subsystem here: Wow64LdrpInitialize calls RtlExitUserProcess,
# which ntdll.spec exports as -version=0x600+, so wow64.dll cannot link below
# that at all.
if(ARCH STREQUAL "amd64" AND DLL_EXPORT_VERSION GREATER_EQUAL 0x600)
    add_subdirectory(wow64)
    add_subdirectory(wow64cpu)
    add_subdirectory(wow64win)
endif()
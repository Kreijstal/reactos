add_executable(kmtcdrunner kmtcdrunner.c)
set_module_type(kmtcdrunner win32cui)
add_importlibs(kmtcdrunner advapi32 shell32 msvcrt kernel32 ntdll)

# kmtcdrunner sits in system32 next to other system binaries; it spawns
# kmtest.exe by absolute path resolved at runtime via GetSystemWindowsDirectory
# (kmtest.exe lives at %SystemRoot%\bin alongside the *_drv.sys files, which
# KmtCreateService resolves relative to its own EXE dir).
add_cd_file(TARGET kmtcdrunner DESTINATION reactos/system32 FOR kmtestcd)

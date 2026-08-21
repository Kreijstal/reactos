
add_rc_deps(iexplore.rc
    ${CMAKE_CURRENT_SOURCE_DIR}/iexplore.ico
    ${CMAKE_CURRENT_SOURCE_DIR}/iexplore.inf)
add_executable(iexplore main.c iexplore.rc)
target_link_libraries(iexplore wine)
set_module_type(iexplore win32gui UNICODE)
add_delay_importlibs(iexplore advpack version user32)
add_importlibs(iexplore ieframe msvcrt kernel32 ntdll)
# BootCD/RegTest media ship iexplore.exe inside reactos.cab at %SystemRoot%.
# syssetup.inf,[Finalize] (run by RunOnceEx on the first normal boot) then
# relocates it to '%ProgramFiles%\Internet Explorer' and DelNode's the copy
# in %SystemRoot%.  A LiveCD never performs that first normal boot -- and its
# medium is read-only anyway -- so ship the file straight to its final layout
# there instead.  That is the location shortcuts.inf and hivesft.inf
# (Clients\StartMenuInternet, the http/https handlers, ...) already expect.
add_cd_file(TARGET iexplore DESTINATION reactos FOR bootcd regtest)
add_cd_file(TARGET iexplore DESTINATION "Program Files/Internet Explorer" FOR livecd)

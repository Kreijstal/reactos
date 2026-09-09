#
# ar9485lab.exe - user-mode script runner for the AR9485 bring-up lab.
#
# Shipped on the CD as well as pushed over luagent: a debugging instrument
# that depends on the remote agent being alive is unavailable exactly when
# things are going badly.
#
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
add_executable(ar9485lab ar9485lab.c lab_script.c)
set_module_type(ar9485lab win32cui)
add_importlibs(ar9485lab msvcrt kernel32)
add_cd_file(TARGET ar9485lab DESTINATION reactos/system32 FOR all)

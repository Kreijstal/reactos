# Register CTest entries that drive the host-side rostest runner (rostest.py).
#
# This file is pulled into the main (cross) build only when ENABLE_ROSTEST_CTEST
# is set, so it never affects a normal build.  The tests run inside a VM: the
# emulator (QEMU or VirtualBox) is a *runtime* dependency, not a build
# dependency.  When no backend is found the entries are still registered but
# marked DISABLED, so configuring with the option on never breaks the build.
#
#   cmake -DENABLE_ROSTEST_CTEST=ON -DROSTEST_IMAGE=/path/reactos.qcow2 ...
#   ctest -R rostest_gdi32
#   ctest -R kmtest_NtfsDirIndex

# The main build already locates Python; only fall back to a plain search if
# not (find_program is safe here, find_package(Python3) is not — it clashes with
# ReactOS's add_custom_target macro override).
if(NOT Python3_EXECUTABLE)
    find_program(Python3_EXECUTABLE NAMES python3 python)
endif()

set(ROSTEST_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/rostest.py")
set(ROSTEST_BACKEND "qemu" CACHE STRING "VM backend for rostest (qemu|virtualbox)")
set(ROSTEST_IMAGE "" CACHE PATH "Installed ReactOS system image reused for rostests")
set(ROSTEST_ISO "${CMAKE_BINARY_DIR}/bootcdregtest.iso" CACHE PATH "Install ISO for --fresh")
set(ROSTEST_KMTEST_IMG "${CMAKE_BINARY_DIR}/kmtest.img" CACHE PATH "Bootable kmtest image")
set(ROSTEST_TIMEOUT "1800" CACHE STRING "Per-test CTest timeout in seconds")

# A usable backend present?  If not, the tests are registered DISABLED.
set(_rostest_disabled FALSE)
if(ROSTEST_BACKEND STREQUAL "qemu")
    find_program(ROSTEST_QEMU qemu-system-x86_64)
    if(NOT ROSTEST_QEMU)
        set(_rostest_disabled TRUE)
    endif()
elseif(ROSTEST_BACKEND STREQUAL "virtualbox")
    find_program(ROSTEST_VBOXMANAGE VBoxManage)
    if(NOT ROSTEST_VBOXMANAGE)
        set(_rostest_disabled TRUE)
    endif()
endif()
if(NOT Python3_EXECUTABLE)
    set(_rostest_disabled TRUE)
endif()
if(_rostest_disabled)
    message(STATUS "rostest: no usable '${ROSTEST_BACKEND}' backend/python found; CTest entries DISABLED")
endif()

function(rostest_add_test _name)
    add_test(NAME ${_name}
             COMMAND ${Python3_EXECUTABLE} ${ROSTEST_RUNNER}
                     --backend ${ROSTEST_BACKEND}
                     --image "${ROSTEST_IMAGE}"
                     --iso "${ROSTEST_ISO}"
                     --kmtest-img "${ROSTEST_KMTEST_IMG}"
                     --timeout ${ROSTEST_TIMEOUT}
                     ${ARGN})
    set_tests_properties(${_name} PROPERTIES TIMEOUT ${ROSTEST_TIMEOUT})
    if(_rostest_disabled)
        set_tests_properties(${_name} PROPERTIES DISABLED TRUE)
    endif()
endfunction()

# --- rostests: one entry per winetest module directory ---------------------
file(GLOB _rostest_modules RELATIVE ${CMAKE_SOURCE_DIR}/modules/rostests/winetests
     ${CMAKE_SOURCE_DIR}/modules/rostests/winetests/*)
foreach(_mod ${_rostest_modules})
    if(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/modules/rostests/winetests/${_mod})
        rostest_add_test(rostest_${_mod} --suite rostests --test ${_mod})
    endif()
endforeach()
rostest_add_test(rostest_all --suite rostests)

# --- kmtest: one entry per (non-hidden) name in the user-mode testlist -------
# kmtcdrunner selects tests by filtering the output of "kmtest_.exe --list",
# which prints exactly the non-hidden rows of the *user-mode* test list
# (modules/rostests/kmtests/kmtest/testlist.c).  Rows look like
# { "RtlMemory", Test_RtlMemory }; names beginning with '-' are hidden and not
# listed (the regex requires an alnum first char right after the quote, so they
# are skipped).  The driver-side list (kmtest_drv/testlist.c) is intentionally
# NOT read: several of its names are hidden in the user-mode list and so can
# never be matched by a --list filter.
set(_kmtest_names "")
set(_path ${CMAKE_SOURCE_DIR}/modules/rostests/kmtests/kmtest/testlist.c)
if(EXISTS ${_path})
    file(STRINGS ${_path} _lines REGEX "{[ \t]*\"[A-Za-z0-9_]+\"")
    foreach(_line ${_lines})
        string(REGEX MATCH "\"([A-Za-z0-9_]+)\"" _ignored "${_line}")
        if(CMAKE_MATCH_1)
            list(APPEND _kmtest_names ${CMAKE_MATCH_1})
        endif()
    endforeach()
endif()
list(REMOVE_DUPLICATES _kmtest_names)
foreach(_t ${_kmtest_names})
    rostest_add_test(kmtest_${_t} --suite kmtest --test ${_t})
endforeach()
rostest_add_test(kmtest_all --suite kmtest)

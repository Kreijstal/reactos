
add_subdirectory(csrlib)
if(NOT SARCH STREQUAL "wow64")
    add_subdirectory(csrsrv)
    add_subdirectory(csrss)
endif()

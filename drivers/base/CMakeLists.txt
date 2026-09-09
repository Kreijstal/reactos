
add_subdirectory(beep)
add_subdirectory(bootvid)
add_subdirectory(condrv)

if(_WINKD_)
    add_subdirectory(kdcom)
    if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
        add_subdirectory(kdgdb)
    endif()
    if(NOT ARCH STREQUAL "arm")
        add_subdirectory(kdvm)
    endif()
else()
    add_subdirectory(kdrosdbg)
    if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
        add_subdirectory(kdvm)
    endif()
endif()
add_subdirectory(kdstub)

if(ARCH STREQUAL "amd64" AND _WINKD_)
    add_subdirectory(kdnet)
endif()

if(NOT ARCH STREQUAL "arm")
add_subdirectory(nmidebug)
endif()
add_subdirectory(null)

# Debug-only: hands arbitrary MMIO, DMA and PCI config access to user mode.
# Off by default and must never be enabled in a build other people will run.
if(ENABLE_ROSPOKE)
    add_subdirectory(rospoke)
endif()

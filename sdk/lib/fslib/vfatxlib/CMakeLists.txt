
list(APPEND SOURCE
    fatx.c
    vfatxlib.c
    vfatxlib.h)

add_library(vfatxlib ${SOURCE})
add_pch(vfatxlib vfatxlib.h SOURCE)
target_link_libraries(vfatxlib chkstk)

if(NOT MSVC)
    if(TARGET libgcc)
        target_link_libraries(vfatxlib libgcc)
    else()
        target_link_libraries(vfatxlib -lgcc)
    endif()
endif()

add_dependencies(vfatxlib psdk)

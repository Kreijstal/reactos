include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND SOURCE
    blue.c
    font.c
    blue.h)

if(ARCH STREQUAL "arm" OR ARCH STREQUAL "arm64")
    # Console rendering onto the boot display framebuffer,
    # reusing the boot video driver font data
    list(APPEND SOURCE
        framebuf.c
        ${REACTOS_SOURCE_DIR}/drivers/base/bootvid/fontdata.c)
endif()

add_library(blue MODULE ${SOURCE} blue.rc)
set_module_type(blue kernelmodedriver)
add_importlibs(blue ntoskrnl hal)
add_pch(blue blue.h SOURCE)
add_cd_file(TARGET blue DESTINATION reactos/system32/drivers NO_CAB FOR all)

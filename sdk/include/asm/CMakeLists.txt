
add_library(genincdata MODULE genincdata.c)
target_compile_definitions(genincdata PRIVATE NTDDI_VERSION=${REACTOS_TARGET_NTDDI})

set_entrypoint(genincdata 0)
add_dependencies(genincdata bugcodes psdk xdk)

if(ARCH STREQUAL "i386")
    set(_filename ks386.inc)
elseif(ARCH STREQUAL "amd64")
    set(_filename ksamd64.inc)
elseif(ARCH STREQUAL "arm")
    set(_filename ksarm.h)
elseif(ARCH STREQUAL "arm64")
    set(_filename ksarm64.h)
endif()

if(REACTOS_GENERATE_MS_ASM)
    set(OPT_MS "-ms")
else()
    set(OPT_MS "")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${_filename}
    COMMAND native-geninc $<TARGET_FILE:genincdata> ${CMAKE_CURRENT_BINARY_DIR}/${_filename} ${OPT_MS}
    DEPENDS genincdata native-geninc)

add_custom_target(asm
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${_filename})


list(APPEND SOURCE
    samplerate.c
    src_linear.c
    src_sinc.c
    src_zoh.c
    precomp.h)

add_library(libsamplerate STATIC ${SOURCE})
add_pch(libsamplerate precomp.h SOURCE)
add_dependencies(libsamplerate psdk)
target_include_directories(libsamplerate INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    # C4815: MSVC 14.52 warns about the flexible array member of the local
    # SINC_FILTER sinc_set_converter() only uses for its fixed-size fields.
    target_compile_options(libsamplerate PRIVATE /wd4815)
endif()

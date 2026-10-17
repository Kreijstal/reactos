add_library(ntfslib
    ntfslib.c
    bootsect.c
    files.c
    restart.c
    upcase_data.c
    chkdskio.c
    chkdsk.c
    chkscan.c
    chkrepair.c
    chkutil.c)
target_compile_definitions(ntfslib PRIVATE NTOS_MODE_USER=)
add_dependencies(ntfslib psdk)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ntfslib PRIVATE -Wno-shift-overflow)
endif()

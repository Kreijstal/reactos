add_library(ntfslib
    ntfslib.c
    mkntfs.c
    chkdsk.c
    chkscan.c
    chkrepair.c
    attrdef.c
    upcase.c)
target_compile_definitions(ntfslib PRIVATE NTOS_MODE_USER=)
add_dependencies(ntfslib psdk)

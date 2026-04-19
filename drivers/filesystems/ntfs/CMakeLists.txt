
list(APPEND SOURCE
    attrib.c
    blockdev.c
    btree.c
    cleanup.c
    close.c
    compress.c
    create.c
    devctl.c
    dirctl.c
    dispatch.c
    ea.c
    fastio.c
    fcb.c
    finfo.c
    fsctl.c
    lock.c
    lznt1.c
    mft.c
    misc.c
    ntfs.c
    pnp.c
    rw.c
    security.c
    shutdown.c
    usn.c
    volinfo.c
    ntfs.h)

add_library(ntfs MODULE ${SOURCE} ntfs.rc)
set_module_type(ntfs kernelmodedriver)
target_link_libraries(ntfs ${PSEH_LIB})
add_importlibs(ntfs ntoskrnl hal)
add_pch(ntfs ntfs.h SOURCE)
add_cd_file(TARGET ntfs DESTINATION reactos/system32/drivers NO_CAB FOR all)

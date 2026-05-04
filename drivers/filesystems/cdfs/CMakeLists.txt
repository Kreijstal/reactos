
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND SOURCE
    allocsup.c
    cachesup.c
    cddata.c
    cdinit.c
    cdprocssrc.c
    cleanup.c
    close.c
    create.c
    devctrl.c
    deviosup.c
    dirctrl.c
    dirsup.c
    fileinfo.c
    filobsup.c
    fsctrl.c
    fspdisp.c
    lockctrl.c
    namesup.c
    pathsup.c
    pnp.c
    prefxsup.c
    read.c
    resrcsup.c
    shutdown.c
    strucsup.c
    verfysup.c
    volinfo.c
    workque.c
    write.c)

add_library(cdfs MODULE ${SOURCE} cdfs.rc)
set_module_type(cdfs kernelmodedriver)
target_link_libraries(cdfs ${PSEH_LIB} memcmp)
if((NOT MSVC) AND (ARCH STREQUAL "amd64"))
    # Block reordering can move code in a _SEH2_TRY scope outside the
    # [begin_try, filter) RVA range that the GCC SEH plugin records,
    # which causes __C_specific_handler to miss the handler and the
    # exception to escape uncaught (e.g. STATUS_VERIFY_REQUIRED on a
    # CD-ROM media-change traps in kdb instead of being handled by
    # CdFsdDispatch).
    target_compile_options(cdfs PRIVATE -fno-reorder-blocks)
endif()
add_importlibs(cdfs ntoskrnl hal)

add_cd_file(TARGET cdfs DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(cdfs_reg.inf)

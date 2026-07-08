list(APPEND SOURCE
    virtiommio.c
    virtiommio.h)

add_library(virtiommio MODULE ${SOURCE} virtiommio.rc)
set_module_type(virtiommio kernelmodedriver)
add_importlibs(virtiommio ntoskrnl hal)
add_pch(virtiommio virtiommio.h SOURCE)
add_cd_file(TARGET virtiommio DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(virtiommio virtiommio.inf)

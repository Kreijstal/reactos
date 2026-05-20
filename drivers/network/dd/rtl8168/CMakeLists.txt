
add_definitions(
    -DNDIS50_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_MINIPORT=1)

list(APPEND SOURCE
    ndis.c
    hardware.c
    indirect.c
    chipinit.c
    phy.c
    info.c
    interrupt.c
    nic.h)

add_library(rtl8168 MODULE ${SOURCE} rtl8168.rc)
add_pch(rtl8168 nic.h SOURCE)
set_module_type(rtl8168 kernelmodedriver)
add_importlibs(rtl8168 ndis ntoskrnl hal)
add_cd_file(TARGET rtl8168 DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(rtl8168 netr8168.inf)

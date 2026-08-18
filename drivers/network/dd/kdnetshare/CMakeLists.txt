# NDIS 6.20 miniport that shares the kernel debugger's network adapter with the
# OS.  It drives no hardware: every frame goes through the share interface
# exported by kdnet.dll, which retains exclusive ownership of the NIC.

add_definitions(
    -DNDIS620_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_DRIVER=1
    -DNTDDI_VERSION=0x06010000)

add_library(kdnetshare MODULE
    kdnetshare.c
    kdnetshare.rc)

set_module_type(kdnetshare kernelmodedriver)

# Imported through kdcom, not kdnet: the boot loader loads whichever KD transport
# was selected under the single hardcoded name "kdcom.dll", so that is the name
# every export of the running transport is reached by.  On a /DEBUGPORT=NET boot
# these bind to kdnet.dll; kdcom itself exports declining stubs.
add_importlibs(kdnetshare ndis kdcom ntoskrnl hal)
add_pch(kdnetshare kdnetshare.h "")

add_cd_file(TARGET kdnetshare DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(kdnetshare netkdnetshare.inf)

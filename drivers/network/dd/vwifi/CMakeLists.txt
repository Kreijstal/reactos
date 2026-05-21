#
# vwifi - Virtual NDIS 6.20 Native-802.11 miniport
#
# Software-only Wi-Fi adapter for upper-stack development and testing.
# No PCI, no IRQ, no DMA. Pretends to be a Wi-Fi card so wlansvc /
# wlanapi / ndisuio / DOT11 OID surface can be exercised under QEMU
# without any real silicon.
#
# Same NT 6.0+ gate as e1000e — needs NDIS 6.20 NET_BUFFER_LIST datapath
# and the DOT11 OID surface (added 2026-05 in sdk/include/psdk/windot11.h).
#

add_definitions(
    -DNDIS620_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_DRIVER=1
    -DNDIS_LEGACY_MINIPORT=1
    -DNTDDI_VERSION=0x06010000)

list(APPEND SOURCE
    init.c
    oid.c
    send.c
    recv.c
    scan.c
    vwifi.h)

add_library(vwifi MODULE ${SOURCE} vwifi.rc)
set_module_type(vwifi kernelmodedriver)

target_compile_options(vwifi PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable)

add_importlibs(vwifi ndis ntoskrnl hal)
add_cd_file(TARGET vwifi DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(vwifi netvwifi.inf)

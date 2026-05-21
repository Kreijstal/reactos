#
# AR9485 - Atheros AR9485 PCIe Wi-Fi NDIS 6.20 miniport
#
# Phase 1a delivers a loading skeleton that binds to PCI 168C:0032,
# maps BAR0, reads AR_SREV at offset 0x4020, and reports a small set
# of identification OIDs.  Subsequent phases drop the verbatim Linux
# ath9k_hw layer into an ath9k/ subdirectory and wire it from init.c.
#

add_definitions(
    -DNDIS620_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_DRIVER=1
    -DNDIS_LEGACY_MINIPORT=1
    -DNTDDI_VERSION=0x06010000)

list(APPEND SOURCE
    driver.c
    init.c
    interrupt.c
    oid.c
    send.c
    receive.c
    ar9485.h
    linux-compat.h
    ath9k/ath_reg.h
    ath9k/reg.h)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(ar9485 MODULE ${SOURCE} ar9485.rc)
set_module_type(ar9485 kernelmodedriver)
add_importlibs(ar9485 ndis ntoskrnl hal)
add_cd_file(TARGET ar9485 DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(ar9485 netar9485.inf)

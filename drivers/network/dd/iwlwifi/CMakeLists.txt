#
# iwlwifi - Intel Wireless PCIe NDIS 6.20 miniport
#
# Phase 1a delivers the identification chain only: bind the PCI IDs in
# hw/devices.c, map BAR0, run the documented transport power-up, decode
# CSR_HW_REV / CSR_HW_RF_ID, and locate + parse the matching .ucode
# container.  MiniportInitializeEx then deliberately fails, because a
# native-802.11 miniport with no NVM address and no data path is not a
# network adapter (see the header of init.c).
#
# That is why this driver is OFF by default.  Turning it on would make
# every Intel wireless machine show a device that binds and then fails,
# in exchange for a diagnostic log.  That trade is right for bring-up
# work and wrong for a release image, so it is a switch:
#
#   -DENABLE_IWLWIFI=ON              build and package the driver
#   -DDOWNLOAD_IWLWIFI_UCODE=ON      fetch the pinned firmware blobs
#
# Flip the default when the data path lands.
#
# fw/ucode_parse.c is deliberately free of NDIS, the DDK and the CRT so the
# same file can be built for the host by fw/harness/ucode_parse_test.c and
# run against real linux-firmware blobs.  Nothing here references the
# harness; see fw/harness/README.md.
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
    hw/devices.c
    hw/trans.c
    fw/ucode_parse.c
    fw/ucode_load.c
    iwlwifi.h
    hw/csr.h
    hw/devices.h
    fw/ucode_file.h)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(iwlwifi MODULE ${SOURCE} iwlwifi.rc)
set_module_type(iwlwifi kernelmodedriver)
add_importlibs(iwlwifi ndis ntoskrnl hal)
add_cd_file(TARGET iwlwifi DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(iwlwifi netiwlwifi.inf)

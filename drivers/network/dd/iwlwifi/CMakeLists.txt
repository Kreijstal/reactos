#
# iwlwifi - Intel Wireless PCIe NDIS 6.20 miniport
#
# This remains opt-in while hardware coverage is limited.  The implemented
# AX211 path loads firmware/PNVM, scans, associates, installs CCMP/TKIP keys,
# and carries an 802.11 data path through the Native Wi-Fi translation layer.
# Other PCI IDs in the identification table have not received equivalent
# hardware validation, so release images should enable it deliberately:
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
    fw/gen3.c
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

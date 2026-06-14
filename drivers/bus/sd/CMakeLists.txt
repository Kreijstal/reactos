
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND SOURCE
    sdbus.c
    hardware.c
    bcm2712.c
    fdo.c
    pdo.c
    enum.c
    hotplug.c
    request.c
    interface.c
    power.c
    sdio.c
    emmc.c
    hardware.h
    bcm2712.h
    sdbus.h)

add_library(sdbus MODULE ${SOURCE} sdbus.rc)
set_module_type(sdbus kernelmodedriver)
add_importlibs(sdbus ntoskrnl hal)
add_pch(sdbus sdbus.h SOURCE)
add_cd_file(TARGET sdbus DESTINATION reactos/system32/drivers NO_CAB FOR all)
# Pre-register sdbus as a boot-start "System Bus Extender" service only on
# platforms that actually boot off an SD/eMMC device (Raspberry Pi / ARM64).
# On PC targets the SD host is a hot-pluggable PCI/ACPI device that PnP
# enumerates on demand through sdbus.inf, exactly like upstream; forcing it
# boot-start there makes freeldr fail fatally ("Cannot load boot driver
# sdbus.sys") when the image cannot be loaded, taking the whole system down.
if(ARCH STREQUAL "arm64")
    add_registry_inf(sdbus_reg.inf)
endif()
add_driver_inf(sdbus sdbus.inf)

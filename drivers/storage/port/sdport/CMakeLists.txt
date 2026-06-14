
spec2def(sdport.sys sdport.spec ADD_IMPORTLIB)

add_library(sdport MODULE
    sdport.c
    protocol.c
    command.c
    transfer.c
    power.c
    sdport.rc
    ${CMAKE_CURRENT_BINARY_DIR}/sdport.def)

set_module_type(sdport kernelmodedriver)
target_link_libraries(sdport ${PSEH_LIB})
add_importlibs(sdport ntoskrnl hal)
add_cd_file(TARGET sdport DESTINATION reactos/system32/drivers NO_CAB FOR all)
# Boot-start only on platforms that boot off SD/eMMC (Raspberry Pi / ARM64).
# On PC targets the SDHCI controller is PnP-enumerated on demand and the boot
# volume is on ATA/AHCI, so forcing sdport boot-start there makes freeldr abort
# the whole boot ("Cannot load boot driver sdport.sys"). See drivers/bus/sd.
if(ARCH STREQUAL "arm64")
    add_registry_inf(sdport_reg.inf)
endif()

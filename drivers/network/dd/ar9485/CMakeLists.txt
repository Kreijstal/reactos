#
# AR9485 - Atheros AR9485 PCIe Wi-Fi NDIS 6.20 miniport
#
# Phase 1a delivers a loading skeleton that binds to PCI 168C:0032,
# maps BAR0, reads AR_SREV at offset 0x4020, and reports a small set
# of identification OIDs.  Subsequent phases drop the verbatim Linux
# ath9k_hw layer into an ath9k/ subdirectory and wire it from init.c.
#
# Phase 2a adds slice 3: the AR9300-family EEPROM/OTP restore
# (ath9k/hw_eeprom.c + ath9k/ar9003_eeprom_min.h) and the RTC power-on
# it needs, so the miniport reports the card's real permanent MAC.
#

#
# The bring-up lab (lab.c) exposes a control device that lets a user-mode tool
# drive the chip's registers and DMA buffers, so AR9485 experiments cost a
# push-and-run instead of a deploy and a reboot.  It is a development
# instrument: an arbitrary MMIO write primitive behind an admin-only DACL.
# On by default for Debug builds only, and never wanted in a release image.
#
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_AR9485_LAB_DEFAULT ON)
else()
    set(_AR9485_LAB_DEFAULT OFF)
endif()
option(AR9485_LAB "Build the AR9485 user-mode bring-up lab control device"
       ${_AR9485_LAB_DEFAULT})

#
# Boot 60 wedged ~90 s into the first build carrying the EEPROM board values and
# the TX I/Q calibration, and is still unexplained.  Two writes were suspected,
# because they touch the analog supply and the reference clock rather than the
# PHY: the PMU reprogram and the crystal tuning caps.  Both were retired
# without spending a boot -- one of them wrongly, as the next paragraph records:
#
#   - the tuning caps are gated on featureEnable & 0x40 and this card's
#     featureEnable is 0x3d, so that function is a no-op here and could never
#     have wedged anything (raw OTP dump, 2026-08-26);
#   - the PMU sequence was replayed write-by-write on the running box through
#     the bring-up lab, immediately after the driver's own bring-up, and the
#     chip took all four writes and stayed alive: AR_SREV constant, PMU1/PMU2
#     holding, PHY still answering.  See docs/asus.txt.
#
# BOOT 63 (2026-08-26) RAN THAT TEST AND THE PMU LOST.  With NO_PMU=OFF the box
# booted, answered luagent, logged its boot drivers and cpu=0/4 over KD, and
# then wedged hard at roughly 90-120 s: no ARP, no KD, physical power button.
# Boot 62 and boot 63 differ in this write sequence and nothing else, and boot
# 60 wedged the same way with it applied.  Two wedges with, clean boots without.
# The lab replay was not wrong, just short -- it watches ~4 s and the failure
# needs ~90, exactly the slow brownout it could not reach.
#
# BOOT 64 (2026-08-27) THEN TOOK IT BACK.  The box powered on with the PMU
# build still on disk and did not wedge -- still answering 13 minutes in, chip
# healthy, and AR_PHY_PMU1 reading back 0x131c827a, the exact constant the
# sequence writes.  Two wedges with the PMU and one clean boot with it: the
# boot-62-vs-63 inference was too strong, and boots 60 and 63 are unexplained
# again.
#
# BOOT 65 (2026-08-27) CLOSED THE LAST ALTERNATIVE.  On the NO_PMU binary,
# AR_PHY_PMU1 reads 0x13188278 -- differing from the programmed 0x131c827a in
# exactly bits 1 and 18 -- and AR_PHY_PMU2's PGM bit is clear.  0x131c827a is
# therefore written by the driver, not a reset default, so boot 64 genuinely
# ran the PMU sequence and genuinely did not wedge.  See hw_board.c.
#
# The default stays OFF so the source keeps doing what upstream does and the
# bug stays visible rather than papered over.  The binary deployed to the ASUS
# is still the NO_PMU one, now as the control for that read rather than as a
# known-good workaround.  It matches upstream's AR_SREV_9485 arm write for
# write, so the fault is not a transcription slip and is still open.
#
option(AR9485_NO_PMU
       "Bisect lever: omit the internal-regulator (PMU) reprogram" OFF)
if(AR9485_NO_PMU)
    add_definitions(-DAR9485_NO_PMU)
endif()
message(STATUS "AR9485: NO_PMU=${AR9485_NO_PMU}")

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
    mlme.c
    log.c
    ath9k/hw_chip.c
    ath9k/hw_eeprom.c
    ath9k/hw_reset.c
    ath9k/hw_board.c
    ath9k/hw_calib_tx.c
    ath9k/hw_txpower.c
    ar9485.h
    dot11frame.h
    linux-compat.h
    ath9k/ath_reg.h
    ath9k/reg.h
    ath9k/hw_min.h
    ath9k/mac_desc.h
    ath9k/ar9003_eeprom_min.h
    ath9k/ar9003_eeprom_templates.h
    ath9k/ar9003_phy.h
    ath9k/ar9485_initvals.h)

if(AR9485_LAB)
    add_definitions(-DAR9485_LAB)
    list(APPEND SOURCE lab.c lab_vm.c)
    message(STATUS "AR9485: bring-up lab control device enabled")
    add_subdirectory(tool)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(ar9485 MODULE ${SOURCE} ar9485.rc)
set_module_type(ar9485 kernelmodedriver)
add_importlibs(ar9485 ndis ntoskrnl hal)
add_cd_file(TARGET ar9485 DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(ar9485 netar9485.inf)

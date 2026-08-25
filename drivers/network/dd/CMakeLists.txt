
add_subdirectory(dc21x4)
add_subdirectory(e1000)
# e1000e is the NDIS 6.20 PCIe variant; only built at NT 6.0 (Vista) or
# later because it depends on NET_BUFFER_LIST datapath and the NDIS 6 stub
# exports in drivers/network/ndis/ndis/60stubs.c. Below Vista the legacy
# e1000 (NDIS 5.0) driver covers the same hardware family.
math(EXPR _E1000E_NT_NUM "${REACTOS_TARGET_NT} + 0")
if(_E1000E_NT_NUM GREATER_EQUAL 1536)
    add_subdirectory(e1000e)
endif()
add_subdirectory(ne2000)
add_subdirectory(netkvm)
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    add_subdirectory(nvnet)
endif()
add_subdirectory(pcnet)
add_subdirectory(rtl8139)
add_subdirectory(rtl8168)
# rp1gem is the Gigabit Ethernet miniport for the RP1 southbridge on the
# Raspberry Pi 5; ARM64-only silicon. hivesys_arm64.inf registers it as a
# boot-start service, so the binary must be present on every ARM64 image or
# the boot loader aborts with "Cannot load boot driver".
if(ARCH STREQUAL "arm64")
    add_subdirectory(rp1gem)
endif()
# ar9485 is an NDIS 6.20 native-802.11 miniport - same NT 6.0+ gating as e1000e.
if(_E1000E_NT_NUM GREATER_EQUAL 1536)
    add_subdirectory(ar9485)
endif()
# iwlwifi is an NDIS 6.20 native-802.11 miniport for Intel Wireless PCIe
# parts; same NT 6.0+ gating as e1000e.  Off by default: Phase 1a stops
# after identifying the part and validating its firmware, so binding it
# would only produce a failed device plus a diagnostic log.  See
# ENABLE_IWLWIFI in sdk/cmake/config.cmake.
if(_E1000E_NT_NUM GREATER_EQUAL 1536 AND ENABLE_IWLWIFI)
    add_subdirectory(iwlwifi)
endif()
# vwifi is a software-only NDIS 6.20 native-802.11 miniport used for
# wlan upper-stack development under QEMU when no real Wi-Fi silicon is
# available. Same NT 6.0+ gate.
if(_E1000E_NT_NUM GREATER_EQUAL 1536)
    add_subdirectory(vwifi)
endif()
# kdnetshare lets the OS use the NIC the kernel debugger owns, so a machine
# with a single adapter no longer has to choose between KDNET and a network.
# NDIS 6.20, same NT 6.0+ gate; inert unless /KDNETSHARE is passed at boot.
if(_E1000E_NT_NUM GREATER_EQUAL 1536)
    add_subdirectory(kdnetshare)
endif()

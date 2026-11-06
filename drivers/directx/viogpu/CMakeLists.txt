
# Genuine Red Hat VirtIO GPU WDDM Display-Only Driver (viogpudo.sys), shipped
# AS-IS from virtio-win (NOT recompiled; BSD-3-Clause, redistributable). It is
# the reference third-party WDDM miniport for ReactOS' dxgkrnl. PnP binds it to
# the QEMU virtio-gpu PCI device via viogpudo_reg.inf (CriticalDeviceDatabase).
#
# Source: virtio-win-0.1.285 (driver 100.101.104.28500), viogpudo/w11/<arch>/
#   amd64: md5 7a5fb036b159375f119864c7fbee82f8
#   arm64: md5 de615259c5a259be6089490cd6b2fc75

if(ARCH STREQUAL "amd64")
    set(VIOGPUDO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/amd64)
elseif(ARCH STREQUAL "arm64")
    set(VIOGPUDO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/arm64)
endif()

if(VIOGPUDO_DIR)
    add_cd_file(FILE ${VIOGPUDO_DIR}/viogpudo.sys
                DESTINATION reactos/system32/drivers NO_CAB FOR all)
    add_cd_file(FILE ${VIOGPUDO_DIR}/viogpudo.inf
                DESTINATION reactos/inf NO_CAB FOR all)
    add_registry_inf(viogpudo_reg.inf)
    if(ARCH STREQUAL "arm64")
        add_registry_inf(viogpudo_msi_reg.inf)
    endif()
endif()

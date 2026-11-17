
# The xHCI driver from upstream ReactOS (#9646): a KMDF client of the UCX class
# extension. Built as usbxhciucx so it can sit next to the usbport miniport
# usbxhci; USB_XHCI_UCX selects which of the two the images bind.

remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT})

list(APPEND SOURCE
    bulkio.cpp
    cmdring.cpp
    ctrlxfer.cpp
    dmapool.cpp
    epctx.cpp
    epstate.cpp
    hc.cpp
    intr.cpp
    isoxfer.cpp
    mmio.cpp
    rootports.cpp
    slotdev.cpp
    slots.cpp
    xferring.cpp
    xhcidrv.h
    xhcientry.cpp)

add_library(usbxhciucx MODULE ${SOURCE} usbxhciucx.rc)
set_module_type(usbxhciucx kmdfdriver)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # WDF_DECLARE_CONTEXT_TYPE passes a string literal as PCHAR
    target_compile_options(usbxhciucx PRIVATE -Wno-write-strings)
endif()

target_compile_definitions(usbxhciucx PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(usbxhciucx ucxstub ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(usbxhciucx wdfldr ntoskrnl hal)
add_cd_file(TARGET usbxhciucx DESTINATION reactos/system32/drivers NO_CAB FOR all)

# The service is boot-start, so register it only in images that bind it
if(USB_XHCI_UCX)
    add_registry_inf(usbxhciucx_reg.inf usbxhciucx_bind.inf)
    add_driver_inf(usbxhciucx usbxhciucx.inf)
endif()

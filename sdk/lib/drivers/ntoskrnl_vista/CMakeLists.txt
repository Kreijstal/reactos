remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0x602)

add_definitions(-DUNICODE -D_UNICODE -D_NTOSKRNL_ -D_NTSYSTEM_)

include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/rtl)

list(APPEND SOURCE
    etw.c
    fsrtl.c
    io.c
    ioprop.c
    po.c
    ke.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/rtl/utf8.c)

# The kernel has the power framework from NT 6.2 on; these stubs would hide it
if(DLL_EXPORT_VERSION LESS 0x602)
    list(APPEND SOURCE pofx.c)
endif()

# IoOpenDriverRegistryKey is a Windows 10 1803 API
set_source_files_properties(io.c PROPERTIES COMPILE_DEFINITIONS NTDDI_VERSION=0x0A000005)

add_library(ntoskrnl_vista ${SOURCE})
target_link_libraries(ntoskrnl_vista PRIVATE pseh) # rtl_vista)
add_dependencies(ntoskrnl_vista bugcodes xdk)

target_compile_definitions(ntoskrnl_vista PUBLIC NTKRNLVISTA)

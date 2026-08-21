# Build a local KDBG kernel alongside a primary WinKD kernel. Keep this in a
# subdirectory so the KDBG definitions and source selection do not leak into
# the primary ntoskrnl and ntkrnlmp targets.
set(_WINKD_ FALSE)
set(KDBG TRUE)
remove_definitions(-D_WINKD_)
include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl)

set(SOURCE)
set(ASM_SOURCE)
include(../ntos.cmake)

set(NTKRNLK_SOURCE ${SOURCE})
set(NTKRNLK_ASM_SOURCE ${ASM_SOURCE})

spec2def(ntkrnlk.exe ../ntoskrnl.spec)

if(MSVC_IDE)
    set_source_files_properties(${NTKRNLK_ASM_SOURCE} PROPERTIES COMPILE_DEFINITIONS "CONFIG_SMP")
endif()

add_asm_files(ntkrnlk_asm ${NTKRNLK_ASM_SOURCE})

if(ARCH STREQUAL "amd64")
    list(APPEND NTKRNLK_SOURCE
        ${REACTOS_SOURCE_DIR}/ntoskrnl/ke/amd64/mproc.c)
endif()

list(APPEND NTKRNLK_PCH_SKIP_SOURCE
    ${REACTOS_SOURCE_DIR}/ntoskrnl/guid.c)

add_executable(ntkrnlk
    ${ntkrnlk_asm}
    ${NTKRNLK_SOURCE}
    ${NTKRNLK_PCH_SKIP_SOURCE}
    ${REACTOS_SOURCE_DIR}/ntoskrnl/ntoskrnl.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ntkrnlk.def)
set_module_type(ntkrnlk kernel)
target_compile_definitions(ntkrnlk PRIVATE CONFIG_SMP __ROS_ROSSYM__)

if(STACK_PROTECTOR)
    target_link_libraries(ntkrnlk gcc_ssp_nt)
endif()

target_link_libraries(ntkrnlk ntoskrnl_vista cportlib csq ${PSEH_LIB} arbiter cmlib ntlsalib rtl rtl_vista rossym libcntpr setjmp wdmguid poguid ioevent)
add_importlibs(ntkrnlk hal kdcom bootvid)
add_pch(ntkrnlk ${REACTOS_SOURCE_DIR}/ntoskrnl/include/ntoskrnl.h "${NTKRNLK_PCH_SKIP_SOURCE}")
add_dependencies(ntkrnlk psdk asm)
add_cd_file(TARGET ntkrnlk DESTINATION reactos/system32 NO_CAB FOR all)

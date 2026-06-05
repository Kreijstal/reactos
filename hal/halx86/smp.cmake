
list(APPEND HAL_SMP_SOURCE
    apic/apicsmp.c
    generic/buildtype.c
    generic/spinlock.c
    smp/ipi.c
    smp/smp.c)

if(ARCH STREQUAL "i386")
    list(APPEND HAL_SMP_ASM_SOURCE
        smp/i386/apentry.S)
    list(APPEND HAL_SMP_SOURCE
        smp/i386/spinup.c)
elseif(ARCH STREQUAL "amd64")
    # The AP trampoline uses .code16 / .code32 / .code64 directives
    # that asm.inc lowers into MASM "SEGMENT use16" prologues.  ml64
    # (and clang-cl-driven ml64) has no 16-bit segment support and
    # rejects the prologue, so we exclude the trampoline from MSVC /
    # clang-cl builds for now.  spinup.c then gates HalStartNextProcessor
    # on _NO_AP_TRAMPOLINE_ and returns FALSE (UP behaviour), so the
    # HAL still exports the symbol but no AP bringup happens under
    # MSVC.  GCC-built amd64 SMP HAL continues to use the trampoline.
    if(NOT MSVC)
        list(APPEND HAL_SMP_ASM_SOURCE
            smp/amd64/apentry.S)
    else()
        set_source_files_properties(smp/amd64/spinup.c PROPERTIES
            COMPILE_DEFINITIONS "_NO_AP_TRAMPOLINE_")
    endif()
    list(APPEND HAL_SMP_SOURCE
        smp/amd64/spinup.c)
endif()

add_asm_files(lib_hal_smp_asm ${HAL_SMP_ASM_SOURCE})
add_library(lib_hal_smp OBJECT ${HAL_SMP_SOURCE} ${lib_hal_smp_asm})
add_dependencies(lib_hal_smp bugcodes asm xdk)
target_compile_definitions(lib_hal_smp PRIVATE CONFIG_SMP)

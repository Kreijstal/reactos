# Static supplement for modules that link ucrtbase.
#
# On x86, ucrtbase.dll does not export ceilf, floorf and sqrtf (they are
# header inlines there, matching Windows), but Clang folds patterns like
# (float)sqrt((double)x) - which is exactly what those inlines expand to -
# back into sqrtf/ceilf/floorf libcalls, so every float user ends up with
# undefined references. Provide the same static fallbacks msvcrtex has for
# this case; libucrtbase links this interface-style, and the archive
# members are only pulled when such a call was actually emitted.
if(ARCH STREQUAL "i386" AND CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
    add_asm_files(ucrtex_asm
        math/i386/ceilf.S
        math/i386/floorf.S)
    add_library(ucrtex
        math/i386/sqrtf.c
        ${ucrtex_asm})
    add_dependencies(ucrtex psdk asm)
endif()

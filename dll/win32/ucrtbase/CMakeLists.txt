
spec2def(ucrtbase.dll ucrtbase.spec NO_DBG ADD_IMPORTLIB)

# Hack to replace the old CRT include directory with the UCRT include directory
get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/ucrt)

add_compile_definitions(
    _UCRT
    _CORECRT_BUILD
    CRTDLL
)

add_library(ucrtbase SHARED
    cxxguard.cpp
    printf.c
    stubs.c
    ucrtbase_stubs.c
    ucrtbase.def
)

if(NOT MSVC)
    target_compile_options(ucrtbase PRIVATE -Wno-builtin-declaration-mismatch)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Silence warnings in ucrtbase_stubs.c
    target_compile_options(ucrtbase PRIVATE -Wno-incompatible-library-redeclaration)
    target_compile_options(ucrtbase PRIVATE
        -fno-builtin-_lrotl
        -fno-builtin-_lrotr
        -fno-builtin-_rotl
        -fno-builtin-_rotr
        -fno-builtin-_rotl64
        -fno-builtin-_rotr64)
endif()

set_entrypoint(ucrtbase __acrt_DllMain 12)

target_link_libraries(ucrtbase
    ucrt
    msvcrt_shared
    crtmath
    vcruntime
    vcstartup
    chkstk
    wine
)

# Implicitly link to vcstartup
target_link_libraries(libucrtbase INTERFACE vcstartup)

if(MSVC)
    target_link_libraries(ucrtbase runtmchk)
endif()

add_importlibs(ucrtbase kernel32 ntdll)

add_cd_file(TARGET ucrtbase DESTINATION reactos/system32 FOR all)

macro(require_program varname execname)
    find_program(${varname} ${execname})
    if(NOT ${varname})
        message(FATAL_ERROR "${execname} not found")
    endif()
endmacro()

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARCH LLVM_MINGW_PREFIX)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED LLVM_MINGW_PREFIX)
    set(LLVM_MINGW_PREFIX "aarch64-w64-mingw32-" CACHE STRING "LLVM-MinGW tool prefix")
endif()

require_program(CMAKE_C_COMPILER ${LLVM_MINGW_PREFIX}gcc)
require_program(CMAKE_ASM_COMPILER ${LLVM_MINGW_PREFIX}gcc)

set(_REACTOS_ARM64_CXX_WRAPPER "/home/ispardoa/git/toolchains/reactos-arm64-bin/${LLVM_MINGW_PREFIX}g++")
if(EXISTS "${_REACTOS_ARM64_CXX_WRAPPER}")
    set(CMAKE_CXX_COMPILER "${_REACTOS_ARM64_CXX_WRAPPER}" CACHE FILEPATH "C++ compiler" FORCE)
else()
    require_program(CMAKE_CXX_COMPILER ${LLVM_MINGW_PREFIX}g++)
endif()

set(CMAKE_ASM_COMPILER_ID "GNU")
set(CMAKE_C_COMPILER_TARGET aarch64-w64-mingw32)
set(CMAKE_CXX_COMPILER_TARGET aarch64-w64-mingw32)

get_filename_component(_LLVM_MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(_LLVM_MINGW_ROOT_DIR "${_LLVM_MINGW_BIN_DIR}" DIRECTORY)
set(_LLVM_MINGW_LIBCXX_INCLUDE "${_LLVM_MINGW_ROOT_DIR}/aarch64-w64-mingw32/include/c++/v1")
set(LLVM_MINGW_LIBCXX_INCLUDE "${_LLVM_MINGW_LIBCXX_INCLUDE}" CACHE PATH "LLVM-MinGW libc++ include directory")
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" --print-resource-dir
    OUTPUT_VARIABLE _LLVM_MINGW_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(LLVM_MINGW_CLANG_INCLUDE "${_LLVM_MINGW_CLANG_RESOURCE_DIR}/include" CACHE PATH "LLVM-MinGW clang builtin include directory")
set(CMAKE_CXX_FLAGS "" CACHE STRING "C++ flags" FORCE)

require_program(CMAKE_MC_COMPILER ${LLVM_MINGW_PREFIX}windmc)
require_program(CMAKE_RC_COMPILER ${LLVM_MINGW_PREFIX}windres)
require_program(CMAKE_DLLTOOL ${LLVM_MINGW_PREFIX}dlltool)
require_program(CMAKE_AR ${LLVM_MINGW_PREFIX}ar)
require_program(CMAKE_RANLIB ${LLVM_MINGW_PREFIX}ranlib)
require_program(CMAKE_OBJCOPY ${LLVM_MINGW_PREFIX}objcopy)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE FILEPATH "C compiler launcher" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "" CACHE FILEPATH "C++ compiler launcher" FORCE)
    set(CMAKE_ASM_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE FILEPATH "ASM compiler launcher" FORCE)
endif()

set(CMAKE_C_CREATE_STATIC_LIBRARY "<CMAKE_AR> crT <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_CREATE_STATIC_LIBRARY ${CMAKE_C_CREATE_STATIC_LIBRARY})
set(CMAKE_ASM_CREATE_STATIC_LIBRARY ${CMAKE_C_CREATE_STATIC_LIBRARY})

set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "Standard C Libraries")
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "Standard C++ Libraries")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_SHARED_LINKER_FLAGS_INIT "-nostdlib -Wl,--enable-auto-image-base,--disable-auto-import")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-nostdlib -Wl,--enable-auto-image-base,--disable-auto-import")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -Wl,--enable-auto-image-base,--disable-auto-import")

set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/overrides-gcc.cmake")

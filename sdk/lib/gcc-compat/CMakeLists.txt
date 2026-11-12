
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/lib/crt/include
    ${REACTOS_SOURCE_DIR}/sdk/include/c++)

list(APPEND SOURCE
    __mingw_vsprintf.c
    __throw_out_of_range_fmt.cpp
    StringCchCopyNA.c)

if(DLL_EXPORT_VERSION LESS 0x600)
    list(APPEND SOURCE
        fseeki64_compat.c
        mbsrtowcs_compat.c
        rand_s.c
        snprintf_compat.c
        strnlen_compat.c
        wchar_compat.c)
endif()

add_library(stdc++compat ${SOURCE})
set_target_cpp_properties(stdc++compat WITH_EXCEPTIONS)
set_target_properties(stdc++compat PROPERTIES USE_SYSTEM_CXX_HEADERS TRUE)
add_dependencies(stdc++compat xdk)
target_link_libraries(stdc++compat PRIVATE cxx_system_includes)
target_compile_definitions(stdc++compat PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:PAL_STDCPP_COMPAT>" _CRT_DECLARE_NONSTDC_NAMES=1)

# Stack protector / fortify / msvcrt-snprintf compat for MSYS2 toolchains.
# Only linked when gcc.cmake detects that libsupc++ needs these symbols;
# MSYS2's libwinpthread additionally references __ms_snprintf.
add_library(gcc_ssp_compat gcc_ssp_compat.c ms_snprintf_compat.c)
add_dependencies(gcc_ssp_compat xdk)

# fegetenv/fesetenv for the UCRT64 libwinpthread (14.0.0.r598+), which
# carries the floating point environment over to new threads. Only linked
# when gcc.cmake finds libwinpthread importing them.
if(ARCH STREQUAL "amd64")
    add_library(fenv_compat fenv_compat.c)
    add_dependencies(fenv_compat xdk)
endif()

if(ARCH STREQUAL "i386" AND _winpthread_syms MATCHES " U _?fegetenv")
    # Keep MinGW's C99 implementation, but use ReactOS's CRT reset routine.
    # CRT_fp*.o bundles its default environment with a conflicting _fpreset.
    execute_process(COMMAND ${CMAKE_AR} t "${LIBMINGWEX_LOCATION}"
        OUTPUT_VARIABLE _mingwex_members RESULT_VARIABLE _mingwex_status)
    if(NOT _mingwex_status EQUAL 0)
        message(FATAL_ERROR "Could not read MinGW's C99 archive")
    endif()
    string(REPLACE "\r" "" _mingwex_members "${_mingwex_members}")
    string(REPLACE "\n" ";" _mingwex_members "${_mingwex_members}")
    list(FILTER _mingwex_members INCLUDE REGEX "(^|-)CRT_fp(4|8|10)[.]o$")
    if(NOT _mingwex_members)
        message(FATAL_ERROR "MinGW's C99 archive has no recognized CRT reset member")
    endif()
    add_library(mingw_fpreset_compat OBJECT mingw_fpreset_compat.c)
    set(_reactos_mingwex "${CMAKE_CURRENT_BINARY_DIR}/libmingwex-reactos.a")
    add_custom_command(OUTPUT "${_reactos_mingwex}"
        COMMAND ${CMAKE_COMMAND} -E copy "${LIBMINGWEX_LOCATION}" "${_reactos_mingwex}"
        COMMAND ${CMAKE_AR} d "${_reactos_mingwex}" ${_mingwex_members}
        COMMAND ${CMAKE_AR} rs "${_reactos_mingwex}" $<TARGET_OBJECTS:mingw_fpreset_compat>
        DEPENDS "${LIBMINGWEX_LOCATION}" mingw_fpreset_compat $<TARGET_OBJECTS:mingw_fpreset_compat>
        COMMENT "Using ReactOS CRT defaults with the MinGW32 C99 library"
        VERBATIM)
    add_custom_target(mingwex-reactos DEPENDS "${_reactos_mingwex}")
    set_target_properties(libmingwex PROPERTIES IMPORTED_LOCATION "${_reactos_mingwex}")
    add_dependencies(libmingwex mingwex-reactos)
endif()

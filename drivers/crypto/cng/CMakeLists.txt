
spec2def(cng.sys cng.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    algorithm.c
    cipher.c
    cng.c
    hash.c
    key.c
    random.c
    cngp.h)

add_library(cng MODULE
    ${SOURCE}
    symcrypt_env.c
    cng.rc
    ${CMAKE_CURRENT_BINARY_DIR}/cng.def)

if(NOT MSVC)
    # SymCrypt's internal header declares static functions it does not define
    set_source_files_properties(symcrypt_env.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
endif()

set_module_type(cng kernelmodedriver)
target_link_libraries(cng symcrypt memcmp chkstk)
add_importlibs(cng ntoskrnl hal)
add_pch(cng cngp.h SOURCE)
add_cd_file(TARGET cng DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(cng_reg.inf)

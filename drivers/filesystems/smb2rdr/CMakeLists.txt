
list(APPEND SOURCE
    smb2rdr.c
    upcall.c
    smb2rdr.h
    upcall.h)

add_definitions(-DRDBSS_TRACKER)

add_library(smb2rdr MODULE ${SOURCE} smb2rdr.rc)
set_module_type(smb2rdr kernelmodedriver)
target_link_libraries(smb2rdr ntoskrnl_vista rdbsslib rxce copysup memcmp ${PSEH_LIB})
add_importlibs(smb2rdr ntoskrnl hal)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(smb2rdr PRIVATE "-Wno-switch")
endif()

add_cd_file(TARGET smb2rdr DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(smb2rdr_reg.inf)


list(APPEND SOURCE
    atom.c
    radeon.c
    radeon_atombios.c
    radeon_bios.c
    radeon_modeset.c
    radeon.h)

add_library(radeonfb MODULE ${SOURCE} radeonfb.rc)
set_module_type(radeonfb kernelmodedriver)
add_importlibs(radeonfb ntoskrnl videoprt)
target_link_libraries(radeonfb libcntpr)
add_cd_file(TARGET radeonfb DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(radeonfb radeonfb.inf)

if(NOT MSVC)
    # atom.c is imported near-verbatim from the Linux radeon driver
    target_compile_options(radeonfb PRIVATE -Wno-unused-function -Wno-pointer-arith)
endif()

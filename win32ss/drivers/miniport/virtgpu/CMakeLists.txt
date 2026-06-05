include_directories(BEFORE
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/virtio)

list(APPEND SOURCE
    virtgpu.c
    virtgpu.h
    virtgpu_shared.h)

add_library(virtgpu MODULE ${SOURCE} virtgpu.rc)
set_module_type(virtgpu kernelmodedriver)
add_importlibs(virtgpu ntoskrnl videoprt hal)
target_link_libraries(virtgpu virtio libcntpr)
add_pch(virtgpu virtgpu.h SOURCE)
add_cd_file(TARGET virtgpu DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(virtgpu_reg.inf)
add_driver_inf(virtgpu virtgpu.inf)

list(APPEND DISPLAY_SOURCE
    display.c
    display.h
    virtgpu_shared.h)

add_library(virtgpu_disp MODULE ${DISPLAY_SOURCE})
# The miniport (virtgpu.sys) and the display driver (virtgpu.dll) intentionally
# share the "virtgpu" output name, but every MSVC link PDB lands in the shared
# msvc_pdb directory named after OUTPUT_NAME - so both would write virtgpu.pdb
# concurrently and the linker fails with LNK1201. Give the display PDB its own
# name; the shipped binary stays virtgpu.dll.
set_target_properties(virtgpu_disp PROPERTIES OUTPUT_NAME "virtgpu" PDB_NAME "virtgpu_disp")
set_module_type(virtgpu_disp kerneldll ENTRYPOINT DrvEnableDriver 12)
target_link_libraries(virtgpu_disp libcntpr)
add_importlibs(virtgpu_disp win32k)
add_dependencies(virtgpu_disp psdk)
add_pch(virtgpu_disp display.h DISPLAY_SOURCE)
add_cd_file(TARGET virtgpu_disp DESTINATION reactos/system32 FOR all)

spec2def(virtgpuogl.dll virtgpuogl.spec)
add_library(virtgpuogl MODULE
    virtgpuogl.c
    virtgpu_shared.h
    ${CMAKE_CURRENT_BINARY_DIR}/virtgpuogl.def)
set_module_type(virtgpuogl win32dll ENTRYPOINT DllMain 12)
add_importlibs(virtgpuogl gdi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET virtgpuogl DESTINATION reactos/system32 FOR all)

if(NOT MSVC)
    target_compile_options(virtgpu PRIVATE
        -Wno-pointer-to-int-cast
        -Wno-int-to-pointer-cast
        -Wno-unused-function)
    target_compile_options(virtgpu_disp PRIVATE
        -Wno-unused-function)
    target_compile_options(virtgpuogl PRIVATE
        -Wno-unused-function)
endif()

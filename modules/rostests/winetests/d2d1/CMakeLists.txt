
# WIDL_C_INLINE_WRAPPERS: several D2D1 methods return an aggregate by value.
# Without it widl emits a poisoned macro that only exists to produce an
# "undeclared identifier ..._define_WIDL_C_INLINE_WRAPPERS_for_aggregate_return_support"
# diagnostic, which is exactly what this test used to fail with under GCC.
add_definitions(-DUSE_WINE_TODOS -D_USE_MATH_DEFINES -DWINETEST_USE_DBGSTR_LONGLONG
                -DWIDL_C_INLINE_WRAPPERS)
add_executable(d2d1_winetest d2d1.c testlist.c)
# The D2D1/D3D10/D3D11/DXGI interface and effect GUIDs live in dx10guid, not dxguid.
target_link_libraries(d2d1_winetest uuid dxguid dx10guid)
set_module_type(d2d1_winetest win32cui)

if(NOT MSVC)
    # Imported Wine test sources; i386 builds with -Werror and these
    # two are Wine's own style, not something to "fix" in a copy we
    # want to keep diffable against upstream.
    target_compile_options(d2d1_winetest PRIVATE
        -Wno-error=discarded-qualifiers -Wno-error=sequence-point)
endif()

add_importlibs(d2d1_winetest d2d1 dwrite d3d10_1 d3d11 d3dcompiler_47 ddraw d3drm
                             ole32 gdi32 advapi32 user32 msvcrt kernel32)

if(MSVC)
    add_importlibs(d2d1_winetest ntdll)
endif()

add_rostests_file(TARGET d2d1_winetest)

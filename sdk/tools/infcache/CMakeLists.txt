
add_host_tool(infcache infcache.c)

if(NOT MSVC)
    target_compile_options(infcache PRIVATE -fshort-wchar)
endif()

target_link_libraries(infcache PRIVATE host_includes inflibhost unicode)

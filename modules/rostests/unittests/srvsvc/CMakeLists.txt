
add_definitions(-DWINETEST_WINE_DBG_STUBS)

include_directories(
    ${REACTOS_SOURCE_DIR}/base/services/srvsvc
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${REACTOS_SOURCE_DIR}/modules/rostests/apitests/include)

add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/srvsvc.idl)

list(APPEND SOURCE
    ${REACTOS_SOURCE_DIR}/base/services/srvsvc/rpcserver.c
    serverinfo.c
    testlist.c
    ${CMAKE_CURRENT_BINARY_DIR}/srvsvc_s.c)

add_executable(srvsvc_unittest ${SOURCE})
set_module_type(srvsvc_unittest win32cui)
add_importlibs(srvsvc_unittest advapi32 rpcrt4 msvcrt kernel32 ntdll)
add_rostests_file(TARGET srvsvc_unittest)

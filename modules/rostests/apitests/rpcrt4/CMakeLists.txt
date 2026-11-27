
include_directories(${CMAKE_CURRENT_BINARY_DIR})
set(IDL_FLAGS ${IDL_FLAGS} --prefix-server=s_)
add_rpc_files(client waitlisten.idl)
add_rpc_files(server waitlisten.idl)

list(APPEND SOURCE
    RpcMgmtWaitServerListen.c
    testlist.c
    ${CMAKE_CURRENT_BINARY_DIR}/waitlisten_c.c
    ${CMAKE_CURRENT_BINARY_DIR}/waitlisten_s.c)

add_executable(rpcrt4_apitest ${SOURCE})
target_link_libraries(rpcrt4_apitest ${PSEH_LIB})
set_module_type(rpcrt4_apitest win32cui)
add_importlibs(rpcrt4_apitest rpcrt4 msvcrt kernel32)
add_rostests_file(TARGET rpcrt4_apitest)

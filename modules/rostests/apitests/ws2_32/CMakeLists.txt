
list(APPEND SOURCE
    accept.c
    acceptexcancel.c
    bind.c
    broadcast.c
    cancelrace.c
    close.c
    closeaddr.c
    closerace.c
    duprace.c
    getaddrinfo.c
    gethostname.c
    getnameinfo.c
    getservbyname.c
    getservbyport.c
    helpers.c
    ioctlsocket.c
    nonblocking.c
    nostartup.c
    open_osfhandle.c
    recv.c
    recvtimeout.c
    selectclose.c
    send.c
    sendbound.c
    sendflood.c
    streamclose.c
    WSAAsync.c
    WSAIoctl.c
    WSARecv.c
    WSAStartup.c)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(ws2_32_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

target_link_libraries(ws2_32_apitest wine ${PSEH_LIB})
set_module_type(ws2_32_apitest win32cui)
add_importlibs(ws2_32_apitest advapi32 iphlpapi ws2_32 msvcrt kernel32 ntdll)
add_pch(ws2_32_apitest ws2_32.h "${PCH_SKIP_SOURCE}")
add_rostests_file(TARGET ws2_32_apitest)

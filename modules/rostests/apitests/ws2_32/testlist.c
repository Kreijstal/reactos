
#define STANDALONE
#include <apitest.h>

extern void func_accept(void);
extern void func_acceptexcancel(void);
extern void func_bind(void);
extern void func_broadcast(void);
extern void func_cancelrace(void);
extern void func_close(void);
extern void func_closeaddr(void);
extern void func_closerace(void);
extern void func_duprace(void);
extern void func_getaddrinfo(void);
extern void func_gethostname(void);
extern void func_getnameinfo(void);
extern void func_getservbyname(void);
extern void func_getservbyport(void);
extern void func_ioctlsocket(void);
extern void func_nonblocking(void);
extern void func_nostartup(void);
extern void func_open_osfhandle(void);
extern void func_recv(void);
extern void func_recvtimeout(void);
extern void func_selectclose(void);
extern void func_send(void);
extern void func_sendbound(void);
extern void func_sendflood(void);
extern void func_streamclose(void);
extern void func_WSAAsync(void);
extern void func_WSAIoctl(void);
extern void func_WSARecv(void);
extern void func_WSAStartup(void);

const struct test winetest_testlist[] =
{
    { "accept", func_accept },
    { "acceptexcancel", func_acceptexcancel },
    { "bind", func_bind },
    { "broadcast", func_broadcast },
    { "cancelrace", func_cancelrace },
    { "close", func_close },
    { "closeaddr", func_closeaddr },
    { "closerace", func_closerace },
    { "duprace", func_duprace },
    { "getaddrinfo", func_getaddrinfo },
    { "gethostname", func_gethostname },
    { "getnameinfo", func_getnameinfo },
    { "getservbyname", func_getservbyname },
    { "getservbyport", func_getservbyport },
    { "ioctlsocket", func_ioctlsocket },
    { "nonblocking", func_nonblocking },
    { "nostartup", func_nostartup },
    { "open_osfhandle", func_open_osfhandle },
    { "recv", func_recv },
    { "recvtimeout", func_recvtimeout },
    { "selectclose", func_selectclose },
    { "send", func_send },
    { "sendbound", func_sendbound },
    { "sendflood", func_sendflood },
    { "streamclose", func_streamclose },
    { "WSAAsync", func_WSAAsync },
    { "WSAIoctl", func_WSAIoctl },
    { "WSARecv", func_WSARecv },
    { "WSAStartup", func_WSAStartup },
    { 0, 0 }
};


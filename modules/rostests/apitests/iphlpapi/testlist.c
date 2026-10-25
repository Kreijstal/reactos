
#define STANDALONE
#include <apitest.h>

extern void func_GetAdaptersAddresses(void);
extern void func_GetExtendedTcpTable(void);
extern void func_GetExtendedUdpTable(void);
extern void func_GetInterfaceName(void);
extern void func_GetNetworkParams(void);
extern void func_GetOwnerModuleFromTcpEntry(void);
extern void func_GetOwnerModuleFromUdpEntry(void);
extern void func_icmp(void);
extern void func_IpHelperV2(void);
extern void func_SendARP(void);

const struct test winetest_testlist[] =
{
    { "GetAdaptersAddresses",       func_GetAdaptersAddresses },
    { "GetExtendedTcpTable",        func_GetExtendedTcpTable },
    { "GetExtendedUdpTable",        func_GetExtendedUdpTable },
    { "GetInterfaceName",           func_GetInterfaceName },
    { "GetNetworkParams",           func_GetNetworkParams },
    { "GetOwnerModuleFromTcpEntry", func_GetOwnerModuleFromTcpEntry },
    { "GetOwnerModuleFromUdpEntry", func_GetOwnerModuleFromUdpEntry },
    { "icmp",                       func_icmp },
    { "IpHelperV2",                 func_IpHelperV2 },
    { "SendARP",                    func_SendARP },

    { 0, 0 }
};


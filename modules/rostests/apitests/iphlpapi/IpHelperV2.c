/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the IPv4 side of the IP Helper v2 interface and route functions
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <stdio.h>

#define TEST_ROUTE_DEST 0x010200C0UL /* 192.0.2.1, network order (TEST-NET-1) */

static PMIB_IPADDRTABLE
GetAddrTable(VOID)
{
    PMIB_IPADDRTABLE Table = NULL;
    ULONG Size = 0;

    if (GetIpAddrTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return NULL;
    Table = HeapAlloc(GetProcessHeap(), 0, Size);
    if (Table && GetIpAddrTable(Table, &Size, FALSE) != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, Table);
        Table = NULL;
    }
    return Table;
}

static VOID
TestIpInterfaceEntry(VOID)
{
    PMIB_IPADDRTABLE Table;
    MIB_IPINTERFACE_ROW Row, Row2;
    MIB_IFROW IfRow;
    NET_LUID Luid;
    DWORD Error, i;

    InitializeIpInterfaceEntry(&Row);
    ok_int(Row.Family, AF_UNSPEC);
    ok_int(Row.InterfaceIndex, 0);
    ok(Row.InterfaceLuid.Value == 0, "Luid %I64x\n", Row.InterfaceLuid.Value);

    /* The family is required */
    Row.InterfaceIndex = 1;
    ok_long(GetIpInterfaceEntry(&Row), ERROR_INVALID_PARAMETER);
    ok_long(GetIpInterfaceEntry(NULL), ERROR_INVALID_PARAMETER);

    InitializeIpInterfaceEntry(&Row);
    Row.Family = AF_INET;
    Row.InterfaceIndex = 0x7FFF0000;
    ok_long(GetIpInterfaceEntry(&Row), ERROR_NOT_FOUND);

    Table = GetAddrTable();
    ok(Table != NULL, "No address table\n");
    if (!Table)
        return;

    for (i = 0; i < Table->dwNumEntries; i++)
    {
        IfRow.dwIndex = Table->table[i].dwIndex;
        if (GetIfEntry(&IfRow) != NO_ERROR)
            continue;

        InitializeIpInterfaceEntry(&Row);
        Row.Family = AF_INET;
        Row.InterfaceIndex = Table->table[i].dwIndex;
        Error = GetIpInterfaceEntry(&Row);
        ok_long(Error, NO_ERROR);
        if (Error != NO_ERROR)
            continue;

        ok_int(Row.Family, AF_INET);
        ok_long(Row.InterfaceIndex, Table->table[i].dwIndex);
        ok_long(Row.NlMtu, IfRow.dwMtu);
        ok(ConvertInterfaceIndexToLuid(Row.InterfaceIndex, &Luid) == NO_ERROR &&
           Luid.Value == Row.InterfaceLuid.Value,
           "Luid %I64x, expected %I64x\n", Row.InterfaceLuid.Value, Luid.Value);

        /* The LUID alone finds the row as well */
        InitializeIpInterfaceEntry(&Row2);
        Row2.Family = AF_INET;
        Row2.InterfaceLuid = Row.InterfaceLuid;
        ok_long(GetIpInterfaceEntry(&Row2), NO_ERROR);
        ok_long(Row2.InterfaceIndex, Row.InterfaceIndex);

        /* Writing back what was read changes nothing and works */
        ok_long(SetIpInterfaceEntry(&Row), NO_ERROR);
        ok_long(GetIpInterfaceEntry(&Row2), NO_ERROR);
        ok_long(Row2.NlMtu, Row.NlMtu);
    }

    HeapFree(GetProcessHeap(), 0, Table);
}

static VOID
TestGetAdapterIndex(VOID)
{
    PIP_ADAPTER_INFO Info, Adapter;
    ULONG Size = 0, Index;
    WCHAR Name[MAX_ADAPTER_NAME_LENGTH + 32];

    ok_long(GetAdapterIndex(NULL, &Index), ERROR_INVALID_PARAMETER);

    if (GetAdaptersInfo(NULL, &Size) != ERROR_BUFFER_OVERFLOW)
    {
        skip("No adapters\n");
        return;
    }
    Info = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Info || GetAdaptersInfo(Info, &Size) != NO_ERROR)
    {
        skip("GetAdaptersInfo failed\n");
        HeapFree(GetProcessHeap(), 0, Info);
        return;
    }

    for (Adapter = Info; Adapter; Adapter = Adapter->Next)
    {
        _snwprintf(Name, ARRAYSIZE(Name), L"\\DEVICE\\TCPIP_%S", Adapter->AdapterName);
        Index = 0xdeadbeef;
        ok_long(GetAdapterIndex(Name, &Index), NO_ERROR);
        ok_long(Index, Adapter->Index);
    }

    HeapFree(GetProcessHeap(), 0, Info);
}

static VOID
TestRoutes(VOID)
{
    MIB_IPFORWARDROW Route;
    MIB_IPFORWARD_ROW2 Row, Found;
    SOCKADDR_INET Dest, Source;
    struct sockaddr_in Sin;
    DWORD Index, Error;

    /* Where 127.0.0.1 and a global address go, against the v1 functions */
    RtlZeroMemory(&Dest, sizeof(Dest));
    Dest.si_family = AF_INET;
    Dest.Ipv4.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    ok_long(GetBestRoute2(NULL, 0, NULL, &Dest, 0, &Row, &Source), NO_ERROR);
    ok_long(GetBestRoute(Dest.Ipv4.sin_addr.S_un.S_addr, 0, &Route), NO_ERROR);
    ok_long(Row.InterfaceIndex, Route.dwForwardIfIndex);

    Dest.Ipv4.sin_addr.S_un.S_addr = inet_addr("8.8.8.8");
    if (GetBestRoute(Dest.Ipv4.sin_addr.S_un.S_addr, 0, &Route) != NO_ERROR || Route.dwForwardDest != 0)
    {
        skip("No default route\n");
        return;
    }
    Error = GetBestRoute2(NULL, 0, NULL, &Dest, 0, &Row, &Source);
    ok_long(Error, NO_ERROR);
    ok_long(Row.InterfaceIndex, Route.dwForwardIfIndex);
    ok_int(Row.DestinationPrefix.Prefix.si_family, AF_INET);
    ok_int(Row.DestinationPrefix.PrefixLength, 0);
    ok_int(Row.NextHop.si_family, AF_INET);
    ok_long(Row.NextHop.Ipv4.sin_addr.S_un.S_addr, Route.dwForwardNextHop);
    ok_int(Source.si_family, AF_INET);
    ok(Source.Ipv4.sin_addr.S_un.S_addr != 0, "No source address\n");

    /* The same, for that interface */
    ok_long(GetBestRoute2(NULL, Row.InterfaceIndex, NULL, &Dest, 0, &Found, &Source), NO_ERROR);
    ok_long(Found.InterfaceIndex, Row.InterfaceIndex);

    RtlZeroMemory(&Sin, sizeof(Sin));
    Sin.sin_family = AF_INET;
    Sin.sin_addr.S_un.S_addr = Dest.Ipv4.sin_addr.S_un.S_addr;
    Index = 0xdeadbeef;
    ok_long(GetBestInterfaceEx((struct sockaddr *)&Sin, &Index), NO_ERROR);
    ok_long(Index, Route.dwForwardIfIndex);

    /* The default route itself */
    Found = Row;
    ok_long(GetIpForwardEntry2(&Found), NO_ERROR);
    ok_long(Found.NextHop.Ipv4.sin_addr.S_un.S_addr, Row.NextHop.Ipv4.sin_addr.S_un.S_addr);

    /* An on-link host route on that interface: add, find, add again, delete */
    RtlZeroMemory(&Row, sizeof(Row));
    Row.InterfaceIndex = Route.dwForwardIfIndex;
    Row.DestinationPrefix.Prefix.si_family = AF_INET;
    Row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = TEST_ROUTE_DEST;
    Row.DestinationPrefix.PrefixLength = 32;
    Row.NextHop.si_family = AF_INET;
    Row.ValidLifetime = 0xFFFFFFFF;
    Row.PreferredLifetime = 0xFFFFFFFF;
    Row.Metric = 1;
    Row.Protocol = MIB_IPPROTO_NETMGMT;

    Found = Row;
    ok_long(GetIpForwardEntry2(&Found), ERROR_NOT_FOUND);
    Error = CreateIpForwardEntry2(&Row);
    ok_long(Error, NO_ERROR);
    if (Error != NO_ERROR)
        return;
    Found = Row;
    ok_long(GetIpForwardEntry2(&Found), NO_ERROR);
    ok_int(Found.DestinationPrefix.PrefixLength, 32);
    ok_long(GetBestRoute(TEST_ROUTE_DEST, 0, &Route), NO_ERROR);
    ok_long(Route.dwForwardDest, TEST_ROUTE_DEST);
    ok_long(CreateIpForwardEntry2(&Row), ERROR_OBJECT_ALREADY_EXISTS);
    ok_long(DeleteIpForwardEntry2(&Row), NO_ERROR);
    Found = Row;
    ok_long(GetIpForwardEntry2(&Found), ERROR_NOT_FOUND);
    ok_long(DeleteIpForwardEntry2(&Row), ERROR_NOT_FOUND);
}

START_TEST(IpHelperV2)
{
    TestIpInterfaceEntry();
    TestGetAdapterIndex();
    TestRoutes();
}

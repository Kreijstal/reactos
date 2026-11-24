/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that DeleteIpForwardEntry deletes exactly the route it names
 */

#include <apitest.h>
#include <winsock2.h>
#include <iphlpapi.h>

/* Network order: 198.51.100.0 (TEST-NET-2) and 203.0.113.0 (TEST-NET-3) */
#define TEST_NET_2 0x006433C6UL
#define TEST_NET_3 0x007100CBUL
#define MASK_24    0x00FFFFFFUL
#define MASK_25    0x80FFFFFFUL

static
BOOL
FindRoute(DWORD Dest, DWORD Mask, DWORD NextHop, DWORD IfIndex)
{
    PMIB_IPFORWARDTABLE Table;
    ULONG Size = 0;
    BOOL Found = FALSE;
    DWORD i;

    if (GetIpForwardTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    Table = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table)
        return FALSE;
    if (GetIpForwardTable(Table, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < Table->dwNumEntries && !Found; i++)
        {
            Found = (Table->table[i].dwForwardDest == Dest &&
                     Table->table[i].dwForwardMask == Mask &&
                     Table->table[i].dwForwardNextHop == NextHop &&
                     Table->table[i].dwForwardIfIndex == IfIndex);
        }
    }
    HeapFree(GetProcessHeap(), 0, Table);
    return Found;
}

static
void
InitRoute(PMIB_IPFORWARDROW Route, DWORD Dest, DWORD Mask, DWORD NextHop, DWORD IfIndex)
{
    ZeroMemory(Route, sizeof(*Route));
    Route->dwForwardDest = Dest;
    Route->dwForwardMask = Mask;
    Route->dwForwardNextHop = NextHop;
    Route->dwForwardIfIndex = IfIndex;
    Route->dwForwardType = MIB_IPROUTE_TYPE_INDIRECT;
    Route->dwForwardProto = MIB_IPPROTO_NETMGMT;
    Route->dwForwardMetric1 = 1;
    Route->dwForwardMetric2 = Route->dwForwardMetric3 = (DWORD)-1;
    Route->dwForwardMetric4 = Route->dwForwardMetric5 = (DWORD)-1;
}

/* Another interface with an address on the gateway's subnet */
static
BOOL
FindSecondInterface(DWORD Gateway, DWORD FirstIfIndex, DWORD *IfIndex)
{
    PMIB_IPADDRTABLE Table;
    ULONG Size = 0;
    BOOL Found = FALSE;
    DWORD i;

    if (GetIpAddrTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    Table = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table)
        return FALSE;
    if (GetIpAddrTable(Table, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < Table->dwNumEntries && !Found; i++)
        {
            if (Table->table[i].dwIndex != FirstIfIndex &&
                Table->table[i].dwAddr != 0 &&
                Table->table[i].dwMask != 0 &&
                (Table->table[i].dwAddr & Table->table[i].dwMask) == (Gateway & Table->table[i].dwMask))
            {
                *IfIndex = Table->table[i].dwIndex;
                Found = TRUE;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, Table);
    return Found;
}

START_TEST(IpForwardEntry)
{
    MIB_IPFORWARDROW Default, Route24, Route25, RouteIf1, RouteIf2;
    DWORD Gateway, IfIndex, IfIndex2, Error;

    if (GetBestRoute(inet_addr("8.8.8.8"), 0, &Default) != NO_ERROR || Default.dwForwardDest != 0)
    {
        skip("No default route\n");
        return;
    }
    Gateway = Default.dwForwardNextHop;
    IfIndex = Default.dwForwardIfIndex;

    /* Two routes to one network through one gateway, told apart by the mask:
     * deleting the /25 must keep the /24 */
    InitRoute(&Route24, TEST_NET_2, MASK_24, Gateway, IfIndex);
    InitRoute(&Route25, TEST_NET_2, MASK_25, Gateway, IfIndex);
    Error = CreateIpForwardEntry(&Route24);
    ok(Error == NO_ERROR, "CreateIpForwardEntry(/24) failed: %lu\n", Error);
    Error = CreateIpForwardEntry(&Route25);
    ok(Error == NO_ERROR, "CreateIpForwardEntry(/25) failed: %lu\n", Error);
    ok(FindRoute(TEST_NET_2, MASK_24, Gateway, IfIndex), "The /24 route is missing\n");
    ok(FindRoute(TEST_NET_2, MASK_25, Gateway, IfIndex), "The /25 route is missing\n");

    Error = DeleteIpForwardEntry(&Route25);
    ok(Error == NO_ERROR, "DeleteIpForwardEntry(/25) failed: %lu\n", Error);
    ok(!FindRoute(TEST_NET_2, MASK_25, Gateway, IfIndex), "The /25 route was not deleted\n");
    ok(FindRoute(TEST_NET_2, MASK_24, Gateway, IfIndex), "Deleting the /25 route deleted the /24 route\n");

    DeleteIpForwardEntry(&Route24);
    DeleteIpForwardEntry(&Route25);
    ok(!FindRoute(TEST_NET_2, MASK_24, Gateway, IfIndex), "The /24 route was not deleted\n");

    /* Two interfaces on the gateway's subnet each route through it: deleting
     * the route of the second interface must keep that of the first */
    if (!FindSecondInterface(Gateway, IfIndex, &IfIndex2))
    {
        skip("No second interface on the gateway's subnet\n");
        return;
    }

    InitRoute(&RouteIf1, TEST_NET_3, MASK_24, Gateway, IfIndex);
    InitRoute(&RouteIf2, TEST_NET_3, MASK_24, Gateway, IfIndex2);
    Error = CreateIpForwardEntry(&RouteIf1);
    ok(Error == NO_ERROR, "CreateIpForwardEntry(first interface) failed: %lu\n", Error);
    Error = CreateIpForwardEntry(&RouteIf2);
    ok(Error == NO_ERROR, "CreateIpForwardEntry(second interface) failed: %lu\n", Error);
    ok(FindRoute(TEST_NET_3, MASK_24, Gateway, IfIndex), "The route of the first interface is missing\n");
    ok(FindRoute(TEST_NET_3, MASK_24, Gateway, IfIndex2), "The route of the second interface is missing\n");

    Error = DeleteIpForwardEntry(&RouteIf2);
    ok(Error == NO_ERROR, "DeleteIpForwardEntry(second interface) failed: %lu\n", Error);
    ok(!FindRoute(TEST_NET_3, MASK_24, Gateway, IfIndex2), "The route of the second interface was not deleted\n");
    ok(FindRoute(TEST_NET_3, MASK_24, Gateway, IfIndex), "Deleting the route of the second interface deleted that of the first\n");

    DeleteIpForwardEntry(&RouteIf1);
    DeleteIpForwardEntry(&RouteIf2);
    ok(!FindRoute(TEST_NET_3, MASK_24, Gateway, IfIndex), "The route of the first interface was not deleted\n");
}

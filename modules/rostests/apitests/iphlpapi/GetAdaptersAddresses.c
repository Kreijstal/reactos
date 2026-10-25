/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for GetAdaptersAddresses
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <stdio.h>

/* Each adapter carries the LUID that its index and its GUID convert to and from */
START_TEST(GetAdaptersAddresses)
{
    PIP_ADAPTER_ADDRESSES Addresses, Adapter;
    ULONG Size = 0, Count = 0;
    NET_IFINDEX Index;
    NET_LUID Luid;
    GUID Guid;
    CHAR GuidString[64];

    ok_long(GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &Size), ERROR_BUFFER_OVERFLOW);
    Addresses = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Addresses)
    {
        skip("Out of memory\n");
        return;
    }
    ok_long(GetAdaptersAddresses(AF_INET, 0, NULL, Addresses, &Size), NO_ERROR);
    for (Adapter = Addresses; Adapter; Adapter = Adapter->Next)
    {
        Count++;
        ok(Adapter->Luid.Value != 0, "Adapter %s has no LUID\n", Adapter->AdapterName);
        ok(Adapter->Luid.Info.IfType == Adapter->IfType, "Adapter %s: LUID type %u, interface type %lu\n",
           Adapter->AdapterName, (UINT)Adapter->Luid.Info.IfType, Adapter->IfType);
        Luid.Value = 0;
        ok(ConvertInterfaceIndexToLuid(Adapter->IfIndex, &Luid) == NO_ERROR &&
           Luid.Value == Adapter->Luid.Value,
           "Adapter %s: LUID %I64x, expected %I64x\n", Adapter->AdapterName, Adapter->Luid.Value, Luid.Value);

        /* And back */
        Index = 0;
        ok(ConvertInterfaceLuidToIndex(&Adapter->Luid, &Index) == NO_ERROR && Index == Adapter->IfIndex,
           "Adapter %s: index %lu, expected %lu\n", Adapter->AdapterName, Index, Adapter->IfIndex);
        if (Adapter->AdapterName[0] == '{')
        {
            ZeroMemory(&Guid, sizeof(Guid));
            ok(ConvertInterfaceLuidToGuid(&Adapter->Luid, &Guid) == NO_ERROR,
               "Adapter %s: no GUID\n", Adapter->AdapterName);
            sprintf(GuidString, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                    Guid.Data1, Guid.Data2, Guid.Data3, Guid.Data4[0], Guid.Data4[1], Guid.Data4[2],
                    Guid.Data4[3], Guid.Data4[4], Guid.Data4[5], Guid.Data4[6], Guid.Data4[7]);
            ok(!_stricmp(GuidString, Adapter->AdapterName), "GUID %s, expected %s\n", GuidString, Adapter->AdapterName);
        }
    }
    ok(Count > 0, "No adapter\n");
    HeapFree(GetProcessHeap(), 0, Addresses);
}

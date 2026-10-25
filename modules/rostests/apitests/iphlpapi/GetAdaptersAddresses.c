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

/* Each adapter carries the LUID that its index converts to */
START_TEST(GetAdaptersAddresses)
{
    PIP_ADAPTER_ADDRESSES Addresses, Adapter;
    ULONG Size = 0, Count = 0;
    NET_LUID Luid;

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
    }
    ok(Count > 0, "No adapter\n");
    HeapFree(GetProcessHeap(), 0, Addresses);
}

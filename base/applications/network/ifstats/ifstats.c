/* ifstats: per-interface counters from GetIfTable, for the AR9485 bring-up. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winioctl.h>
#include <ntddndis.h>

#ifndef OID_GEN_STATISTICS
#define OID_GEN_STATISTICS 0x00020106
#endif
#ifndef IOCTL_NDIS_QUERY_GLOBAL_STATS
#define IOCTL_NDIS_QUERY_GLOBAL_STATS CTL_CODE(FILE_DEVICE_PHYSICAL_NETCARD, 0, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#endif

static void QueryMiniport(const char *guid)
{
    WCHAR path[128];
    HANDLE h;
    NDIS_OID oid;
    DWORD got;
    ULONGLONG buf[64];
    ULONG v;

    _snwprintf(path, 128, L"\\\\?\\GLOBALROOT\\Device\\%S", guid);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { wprintf(L"   miniport %s: open failed %lu\n", path, GetLastError()); return; }
    oid = OID_GEN_XMIT_OK; v = 0;
    if (DeviceIoControl(h, IOCTL_NDIS_QUERY_GLOBAL_STATS, &oid, sizeof(oid), &v, sizeof(v), &got, NULL))
        wprintf(L"   OID_GEN_XMIT_OK %lu", v); else wprintf(L"   XMIT_OK err %lu", GetLastError());
    oid = OID_GEN_RCV_OK; v = 0;
    if (DeviceIoControl(h, IOCTL_NDIS_QUERY_GLOBAL_STATS, &oid, sizeof(oid), &v, sizeof(v), &got, NULL))
        wprintf(L"  RCV_OK %lu", v); else wprintf(L"  RCV_OK err %lu", GetLastError());
    oid = OID_GEN_RCV_ERROR; v = 0;
    if (DeviceIoControl(h, IOCTL_NDIS_QUERY_GLOBAL_STATS, &oid, sizeof(oid), &v, sizeof(v), &got, NULL))
        wprintf(L"  RCV_ERROR %lu", v); else wprintf(L"  RCV_ERROR err %lu", GetLastError());
    oid = OID_GEN_STATISTICS; memset(buf, 0, sizeof(buf));
    if (DeviceIoControl(h, IOCTL_NDIS_QUERY_GLOBAL_STATS, &oid, sizeof(oid), buf, sizeof(buf), &got, NULL))
    {
        /* NDIS_OBJECT_HEADER(4) SupportedStatistics(4) then ULONG64:
           InDiscards InErrors InOctets InUcast InMcast InBcast OutOctets OutUcast OutMcast OutBcast OutErrors OutDiscards */
        wprintf(L"\n   GEN_STATISTICS flags 0x%lx InDisc %I64u InErr %I64u InOct %I64u InUc %I64u InMc %I64u InBc %I64u "
                L"OutOct %I64u OutUc %I64u OutMc %I64u OutBc %I64u OutErr %I64u OutDisc %I64u",
                *(ULONG *)((PUCHAR)buf + 4), buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], buf[12]);
    }
    else wprintf(L"  GEN_STATISTICS err %lu", GetLastError());
    wprintf(L"\n");
    CloseHandle(h);
}

int wmain(void)
{
    PMIB_IFTABLE t = NULL;
    ULONG size = 0, i;
    if (GetIfTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return 1;
    t = malloc(size);
    if (GetIfTable(t, &size, FALSE) != NO_ERROR) return 2;
    for (i = 0; i < t->dwNumEntries; i++)
    {
        MIB_IFROW *r = &t->table[i];
        wprintf(L"idx %lu type %lu oper %lu adm %lu mtu %lu speed %lu %S\n"
                L"   in : oct %lu ucast %lu nucast %lu disc %lu err %lu unk %lu\n"
                L"   out: oct %lu ucast %lu nucast %lu disc %lu err %lu\n",
                r->dwIndex, r->dwType, r->dwOperStatus, r->dwAdminStatus, r->dwMtu, r->dwSpeed, r->bDescr,
                r->dwInOctets, r->dwInUcastPkts, r->dwInNUcastPkts, r->dwInDiscards, r->dwInErrors, r->dwInUnknownProtos,
                r->dwOutOctets, r->dwOutUcastPkts, r->dwOutNUcastPkts, r->dwOutDiscards, r->dwOutErrors);
        if (r->bDescr[0] == '{')
            QueryMiniport((const char *)r->bDescr);
    }
    return 0;
}

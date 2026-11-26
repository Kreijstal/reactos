/*
 * PROJECT:     ReactOS IP Helper
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IP interface change subscriptions over the legacy IP tables
 */

#include "iphlpapi_private.h"

#if DLL_EXPORT_VERSION >= 0x600

typedef struct _INTERFACE_STATE
{
    NET_LUID Luid;
    DWORD Index, Type, Mtu, AdminStatus, OperStatus, Addresses;
} INTERFACE_STATE;

typedef struct _INTERFACE_SNAPSHOT
{
    DWORD Count;
    INTERFACE_STATE Rows[ANYSIZE_ARRAY];
} INTERFACE_SNAPSHOT;

typedef struct _INTERFACE_NOTIFICATION
{
    LIST_ENTRY Link;
    PIPINTERFACE_CHANGE_CALLBACK Callback;
    PVOID Context;
    HANDLE StopEvent, Thread;
    DWORD ThreadId;
    BOOLEAN Initial;
    HMODULE Module;
    INTERFACE_SNAPSHOT *Snapshot;
} INTERFACE_NOTIFICATION;

static SRWLOCK NotificationLock = SRWLOCK_INIT;
static LIST_ENTRY Notifications = { &Notifications, &Notifications };

static DWORD QueryLegacyTable(BOOL Addresses, PVOID *Table)
{
    DWORD Size = 0, Error;
    PVOID Buffer;
    ULONG Attempt;

    *Table = NULL;
    Error = Addresses ? GetIpAddrTable(NULL, &Size, FALSE) : GetIfTable(NULL, &Size, FALSE);
    if (Error != NO_ERROR && Error != ERROR_INSUFFICIENT_BUFFER && Error != ERROR_NO_DATA)
        return Error;
    if (Size < sizeof(DWORD)) Size = sizeof(DWORD);
    for (Attempt = 0; Attempt < 3; ++Attempt)
    {
        Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
        if (!Buffer) return ERROR_NOT_ENOUGH_MEMORY;
        Error = Addresses ? GetIpAddrTable(Buffer, &Size, FALSE) : GetIfTable(Buffer, &Size, FALSE);
        if (Error == NO_ERROR || Error == ERROR_NO_DATA)
        {
            *Table = Buffer;
            return NO_ERROR;
        }
        HeapFree(GetProcessHeap(), 0, Buffer);
        if (Error != ERROR_INSUFFICIENT_BUFFER) return Error;
    }
    return ERROR_RETRY;
}

static DWORD CaptureInterfaces(INTERFACE_SNAPSHOT **Snapshot)
{
    PMIB_IFTABLE Interfaces;
    PMIB_IPADDRTABLE Addresses;
    INTERFACE_SNAPSHOT *Result;
    DWORD Error, i, j;
    SIZE_T Bytes;

    *Snapshot = NULL;
    Error = QueryLegacyTable(FALSE, (PVOID *)&Interfaces);
    if (Error) return Error;
    Error = QueryLegacyTable(TRUE, (PVOID *)&Addresses);
    if (Error)
    {
        HeapFree(GetProcessHeap(), 0, Interfaces);
        return Error;
    }
    Bytes = FIELD_OFFSET(INTERFACE_SNAPSHOT, Rows) +
            (SIZE_T)Interfaces->dwNumEntries * sizeof(INTERFACE_STATE);
    Result = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Bytes);
    if (!Result)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Done;
    }
    Result->Count = Interfaces->dwNumEntries;
    for (i = 0; i < Result->Count; ++i)
    {
        INTERFACE_STATE *Row = &Result->Rows[i];
        MIB_IFROW *Source = &Interfaces->table[i];
        Row->Index = Source->dwIndex;
        Row->Type = Source->dwType;
        Row->Mtu = Source->dwMtu;
        Row->AdminStatus = Source->dwAdminStatus;
        Row->OperStatus = Source->dwOperStatus;
        ConvertInterfaceIndexToLuid(Row->Index, &Row->Luid);
        for (j = 0; j < Addresses->dwNumEntries; ++j)
            if (Addresses->table[j].dwIndex == Row->Index)
                Row->Addresses ^= Addresses->table[j].dwAddr * 16777619u ^ Addresses->table[j].dwMask;
    }
    *Snapshot = Result;
Done:
    HeapFree(GetProcessHeap(), 0, Addresses);
    HeapFree(GetProcessHeap(), 0, Interfaces);
    return Error;
}

static INTERFACE_STATE *FindInterface(INTERFACE_SNAPSHOT *Snapshot, DWORD Index)
{
    DWORD i;
    for (i = 0; i < Snapshot->Count; ++i)
        if (Snapshot->Rows[i].Index == Index) return &Snapshot->Rows[i];
    return NULL;
}

DWORD WINAPI GetIpInterfaceTable(ADDRESS_FAMILY Family, PMIB_IPINTERFACE_TABLE *Table)
{
    INTERFACE_SNAPSHOT *Snapshot;
    PMIB_IPINTERFACE_TABLE Result;
    DWORD Error, i;

    if (!Table) return ERROR_INVALID_PARAMETER;
    *Table = NULL;
    if (Family != AF_INET && Family != AF_UNSPEC && Family != AF_INET6)
        return ERROR_INVALID_PARAMETER;
    if (Family == AF_INET6) return ERROR_NOT_SUPPORTED;
    Error = CaptureInterfaces(&Snapshot);
    if (Error) return Error;
    Result = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                        FIELD_OFFSET(MIB_IPINTERFACE_TABLE, Table) +
                        (SIZE_T)Snapshot->Count * sizeof(MIB_IPINTERFACE_ROW));
    if (!Result) { HeapFree(GetProcessHeap(), 0, Snapshot); return ERROR_NOT_ENOUGH_MEMORY; }
    for (i = 0; i < Snapshot->Count; ++i)
    {
        PMIB_IPINTERFACE_ROW Row = &Result->Table[Result->NumEntries];
        Row->Family = AF_INET;
        Row->InterfaceIndex = Snapshot->Rows[i].Index;
        Error = GetIpInterfaceEntry(Row);
        if (Error == NO_ERROR) Result->NumEntries++;
        else if (Error != ERROR_NOT_FOUND)
        {
            HeapFree(GetProcessHeap(), 0, Result);
            HeapFree(GetProcessHeap(), 0, Snapshot);
            return Error;
        }
    }
    HeapFree(GetProcessHeap(), 0, Snapshot);
    *Table = Result;
    return NO_ERROR;
}

VOID WINAPI FreeMibTable(PVOID Table)
{
    if (Table) HeapFree(GetProcessHeap(), 0, Table);
}

static VOID ReportInterface(INTERFACE_NOTIFICATION *Notification,
                            const INTERFACE_STATE *State, MIB_NOTIFICATION_TYPE Type)
{
    MIB_IPINTERFACE_ROW Row;
    if (WaitForSingleObject(Notification->StopEvent, 0) == WAIT_OBJECT_0) return;
    ZeroMemory(&Row, sizeof(Row));
    Row.Family = AF_INET;
    Row.InterfaceLuid = State->Luid;
    Row.InterfaceIndex = State->Index;
    Notification->Callback(Notification->Context, &Row, Type);
}

static DWORD WINAPI InterfaceNotificationThread(PVOID Parameter)
{
    INTERFACE_NOTIFICATION *Notification = Parameter;
    INTERFACE_SNAPSHOT *Next;
    INTERFACE_STATE *Old;
    DWORD i;
    HMODULE Module = Notification->Module;

    if (Notification->Initial && WaitForSingleObject(Notification->StopEvent, 0) != WAIT_OBJECT_0)
        Notification->Callback(Notification->Context, NULL, MibInitialNotification);

    /* NotifyAddrChange is still a non-completing stub. Poll only configuration
     * fields, never traffic counters, until the driver exposes a change IRP. */
    while (WaitForSingleObject(Notification->StopEvent, 1000) == WAIT_TIMEOUT)
    {
        if (CaptureInterfaces(&Next) != NO_ERROR) continue;
        for (i = 0; i < Next->Count; ++i)
        {
            INTERFACE_STATE *Current = &Next->Rows[i];
            Old = FindInterface(Notification->Snapshot, Current->Index);
            if (!Old)
                ReportInterface(Notification, Current, MibAddInstance);
            else if (Old->Luid.Value != Current->Luid.Value || Old->Type != Current->Type ||
                     Old->Mtu != Current->Mtu || Old->AdminStatus != Current->AdminStatus ||
                     Old->OperStatus != Current->OperStatus || Old->Addresses != Current->Addresses)
                ReportInterface(Notification, Current, MibParameterNotification);
        }
        for (i = 0; i < Notification->Snapshot->Count; ++i)
            if (!FindInterface(Next, Notification->Snapshot->Rows[i].Index))
                ReportInterface(Notification, &Notification->Snapshot->Rows[i], MibDeleteInstance);
        HeapFree(GetProcessHeap(), 0, Notification->Snapshot);
        Notification->Snapshot = Next;
    }
    HeapFree(GetProcessHeap(), 0, Notification->Snapshot);
    FreeLibraryAndExitThread(Module, 0);
}

DWORD WINAPI NotifyIpInterfaceChange(ADDRESS_FAMILY Family,
                                    PIPINTERFACE_CHANGE_CALLBACK Callback,
                                    PVOID CallerContext, BOOLEAN InitialNotification,
                                    HANDLE *NotificationHandle)
{
    INTERFACE_NOTIFICATION *Notification;
    DWORD Error;

    if (!NotificationHandle) return ERROR_INVALID_PARAMETER;
    *NotificationHandle = NULL;
    if (!Callback || (Family != AF_UNSPEC && Family != AF_INET && Family != AF_INET6))
        return ERROR_INVALID_PARAMETER;
    if (Family == AF_INET6) return ERROR_NOT_SUPPORTED;
    Notification = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Notification));
    if (!Notification) return ERROR_NOT_ENOUGH_MEMORY;
    Error = CaptureInterfaces(&Notification->Snapshot);
    if (Error) goto Fail;
    Notification->Callback = Callback;
    Notification->Context = CallerContext;
    Notification->Initial = InitialNotification;
    Notification->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Notification->StopEvent) { Error = GetLastError(); goto Fail; }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)InterfaceNotificationThread, &Notification->Module))
    {
        Error = GetLastError();
        goto Fail;
    }
    Notification->Thread = CreateThread(NULL, 0, InterfaceNotificationThread,
                                         Notification, CREATE_SUSPENDED, &Notification->ThreadId);
    if (!Notification->Thread) { Error = GetLastError(); goto Fail; }
    AcquireSRWLockExclusive(&NotificationLock);
    InsertTailList(&Notifications, &Notification->Link);
    ReleaseSRWLockExclusive(&NotificationLock);
    *NotificationHandle = Notification;
    ResumeThread(Notification->Thread);
    return NO_ERROR;
Fail:
    if (Notification->Module) FreeLibrary(Notification->Module);
    if (Notification->StopEvent) CloseHandle(Notification->StopEvent);
    if (Notification->Snapshot) HeapFree(GetProcessHeap(), 0, Notification->Snapshot);
    HeapFree(GetProcessHeap(), 0, Notification);
    return Error;
}

DWORD WINAPI CancelMibChangeNotify2(HANDLE NotificationHandle)
{
    INTERFACE_NOTIFICATION *Notification = NULL;
    LIST_ENTRY *Link;

    AcquireSRWLockExclusive(&NotificationLock);
    for (Link = Notifications.Flink; Link != &Notifications; Link = Link->Flink)
    {
        INTERFACE_NOTIFICATION *Current = CONTAINING_RECORD(Link, INTERFACE_NOTIFICATION, Link);
        if ((HANDLE)Current != NotificationHandle) continue;
        if (Current->ThreadId == GetCurrentThreadId())
        {
            ReleaseSRWLockExclusive(&NotificationLock);
            return ERROR_INVALID_PARAMETER;
        }
        Notification = Current;
        RemoveEntryList(Link);
        break;
    }
    ReleaseSRWLockExclusive(&NotificationLock);
    if (!Notification) return ERROR_INVALID_PARAMETER;
    SetEvent(Notification->StopEvent);
    WaitForSingleObject(Notification->Thread, INFINITE);
    CloseHandle(Notification->Thread);
    CloseHandle(Notification->StopEvent);
    HeapFree(GetProcessHeap(), 0, Notification);
    return NO_ERROR;
}

#endif

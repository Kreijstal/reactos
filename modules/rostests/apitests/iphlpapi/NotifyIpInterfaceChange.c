/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Interface notification callback and cancellation contracts
 */

#include <apitest.h>

#if (DLL_EXPORT_VERSION >= 0x600)

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

typedef struct _CALLBACK_STATE
{
    HANDLE Event;
    LONG Count, Initial, Invalid;
} CALLBACK_STATE;

static VOID WINAPI InterfaceChanged(PVOID Context, PMIB_IPINTERFACE_ROW Row, MIB_NOTIFICATION_TYPE Type)
{
    CALLBACK_STATE *State = Context;
    InterlockedIncrement(&State->Count);
    if (Type == MibInitialNotification)
    {
        if (Row) InterlockedExchange(&State->Invalid, 1);
        InterlockedIncrement(&State->Initial);
        SetEvent(State->Event);
    }
    else if (!Row || Row->Family != AF_INET || (!Row->InterfaceIndex && !Row->InterfaceLuid.Value))
        InterlockedExchange(&State->Invalid, 1);
}

START_TEST(NotifyIpInterfaceChange)
{
    DWORD (WINAPI *Notify)(ADDRESS_FAMILY, PIPINTERFACE_CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE *);
    DWORD (WINAPI *Cancel)(HANDLE);
    DWORD (WINAPI *GetTable)(ADDRESS_FAMILY, PMIB_IPINTERFACE_TABLE *);
    VOID (WINAPI *FreeTable)(PVOID);
    HMODULE Module = LoadLibraryW(L"iphlpapi.dll");
    CALLBACK_STATE State = {0};
    HANDLE Handle = (HANDLE)1;
    DWORD Error, Wait;
    LONG Count;
    WCHAR ModulePath[MAX_PATH];
    PMIB_IPINTERFACE_TABLE Table = NULL;
    ULONG i;

    ok(Module != NULL, "Could not load IP Helper\n");
    if (!Module) return;
    GetModuleFileNameW(Module, ModulePath, ARRAYSIZE(ModulePath));
    trace("IP Helper module: %S\n", ModulePath);
    Notify = (void *)GetProcAddress(Module, "NotifyIpInterfaceChange");
    Cancel = (void *)GetProcAddress(Module, "CancelMibChangeNotify2");
    GetTable = (void *)GetProcAddress(Module, "GetIpInterfaceTable");
    FreeTable = (void *)GetProcAddress(Module, "FreeMibTable");
    if (GetTable && FreeTable)
    {
        Error = GetTable(AF_INET, &Table);
        ok(Error == NO_ERROR && Table != NULL, "Interface enumeration failed: %lu\n", Error);
        if (Table)
        {
            for (i = 0; i < Table->NumEntries; ++i)
                ok(Table->Table[i].Family == AF_INET && Table->Table[i].InterfaceIndex != 0,
                   "Invalid interface row %lu\n", i);
            FreeTable(Table);
        }
        Table = NULL;
        Error = GetTable(0xffff, &Table);
        ok(Error == ERROR_INVALID_PARAMETER, "Invalid enumeration family accepted: %lu\n", Error);
        ok(Table == NULL, "Invalid enumeration returned a table\n");
    }
    if (!Notify || !Cancel)
    {
        win_skip("Interface notification APIs unavailable\n");
        FreeLibrary(Module);
        return;
    }
    State.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(State.Event != NULL, "Event creation failed\n");
    if (!State.Event) { FreeLibrary(Module); return; }
    Error = Notify(0xffff, InterfaceChanged, &State, TRUE, &Handle);
    ok(Error == ERROR_INVALID_PARAMETER, "Invalid family accepted: %lu\n", Error);
    ok(Handle == NULL, "Error left a notification handle\n");
    Error = Cancel(NULL);
    trace("NULL cancellation returned %lu\n", Error);
    ok(Error != NO_ERROR, "NULL cancellation accepted\n");
    Error = Notify(AF_INET, InterfaceChanged, &State, TRUE, &Handle);
    trace("Registration returned %lu, handle %p\n", Error, Handle);
    ok(Error == NO_ERROR && Handle != NULL, "Registration failed: %lu\n", Error);
    if (Error == NO_ERROR)
    {
        Wait = WaitForSingleObject(State.Event, 5000);
        ok(Wait == WAIT_OBJECT_0, "Initial callback did not arrive: %lu\n", Wait);
        ok(Cancel(Handle) == NO_ERROR, "Cancellation failed\n");
        ok(State.Initial == 1 && !State.Invalid, "Malformed initial notification\n");
        Count = State.Count;
        Sleep(1200);
        ok(State.Count == Count, "Callback ran after cancellation\n");
    }
    State.Initial = State.Count = State.Invalid = 0;
    Error = Notify(AF_INET, InterfaceChanged, &State, FALSE, &Handle);
    ok(Error == NO_ERROR && Handle != NULL, "Registration without initial callback failed: %lu\n", Error);
    if (Error == NO_ERROR)
    {
        ok(Cancel(Handle) == NO_ERROR, "Immediate cancellation failed\n");
        ok(State.Initial == 0, "Unrequested initial callback\n");
    }
    CloseHandle(State.Event);
    FreeLibrary(Module);
}

#else

START_TEST(NotifyIpInterfaceChange)
{
    skip("NotifyIpInterfaceChange requires NT 6.0 or later\n");
}

#endif

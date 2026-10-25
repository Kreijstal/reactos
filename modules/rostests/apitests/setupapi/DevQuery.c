/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the device query functions
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <devquery.h>
#include <initguid.h>
#include <devpkey.h>
#include <ntddstor.h>

/* Windows exports these from cfgmgr32 only */
static HRESULT (WINAPI *pDevCreateObjectQuery)(DEV_OBJECT_TYPE, ULONG, ULONG, const DEVPROPCOMPKEY *,
                                               ULONG, const DEVPROP_FILTER_EXPRESSION *,
                                               PDEV_QUERY_RESULT_CALLBACK, PVOID, PHDEVQUERY);
static VOID (WINAPI *pDevCloseObjectQuery)(HDEVQUERY);
static HRESULT (WINAPI *pDevGetObjects)(DEV_OBJECT_TYPE, ULONG, ULONG, const DEVPROPCOMPKEY *,
                                        ULONG, const DEVPROP_FILTER_EXPRESSION *,
                                        PULONG, const DEV_OBJECT **);
static VOID (WINAPI *pDevFreeObjects)(ULONG, const DEV_OBJECT *);

static const DEVPROP_BOOLEAN True = DEVPROP_TRUE;

static const DEVPROPCOMPKEY Requested[] =
{
    { { { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256 }, DEVPROP_STORE_SYSTEM, NULL },
    { { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 3 }, DEVPROP_STORE_SYSTEM, NULL },
    { { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 4 }, DEVPROP_STORE_SYSTEM, NULL },
};

static
VOID
SetFilter(
    _Out_ DEVPROP_FILTER_EXPRESSION *Filter,
    _In_ DEVPROP_OPERATOR Operator,
    _In_ const DEVPROPKEY *Key,
    _In_ DEVPROPTYPE Type,
    _In_ const VOID *Buffer,
    _In_ ULONG BufferSize)
{
    ZeroMemory(Filter, sizeof(*Filter));
    Filter->Operator = Operator;
    Filter->Property.CompKey.Key = *Key;
    Filter->Property.CompKey.Store = DEVPROP_STORE_SYSTEM;
    Filter->Property.Type = Type;
    Filter->Property.Buffer = (PVOID)Buffer;
    Filter->Property.BufferSize = BufferSize;
}

static
const DEVPROPERTY *
FindProperty(
    _In_ const DEV_OBJECT *Object,
    _In_ const DEVPROPKEY *Key)
{
    ULONG i;

    for (i = 0; i < Object->cPropertyCount; i++)
    {
        if (IsEqualDevPropKey(Object->pProperties[i].CompKey.Key, *Key))
            return &Object->pProperties[i];
    }
    return NULL;
}

static
BOOL
MultiSzContains(
    _In_ PCWSTR List,
    _In_ PCWSTR String)
{
    for (; *List; List += wcslen(List) + 1)
    {
        if (!_wcsicmp(List, String))
            return TRUE;
    }
    return FALSE;
}

/* The enabled disk interfaces, of one device or of all */
static
PWSTR
GetDiskInterfaces(
    _In_opt_ PCWSTR InstanceId,
    _Out_ PULONG Count)
{
    ULONG Length = 0;
    PWSTR List, p;

    *Count = 0;
    if (CM_Get_Device_Interface_List_SizeW(&Length, (LPGUID)&GUID_DEVINTERFACE_DISK, (DEVINSTID_W)InstanceId,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return NULL;
    List = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (Length + 1) * sizeof(WCHAR));
    if (!List)
        return NULL;
    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_DISK, (DEVINSTID_W)InstanceId, List, Length + 1,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, List);
        return NULL;
    }
    for (p = List; *p; p += wcslen(p) + 1)
        (*Count)++;
    return List;
}

typedef struct _QUERY_CTX
{
    HANDLE Done;               /* set once enumerated or aborted */
    HANDLE ClosedEvent;        /* set on an asynchronous close */
    BOOL CloseInCallback;      /* close the query once enumerated */
    LONG Calls;
    LONG Adds;
    LONG AddsBeforeCompletion;
    LONG Completed;
    LONG Closed;
    LONG Aborted;
    LONG Other;
    LONG BadObjects;
    LONG Handles;              /* how many distinct handles the callback saw */
    HDEVQUERY Handle;
    WCHAR FirstId[512];
} QUERY_CTX, *PQUERY_CTX;

static
VOID
WINAPI
QueryCallback(
    _In_ HDEVQUERY hDevQuery,
    _In_opt_ PVOID pContext,
    _In_ const DEV_QUERY_RESULT_ACTION_DATA *pActionData)
{
    PQUERY_CTX Ctx = pContext;

    InterlockedIncrement(&Ctx->Calls);
    if (Ctx->Handle != hDevQuery)
    {
        Ctx->Handle = hDevQuery;
        InterlockedIncrement(&Ctx->Handles);
    }

    switch (pActionData->Action)
    {
        case DevQueryResultAdd:
            if (pActionData->Data.DeviceObject.ObjectType != DevObjectTypeDeviceInterface ||
                !pActionData->Data.DeviceObject.pszObjectId)
            {
                InterlockedIncrement(&Ctx->BadObjects);
            }
            else if (InterlockedIncrement(&Ctx->Adds) == 1)
            {
                lstrcpynW(Ctx->FirstId, pActionData->Data.DeviceObject.pszObjectId, ARRAYSIZE(Ctx->FirstId));
            }
            if (!Ctx->Completed)
                InterlockedIncrement(&Ctx->AddsBeforeCompletion);
            break;

        case DevQueryResultStateChange:
            if (pActionData->Data.State == DevQueryStateEnumCompleted)
            {
                InterlockedIncrement(&Ctx->Completed);
                if (Ctx->CloseInCallback)
                    pDevCloseObjectQuery(hDevQuery);
            }
            else if (pActionData->Data.State == DevQueryStateClosed)
            {
                InterlockedIncrement(&Ctx->Closed);
                SetEvent(Ctx->ClosedEvent);
                break;
            }
            else if (pActionData->Data.State == DevQueryStateAborted)
                InterlockedIncrement(&Ctx->Aborted);
            else
                InterlockedIncrement(&Ctx->Other);
            SetEvent(Ctx->Done);
            break;

        default:
            InterlockedIncrement(&Ctx->Other);
            break;
    }
}

static
VOID
TestArguments(VOID)
{
    DEVPROP_FILTER_EXPRESSION Filter;
    QUERY_CTX Ctx = { 0 };
    HDEVQUERY Query;
    const DEV_OBJECT *Objects;
    ULONG Count;
    HRESULT hr;

    /* Windows checks neither the handle pointer nor DevGetObjects' output pointers */

    hr = pDevCreateObjectQuery(DevObjectTypeDeviceInterface, 0, 0, NULL, 0, NULL, NULL, &Ctx, &Query);
    ok(hr == E_INVALIDARG, "No callback: 0x%lx\n", hr);

    hr = pDevCreateObjectQuery(DevObjectTypeDeviceInterface, 0, 0, NULL, 1, NULL, QueryCallback, &Ctx, &Query);
    ok(hr == E_INVALIDARG, "No filter: 0x%lx\n", hr);

    hr = pDevCreateObjectQuery(DevObjectTypeDeviceInterface, 0, 1, NULL, 0, NULL, QueryCallback, &Ctx, &Query);
    ok(hr == E_INVALIDARG, "No requested properties: 0x%lx\n", hr);

    hr = pDevCreateObjectQuery(DevObjectTypeDeviceInterface, 0x80000000, 0, NULL, 0, NULL, QueryCallback, &Ctx, &Query);
    ok(hr == E_INVALIDARG, "Unknown flag: 0x%lx\n", hr);

    /* A logical operator without its partner */
    SetFilter(&Filter, DEVPROP_OPERATOR_AND_OPEN, &DEVPKEY_DeviceInterface_Enabled, DEVPROP_TYPE_EMPTY, NULL, 0);
    hr = pDevGetObjects(DevObjectTypeDeviceInterface, 0, 0, NULL, 1, &Filter, &Count, &Objects);
    ok(FAILED(hr), "Unbalanced filter: 0x%lx\n", hr);
    if (SUCCEEDED(hr))
        pDevFreeObjects(Count, Objects);

    ok(Ctx.Calls == 0, "The callback ran %ld times\n", Ctx.Calls);
}

static
VOID
TestGetObjects(
    _Out_writes_(InstanceIdLength) PWSTR InstanceId,
    _In_ ULONG InstanceIdLength)
{
    DEVPROP_FILTER_EXPRESSION Filter[3];
    const DEV_OBJECT *Objects;
    const DEVPROPERTY *Property;
    ULONG Count, Expected, ForDevice, i;
    PWSTR List, DeviceList;
    WCHAR Lower[MAX_DEVICE_ID_LEN];
    HRESULT hr;

    InstanceId[0] = UNICODE_NULL;

    List = GetDiskInterfaces(NULL, &Expected);
    if (!List)
    {
        skip("Cannot list the disk interfaces\n");
        return;
    }

    SetFilter(&Filter[0], DEVPROP_OPERATOR_EQUALS, &DEVPKEY_DeviceInterface_ClassGuid,
              DEVPROP_TYPE_GUID, &GUID_DEVINTERFACE_DISK, sizeof(GUID));
    SetFilter(&Filter[1], DEVPROP_OPERATOR_EQUALS, &DEVPKEY_DeviceInterface_Enabled,
              DEVPROP_TYPE_BOOLEAN, &True, sizeof(True));
    hr = pDevGetObjects(DevObjectTypeDeviceInterface, 0, ARRAYSIZE(Requested), Requested,
                        2, Filter, &Count, &Objects);
    ok(hr == S_OK, "DevGetObjects failed: 0x%lx\n", hr);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, List);
        return;
    }
    ok(Count == Expected, "Got %lu disk interfaces, expected %lu\n", Count, Expected);

    for (i = 0; i < Count; i++)
    {
        winetest_push_context("%lu", i);
        ok(Objects[i].ObjectType == DevObjectTypeDeviceInterface, "Object type %d\n", Objects[i].ObjectType);
        ok(Objects[i].pszObjectId && MultiSzContains(List, Objects[i].pszObjectId),
           "Unexpected interface %S\n", Objects[i].pszObjectId);
        ok(Objects[i].cPropertyCount == ARRAYSIZE(Requested), "%lu properties\n", Objects[i].cPropertyCount);

        Property = FindProperty(&Objects[i], &DEVPKEY_DeviceInterface_Enabled);
        ok(Property && Property->Type == DEVPROP_TYPE_BOOLEAN && Property->BufferSize == sizeof(DEVPROP_BOOLEAN) &&
           *(DEVPROP_BOOLEAN *)Property->Buffer == DEVPROP_TRUE, "Interface not enabled\n");
        Property = FindProperty(&Objects[i], &DEVPKEY_DeviceInterface_ClassGuid);
        ok(Property && Property->Type == DEVPROP_TYPE_GUID && Property->BufferSize == sizeof(GUID) &&
           IsEqualGUID(Property->Buffer, &GUID_DEVINTERFACE_DISK), "Wrong interface class\n");
        Property = FindProperty(&Objects[i], &DEVPKEY_Device_InstanceId);
        ok(Property && Property->Type == DEVPROP_TYPE_STRING && Property->Buffer, "No instance id\n");
        if (Property && Property->Type == DEVPROP_TYPE_STRING && Property->Buffer)
        {
            /* The interface belongs to that device */
            DeviceList = GetDiskInterfaces(Property->Buffer, &ForDevice);
            ok(DeviceList && MultiSzContains(DeviceList, Objects[i].pszObjectId),
               "%S is not an interface of %S\n", Objects[i].pszObjectId, (PCWSTR)Property->Buffer);
            HeapFree(GetProcessHeap(), 0, DeviceList);
            if (i == 0)
                lstrcpynW(InstanceId, Property->Buffer, InstanceIdLength);
        }
        winetest_pop_context();
    }
    pDevFreeObjects(Count, Objects);
    HeapFree(GetProcessHeap(), 0, List);

    if (!InstanceId[0])
        return;

    /* Filter by the device, ignoring the case, as Wintun does */
    lstrcpynW(Lower, InstanceId, ARRAYSIZE(Lower));
    CharLowerW(Lower);
    SetFilter(&Filter[2], DEVPROP_OPERATOR_EQUALS_IGNORE_CASE, &DEVPKEY_Device_InstanceId,
              DEVPROP_TYPE_STRING, Lower, (ULONG)((wcslen(Lower) + 1) * sizeof(WCHAR)));
    DeviceList = GetDiskInterfaces(InstanceId, &ForDevice);
    hr = pDevGetObjects(DevObjectTypeDeviceInterface, 0, 0, NULL, 3, Filter, &Count, &Objects);
    ok(hr == S_OK, "DevGetObjects failed: 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(Count == ForDevice, "Got %lu interfaces of %S, expected %lu\n", Count, InstanceId, ForDevice);
        for (i = 0; i < Count; i++)
        {
            ok(DeviceList && MultiSzContains(DeviceList, Objects[i].pszObjectId),
               "%S is not an interface of %S\n", Objects[i].pszObjectId, InstanceId);
            ok(Objects[i].cPropertyCount == 0, "%lu properties\n", Objects[i].cPropertyCount);
        }
        pDevFreeObjects(Count, Objects);
    }
    HeapFree(GetProcessHeap(), 0, DeviceList);

    /* Case matters without IGNORE_CASE, if the id has letters */
    if (wcscmp(Lower, InstanceId))
    {
        Filter[2].Operator = DEVPROP_OPERATOR_EQUALS;
        hr = pDevGetObjects(DevObjectTypeDeviceInterface, 0, 0, NULL, 3, Filter, &Count, &Objects);
        ok(hr == S_OK, "DevGetObjects failed: 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            ok(Count == 0, "Got %lu interfaces\n", Count);
            pDevFreeObjects(Count, Objects);
        }
    }

    /* A device that does not exist */
    SetFilter(&Filter[2], DEVPROP_OPERATOR_EQUALS_IGNORE_CASE, &DEVPKEY_Device_InstanceId,
              DEVPROP_TYPE_STRING, L"ROOT\\APITEST_NO_SUCH_DEVICE\\0000", sizeof(L"ROOT\\APITEST_NO_SUCH_DEVICE\\0000"));
    hr = pDevGetObjects(DevObjectTypeDeviceInterface, 0, 0, NULL, 3, Filter, &Count, &Objects);
    ok(hr == S_OK, "DevGetObjects failed: 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(Count == 0, "Got %lu interfaces\n", Count);
        pDevFreeObjects(Count, Objects);
    }

    /* The interfaces of a device that are not enabled */
    SetFilter(&Filter[1], DEVPROP_OPERATOR_NOT_EQUALS, &DEVPKEY_DeviceInterface_Enabled,
              DEVPROP_TYPE_BOOLEAN, &True, sizeof(True));
    SetFilter(&Filter[2], DEVPROP_OPERATOR_EQUALS_IGNORE_CASE, &DEVPKEY_Device_InstanceId,
              DEVPROP_TYPE_STRING, Lower, (ULONG)((wcslen(Lower) + 1) * sizeof(WCHAR)));
    hr = pDevGetObjects(DevObjectTypeDeviceInterface, 0, 0, NULL, 3, Filter, &Count, &Objects);
    ok(hr == S_OK, "DevGetObjects failed: 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        DeviceList = GetDiskInterfaces(InstanceId, &ForDevice);
        for (i = 0; i < Count; i++)
        {
            ok(!DeviceList || !MultiSzContains(DeviceList, Objects[i].pszObjectId),
               "%S is enabled\n", Objects[i].pszObjectId);
        }
        HeapFree(GetProcessHeap(), 0, DeviceList);
        pDevFreeObjects(Count, Objects);
    }
}

/* The filter Wintun waits for its adapter with */
static
VOID
TestQuery(
    _In_ PCWSTR InstanceId,
    _In_ ULONG QueryFlags,
    _In_ BOOL CloseInCallback)
{
    DEVPROP_FILTER_EXPRESSION Filter[3];
    QUERY_CTX Ctx = { 0 };
    HDEVQUERY Query = NULL;
    ULONG Expected;
    PWSTR List;
    LONG Calls;
    HRESULT hr;
    DWORD Wait;

    winetest_push_context("flags 0x%lx%s", QueryFlags, CloseInCallback ? " closed in callback" : "");

    List = GetDiskInterfaces(InstanceId, &Expected);
    SetFilter(&Filter[0], DEVPROP_OPERATOR_EQUALS_IGNORE_CASE, &DEVPKEY_Device_InstanceId,
              DEVPROP_TYPE_STRING, InstanceId, (ULONG)((wcslen(InstanceId) + 1) * sizeof(WCHAR)));
    SetFilter(&Filter[1], DEVPROP_OPERATOR_EQUALS, &DEVPKEY_DeviceInterface_Enabled,
              DEVPROP_TYPE_BOOLEAN, &True, sizeof(True));
    SetFilter(&Filter[2], DEVPROP_OPERATOR_EQUALS, &DEVPKEY_DeviceInterface_ClassGuid,
              DEVPROP_TYPE_GUID, &GUID_DEVINTERFACE_DISK, sizeof(GUID));

    Ctx.Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    Ctx.ClosedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    Ctx.CloseInCallback = CloseInCallback;
    hr = pDevCreateObjectQuery(DevObjectTypeDeviceInterface, QueryFlags, 0, NULL, ARRAYSIZE(Filter), Filter,
                               QueryCallback, &Ctx, &Query);
    ok(hr == S_OK, "DevCreateObjectQuery failed: 0x%lx\n", hr);
    if (FAILED(hr))
    {
        CloseHandle(Ctx.ClosedEvent);
        CloseHandle(Ctx.Done);
        HeapFree(GetProcessHeap(), 0, List);
        winetest_pop_context();
        return;
    }
    ok(Query != NULL, "No query\n");

    Wait = WaitForSingleObject(Ctx.Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Wait returned %lu\n", Wait);
    ok(Ctx.Completed == 1, "Completed %ld times\n", Ctx.Completed);
    ok(Ctx.Aborted == 0, "Aborted %ld times\n", Ctx.Aborted);
    ok(Ctx.AddsBeforeCompletion == (LONG)Expected, "%ld adds, expected %lu\n", Ctx.AddsBeforeCompletion, Expected);
    ok(Ctx.BadObjects == 0, "%ld bad objects\n", Ctx.BadObjects);
    ok(Ctx.Other == 0, "%ld unexpected actions\n", Ctx.Other);
    if (Ctx.Adds && List)
        ok(MultiSzContains(List, Ctx.FirstId), "Unexpected interface %S\n", Ctx.FirstId);

    if (!CloseInCallback)
        pDevCloseObjectQuery(Query);

    if (QueryFlags & DevQueryFlagAsyncClose)
    {
        Wait = WaitForSingleObject(Ctx.ClosedEvent, 10000);
        ok(Wait == WAIT_OBJECT_0, "Wait for the close returned %lu\n", Wait);
        ok(Ctx.Closed == 1, "Closed %ld times\n", Ctx.Closed);
    }
    else if (!CloseInCallback)
    {
        /* No callback comes after a synchronous close */
        Calls = Ctx.Calls;
        Sleep(600);
        ok(Ctx.Calls == Calls, "%ld callbacks after the close\n", Ctx.Calls - Calls);
        ok(Ctx.Closed == 0, "Closed %ld times\n", Ctx.Closed);
    }
    /* The callback gets the handle we got */
    ok(Ctx.Handles == 1 && Ctx.Handle == Query, "Callback handles %ld, last %p, expected %p\n",
       Ctx.Handles, Ctx.Handle, Query);

    CloseHandle(Ctx.ClosedEvent);
    CloseHandle(Ctx.Done);
    HeapFree(GetProcessHeap(), 0, List);
    winetest_pop_context();
}

START_TEST(DevQuery)
{
    HMODULE Cfgmgr32 = GetModuleHandleW(L"cfgmgr32.dll");
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];

    if (!Cfgmgr32)
        Cfgmgr32 = LoadLibraryW(L"cfgmgr32.dll");
    pDevCreateObjectQuery = (PVOID)GetProcAddress(Cfgmgr32, "DevCreateObjectQuery");
    pDevCloseObjectQuery = (PVOID)GetProcAddress(Cfgmgr32, "DevCloseObjectQuery");
    pDevGetObjects = (PVOID)GetProcAddress(Cfgmgr32, "DevGetObjects");
    pDevFreeObjects = (PVOID)GetProcAddress(Cfgmgr32, "DevFreeObjects");

    if (!pDevCreateObjectQuery || !pDevCloseObjectQuery || !pDevGetObjects || !pDevFreeObjects)
    {
        skip("The device query functions are not available\n");
        return;
    }

    TestArguments();
    TestGetObjects(InstanceId, ARRAYSIZE(InstanceId));
    if (!InstanceId[0])
    {
        skip("No disk interface to query\n");
        return;
    }
    TestQuery(InstanceId, DevQueryFlagUpdateResults, FALSE);
    TestQuery(InstanceId, DevQueryFlagNone, FALSE);
    TestQuery(InstanceId, DevQueryFlagUpdateResults | DevQueryFlagAsyncClose, FALSE);
    /* Only an asynchronous close may come from the callback */
    TestQuery(InstanceId, DevQueryFlagUpdateResults | DevQueryFlagAsyncClose, TRUE);
}

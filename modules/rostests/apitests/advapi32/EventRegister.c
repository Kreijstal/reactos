/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for manifest-based ETW provider registration
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"

#include <evntprov.h>

typedef ULONG (WINAPI *FN_EventRegister)(LPCGUID, PENABLECALLBACK, PVOID, PREGHANDLE);
typedef ULONG (WINAPI *FN_EventUnregister)(REGHANDLE);
typedef BOOLEAN (WINAPI *FN_EventEnabled)(REGHANDLE, PCEVENT_DESCRIPTOR);
typedef ULONG (WINAPI *FN_EventWrite)(REGHANDLE, PCEVENT_DESCRIPTOR, ULONG, PEVENT_DATA_DESCRIPTOR);
typedef ULONG (WINAPI *FN_EventSetInformation)(REGHANDLE, EVENT_INFO_CLASS, PVOID, ULONG);
typedef ULONG (WINAPI *FN_EventActivityIdControl)(ULONG, LPGUID);

/* {6B6F3B9C-5A3E-4C2D-9C55-3E1A0B5A7E11} */
static const GUID TestProvider =
    { 0x6b6f3b9c, 0x5a3e, 0x4c2d, { 0x9c, 0x55, 0x3e, 0x1a, 0x0b, 0x5a, 0x7e, 0x11 } };

static LONG CallbackCount;

static
VOID
NTAPI
EnableCallback(
    LPCGUID SourceId,
    ULONG IsEnabled,
    UCHAR Level,
    ULONGLONG MatchAnyKeyword,
    ULONGLONG MatchAllKeyword,
    PEVENT_FILTER_DESCRIPTOR FilterData,
    PVOID CallbackContext)
{
    InterlockedIncrement(&CallbackCount);
}

START_TEST(EventRegister)
{
    HMODULE Advapi32 = GetModuleHandleW(L"advapi32.dll");
    FN_EventRegister pEventRegister;
    FN_EventUnregister pEventUnregister;
    FN_EventEnabled pEventEnabled;
    FN_EventWrite pEventWrite;
    FN_EventSetInformation pEventSetInformation;
    FN_EventActivityIdControl pEventActivityIdControl;
    GUID Id1, Id2, Saved;
    UCHAR Traits[3] = { 3, 0, 0 };
    EVENT_DESCRIPTOR Descriptor = { 1, 0, 0, 4, 0, 0, 0 };
    EVENT_DATA_DESCRIPTOR Data;
    REGHANDLE Handle, Handle2;
    ULONG Value = 42;

    pEventRegister = (FN_EventRegister)GetProcAddress(Advapi32, "EventRegister");
    pEventUnregister = (FN_EventUnregister)GetProcAddress(Advapi32, "EventUnregister");
    pEventEnabled = (FN_EventEnabled)GetProcAddress(Advapi32, "EventEnabled");
    pEventWrite = (FN_EventWrite)GetProcAddress(Advapi32, "EventWrite");
    pEventSetInformation = (FN_EventSetInformation)GetProcAddress(Advapi32, "EventSetInformation");
    pEventActivityIdControl = (FN_EventActivityIdControl)GetProcAddress(Advapi32, "EventActivityIdControl");
    if (!pEventRegister || !pEventUnregister || !pEventEnabled || !pEventWrite)
    {
        ok(GetNTVersion() < _WIN32_WINNT_VISTA, "Event provider functions are not exported\n");
        return;
    }

    ok_long(pEventRegister(NULL, NULL, NULL, &Handle), ERROR_INVALID_PARAMETER);
    ok_long(pEventRegister(&TestProvider, NULL, NULL, NULL), ERROR_INVALID_PARAMETER);

    Handle = 0;
    ok_long(pEventRegister(&TestProvider, EnableCallback, NULL, &Handle), ERROR_SUCCESS);
    ok(Handle != 0, "Got a null handle\n");
    Handle2 = 0;
    ok_long(pEventRegister(&TestProvider, NULL, NULL, &Handle2), ERROR_SUCCESS);
    ok(Handle2 != 0 && Handle2 != Handle, "Handles %I64x %I64x\n", Handle, Handle2);

    /* Nobody traces this provider */
    ok(!pEventEnabled(Handle, &Descriptor), "Provider is enabled\n");
    ok_long(CallbackCount, 0);

    EventDataDescCreate(&Data, &Value, sizeof(Value));
    ok_long(pEventWrite(Handle, &Descriptor, 1, &Data), ERROR_SUCCESS);
    ok_long(pEventWrite(Handle, NULL, 1, &Data), ERROR_INVALID_PARAMETER);
    ok_long(pEventWrite(0, &Descriptor, 1, &Data), ERROR_INVALID_HANDLE);

    if (pEventSetInformation)
    {
        ok_long(pEventSetInformation(Handle, EventProviderSetTraits, Traits, sizeof(Traits)), ERROR_SUCCESS);
        ok_long(pEventSetInformation(0, EventProviderSetTraits, Traits, sizeof(Traits)), ERROR_INVALID_HANDLE);
    }
    else
    {
        ok(GetNTVersion() < _WIN32_WINNT_WIN8, "EventSetInformation is not exported\n");
    }

    ok_long(pEventUnregister(Handle), ERROR_SUCCESS);
    ok_long(pEventUnregister(Handle2), ERROR_SUCCESS);

    /* A handle is dead once unregistered */
    ok_long(pEventWrite(Handle, &Descriptor, 1, &Data), ERROR_INVALID_HANDLE);
    ok_long(pEventUnregister(Handle), ERROR_INVALID_HANDLE);

    if (!pEventActivityIdControl)
    {
        ok(GetNTVersion() < _WIN32_WINNT_VISTA, "EventActivityIdControl is not exported\n");
        return;
    }

    /* The activity ID belongs to the thread */
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_GET_ID, &Saved), ERROR_SUCCESS);
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_CREATE_ID, &Id1), ERROR_SUCCESS);
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_CREATE_ID, &Id2), ERROR_SUCCESS);
    ok(!IsEqualGUID(&Id1, &Id2), "Created the same activity ID twice\n");
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_SET_ID, &Id1), ERROR_SUCCESS);
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_GET_ID, &Id2), ERROR_SUCCESS);
    ok(IsEqualGUID(&Id1, &Id2), "SET_ID did not stick\n");
    Id2 = Saved;
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_GET_SET_ID, &Id2), ERROR_SUCCESS);
    ok(IsEqualGUID(&Id1, &Id2), "GET_SET_ID did not return the previous ID\n");
    ok_long(pEventActivityIdControl(0, &Id2), ERROR_INVALID_PARAMETER);
    ok_long(pEventActivityIdControl(EVENT_ACTIVITY_CTRL_GET_ID, NULL), ERROR_INVALID_PARAMETER);
}

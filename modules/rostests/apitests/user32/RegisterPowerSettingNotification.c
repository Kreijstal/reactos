/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for RegisterPowerSettingNotification
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"

/* The Vista types, whatever the target version of the test is */
typedef struct
{
    GUID PowerSetting;
    DWORD DataLength;
    UCHAR Data[1];
} TEST_POWERBROADCAST_SETTING;
#define TEST_PBT_POWERSETTINGCHANGE 0x8013

typedef PVOID (WINAPI *FN_RegisterPowerSettingNotification)(HANDLE, LPCGUID, DWORD);
typedef BOOL (WINAPI *FN_UnregisterPowerSettingNotification)(PVOID);
typedef DWORD (WINAPI *FN_PowerGetActiveScheme)(HKEY, GUID **);
typedef DWORD (WINAPI *FN_PowerReadValueIndex)(HKEY, const GUID *, const GUID *, const GUID *, LPDWORD);
typedef DWORD (WINAPI *FN_PowerWriteValueIndex)(HKEY, const GUID *, const GUID *, const GUID *, DWORD);

static FN_RegisterPowerSettingNotification pRegisterPowerSettingNotification;
static FN_UnregisterPowerSettingNotification pUnregisterPowerSettingNotification;

static const GUID TestGuidAcDcPowerSource =
    { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };
static const GUID TestGuidMonitorPowerOn =
    { 0x02731015, 0x4510, 0x4526, { 0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea } };
static const GUID TestGuidDiskSubgroup =
    { 0x0012ee47, 0x9041, 0x4b5d, { 0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42 } };
static const GUID TestGuidDiskPowerdownTimeout =
    { 0x6738e2c4, 0xe8a5, 0x4a42, { 0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e } };

/* The last PBT_POWERSETTINGCHANGE the window got */
static ULONG NotifyCount;
static GUID NotifyGuid;
static DWORD NotifyLength;
static DWORD NotifyValue;
static DWORD NotifyThread;

static
LRESULT
CALLBACK
PowerWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == WM_POWERBROADCAST && wParam == TEST_PBT_POWERSETTINGCHANGE)
    {
        const TEST_POWERBROADCAST_SETTING *Setting = (const TEST_POWERBROADCAST_SETTING *)lParam;

        NotifyThread = GetCurrentThreadId();
        ok(Setting != NULL, "No POWERBROADCAST_SETTING\n");
        if (Setting)
        {
            NotifyGuid = Setting->PowerSetting;
            NotifyLength = Setting->DataLength;
            NotifyValue = 0xdeadbeef;
            if (Setting->DataLength == sizeof(DWORD))
                NotifyValue = *(const DWORD *)Setting->Data;
        }
        NotifyCount++;
        return TRUE;
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

/* Pumps messages until another notification came or the time ran out */
static
BOOL
WaitForNotification(ULONG Count, DWORD TimeoutMs)
{
    DWORD Start = GetTickCount();
    MSG Msg;

    for (;;)
    {
        DWORD Elapsed;

        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        if (NotifyCount != Count)
            return TRUE;

        Elapsed = GetTickCount() - Start;
        if (Elapsed >= TimeoutMs)
            return FALSE;
        MsgWaitForMultipleObjects(0, NULL, FALSE, TimeoutMs - Elapsed, QS_ALLINPUT);
    }
}

static
VOID
TestInitialValue(HWND hWnd, const GUID *Guid, DWORD MaxValue, PCSTR Name)
{
    PVOID Notify;
    ULONG Count = NotifyCount;
    BOOL Ret;

    Notify = pRegisterPowerSettingNotification(hWnd, Guid, DEVICE_NOTIFY_WINDOW_HANDLE);
    ok(Notify != NULL, "%s: RegisterPowerSettingNotification failed, error %lu\n", Name, GetLastError());
    if (!Notify)
        return;

    /* The current value comes right after registering */
    ok(WaitForNotification(Count, 5000), "%s: no notification of the current value\n", Name);
    ok(NotifyCount == Count + 1, "%s: %lu notifications\n", Name, NotifyCount - Count);
    ok(IsEqualGUID(&NotifyGuid, Guid), "%s: notified for %08lx\n", Name, NotifyGuid.Data1);
    ok(NotifyLength == sizeof(DWORD), "%s: DataLength %lu\n", Name, NotifyLength);
    ok(NotifyValue <= MaxValue, "%s: value %lu\n", Name, NotifyValue);
    ok(NotifyThread == GetCurrentThreadId(), "%s: delivered on thread %lu\n", Name, NotifyThread);

    Ret = pUnregisterPowerSettingNotification(Notify);
    ok(Ret == TRUE, "%s: UnregisterPowerSettingNotification returned %d, error %lu\n", Name, Ret, GetLastError());

    SetLastError(0xdeadbeef);
    Ret = pUnregisterPowerSettingNotification(Notify);
    ok(Ret == FALSE, "%s: second UnregisterPowerSettingNotification returned %d\n", Name, Ret);
}

/* A scheme value written through powrprof reaches a registered window */
static
VOID
TestSchemeChange(HWND hWnd)
{
    FN_PowerGetActiveScheme pPowerGetActiveScheme;
    FN_PowerReadValueIndex pPowerReadACValueIndex, pPowerReadDCValueIndex;
    FN_PowerWriteValueIndex pPowerWriteACValueIndex, pPowerWriteDCValueIndex;
    HMODULE hPowrprof;
    GUID *Scheme = NULL;
    DWORD Ac = 0, Dc = 0, NewValue, Error;
    PVOID Notify;
    ULONG Count;
    BOOL Ret;

    hPowrprof = LoadLibraryW(L"powrprof.dll");
    if (!hPowrprof)
    {
        skip("No powrprof\n");
        return;
    }
    pPowerGetActiveScheme = (FN_PowerGetActiveScheme)GetProcAddress(hPowrprof, "PowerGetActiveScheme");
    pPowerReadACValueIndex = (FN_PowerReadValueIndex)GetProcAddress(hPowrprof, "PowerReadACValueIndex");
    pPowerReadDCValueIndex = (FN_PowerReadValueIndex)GetProcAddress(hPowrprof, "PowerReadDCValueIndex");
    pPowerWriteACValueIndex = (FN_PowerWriteValueIndex)GetProcAddress(hPowrprof, "PowerWriteACValueIndex");
    pPowerWriteDCValueIndex = (FN_PowerWriteValueIndex)GetProcAddress(hPowrprof, "PowerWriteDCValueIndex");
    if (!pPowerGetActiveScheme || !pPowerReadACValueIndex || !pPowerReadDCValueIndex ||
        !pPowerWriteACValueIndex || !pPowerWriteDCValueIndex)
    {
        skip("No power scheme API\n");
        FreeLibrary(hPowrprof);
        return;
    }

    Error = pPowerGetActiveScheme(NULL, &Scheme);
    ok(Error == ERROR_SUCCESS, "PowerGetActiveScheme returned %lu\n", Error);
    if (Error != ERROR_SUCCESS)
    {
        FreeLibrary(hPowrprof);
        return;
    }
    Error = pPowerReadACValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, &Ac);
    ok(Error == ERROR_SUCCESS, "PowerReadACValueIndex returned %lu\n", Error);
    Error = pPowerReadDCValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, &Dc);
    ok(Error == ERROR_SUCCESS, "PowerReadDCValueIndex returned %lu\n", Error);

    Count = NotifyCount;
    Notify = pRegisterPowerSettingNotification(hWnd, &TestGuidDiskPowerdownTimeout, DEVICE_NOTIFY_WINDOW_HANDLE);
    ok(Notify != NULL, "RegisterPowerSettingNotification failed, error %lu\n", GetLastError());
    if (!Notify)
        goto Cleanup;

    ok(WaitForNotification(Count, 5000), "No notification of the current disk time-out\n");
    ok(IsEqualGUID(&NotifyGuid, &TestGuidDiskPowerdownTimeout), "Notified for %08lx\n", NotifyGuid.Data1);
    ok(NotifyValue == Ac || NotifyValue == Dc, "Disk time-out %lu, scheme has %lu/%lu\n", NotifyValue, Ac, Dc);

    /* Both sources, so the value changes whichever one the system runs from */
    NewValue = max(Ac, Dc) + 77;
    Count = NotifyCount;
    Error = pPowerWriteACValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, NewValue);
    ok(Error == ERROR_SUCCESS, "PowerWriteACValueIndex returned %lu\n", Error);
    Error = pPowerWriteDCValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, NewValue);
    ok(Error == ERROR_SUCCESS, "PowerWriteDCValueIndex returned %lu\n", Error);

    /* AC and DC may come as one change or two */
    while (WaitForNotification(Count, 5000) && NotifyValue != NewValue)
        Count = NotifyCount;
    ok(NotifyValue == NewValue, "Disk time-out %lu after writing %lu\n", NotifyValue, NewValue);

    Ret = pUnregisterPowerSettingNotification(Notify);
    ok(Ret == TRUE, "UnregisterPowerSettingNotification returned %d\n", Ret);

    /* Nothing after unregistering */
    WaitForNotification(NotifyCount, 1000);
    Count = NotifyCount;
    pPowerWriteACValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, NewValue + 1);
    ok(!WaitForNotification(Count, 2000), "Notified after unregistering (value %lu)\n", NotifyValue);

Cleanup:
    Error = pPowerWriteACValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, Ac);
    ok(Error == ERROR_SUCCESS, "Restoring the AC disk time-out returned %lu\n", Error);
    Error = pPowerWriteDCValueIndex(NULL, Scheme, &TestGuidDiskSubgroup, &TestGuidDiskPowerdownTimeout, Dc);
    ok(Error == ERROR_SUCCESS, "Restoring the DC disk time-out returned %lu\n", Error);
    LocalFree(Scheme);
    FreeLibrary(hPowrprof);
}

START_TEST(RegisterPowerSettingNotification)
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    WNDCLASSW WndClass = { 0 };
    HWND hWnd;
    PVOID Notify;

    pRegisterPowerSettingNotification =
        (FN_RegisterPowerSettingNotification)GetProcAddress(hUser32, "RegisterPowerSettingNotification");
    pUnregisterPowerSettingNotification =
        (FN_UnregisterPowerSettingNotification)GetProcAddress(hUser32, "UnregisterPowerSettingNotification");
    if (!pRegisterPowerSettingNotification || !pUnregisterPowerSettingNotification)
    {
        skip("RegisterPowerSettingNotification is not available\n");
        return;
    }

    WndClass.lpfnWndProc = PowerWndProc;
    WndClass.hInstance = GetModuleHandleW(NULL);
    WndClass.lpszClassName = L"PowerSettingTest";
    ok(RegisterClassW(&WndClass) != 0, "RegisterClassW failed\n");
    hWnd = CreateWindowExW(0, L"PowerSettingTest", L"", WS_OVERLAPPEDWINDOW,
                           0, 0, 100, 100, NULL, NULL, WndClass.hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowExW failed\n");
    if (!hWnd)
        return;

    /* SYSTEM_POWER_CONDITION: PoAc, PoDc or PoHot */
    TestInitialValue(hWnd, &TestGuidAcDcPowerSource, 2, "ACDC_POWER_SOURCE");
    /* The display is on while the test runs */
    TestInitialValue(hWnd, &TestGuidMonitorPowerOn, 1, "MONITOR_POWER_ON");
    ok(NotifyValue == 1, "MONITOR_POWER_ON: value %lu\n", NotifyValue);

    TestSchemeChange(hWnd);

    SetLastError(0xdeadbeef);
    Notify = pRegisterPowerSettingNotification(hWnd, &TestGuidAcDcPowerSource, 0x10);
    ok(Notify == NULL, "Registered with invalid flags\n");
    if (Notify)
        pUnregisterPowerSettingNotification(Notify);

    DestroyWindow(hWnd);
    UnregisterClassW(L"PowerSettingTest", WndClass.hInstance);
}

/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the unified device property functions
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>

/* A private property key for the test */
static const DEVPROPKEY DEVPKEY_Apitest_Name = {
    { 0x3361c968, 0x2f2e, 0x4660, { 0xb4, 0x7e, 0x69, 0x9c, 0xdc, 0x4c, 0x32, 0xb9 } }, 2
};

static const GUID GUID_DEVCLASS_SAMPLE = {
    0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

START_TEST(SetupDiDeviceProperty)
{
    HDEVINFO DevInfo;
    SP_DEVINFO_DATA DevInfoData = { sizeof(DevInfoData) };
    DEVPROPTYPE Type;
    DWORD Required;
    BOOL Ret;
    WCHAR Value[64], Read[64];
    static const WCHAR Hwid[] = L"ApitestDeviceProperty\0";

    DevInfo = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SAMPLE, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        skip("SetupDiCreateDeviceInfoList failed: %lu\n", GetLastError());
        return;
    }

    if (!SetupDiCreateDeviceInfoW(DevInfo, L"ApitestDeviceProperty", &GUID_DEVCLASS_SAMPLE,
                                  NULL, NULL, DICD_GENERATE_ID, &DevInfoData))
    {
        skip("SetupDiCreateDeviceInfoW failed: %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(DevInfo);
        return;
    }

    /* Register it so the device node exists and has a Properties key */
    if (!SetupDiSetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_HARDWAREID,
                                           (const BYTE *)Hwid, sizeof(Hwid)) ||
        !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
    {
        skip("Registering the test device failed: %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(DevInfo);
        return;
    }

    /* Reading a property that was never set fails */
    Type = 0xdead;
    Required = sizeof(Read);
    SetLastError(0xdeadbeef);
    Ret = SetupDiGetDevicePropertyW(DevInfo, &DevInfoData, &DEVPKEY_Apitest_Name, &Type,
                                    (PBYTE)Read, sizeof(Read), &Required, 0);
    ok(!Ret, "SetupDiGetDevicePropertyW succeeded for a missing property\n");
    ok(GetLastError() == ERROR_NOT_FOUND, "LastError = %lu\n", GetLastError());

    /* Set a string property */
    wcscpy(Value, L"Hello Wintun");
    SetLastError(0xdeadbeef);
    Ret = SetupDiSetDevicePropertyW(DevInfo, &DevInfoData, &DEVPKEY_Apitest_Name,
                                    DEVPROP_TYPE_STRING, (const BYTE *)Value,
                                    (DWORD)((wcslen(Value) + 1) * sizeof(WCHAR)), 0);
    ok(Ret, "SetupDiSetDevicePropertyW failed: %lu\n", GetLastError());

    /* Read it back */
    Type = 0;
    Required = 0;
    ZeroMemory(Read, sizeof(Read));
    SetLastError(0xdeadbeef);
    Ret = SetupDiGetDevicePropertyW(DevInfo, &DevInfoData, &DEVPKEY_Apitest_Name, &Type,
                                    (PBYTE)Read, sizeof(Read), &Required, 0);
    ok(Ret, "SetupDiGetDevicePropertyW failed: %lu\n", GetLastError());
    ok(Type == DEVPROP_TYPE_STRING, "Type = 0x%lx\n", Type);
    ok(Required == (wcslen(Value) + 1) * sizeof(WCHAR), "Required = %lu\n", Required);
    ok(!wcscmp(Read, Value), "Read = \"%ls\"\n", Read);

    /* A too-small buffer reports the required size */
    Type = 0;
    Required = 0;
    SetLastError(0xdeadbeef);
    Ret = SetupDiGetDevicePropertyW(DevInfo, &DevInfoData, &DEVPKEY_Apitest_Name, &Type,
                                    (PBYTE)Read, 2, &Required, 0);
    ok(!Ret, "SetupDiGetDevicePropertyW succeeded with a small buffer\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "LastError = %lu\n", GetLastError());
    ok(Required == (wcslen(Value) + 1) * sizeof(WCHAR), "Required = %lu\n", Required);

    /* Also reachable through CM_Get_DevNode_PropertyW, forwarded by setupapi */
    {
        CONFIGRET (WINAPI *pCM_Get_DevNode_PropertyW)(DEVINST, const DEVPROPKEY *, DEVPROPTYPE *,
                                                      PBYTE, PULONG, ULONG);
        pCM_Get_DevNode_PropertyW = (PVOID)GetProcAddress(GetModuleHandleW(L"setupapi.dll"),
                                                          "CM_Get_DevNode_PropertyW");
        if (pCM_Get_DevNode_PropertyW)
        {
            Type = 0;
            Required = sizeof(Read);
            ok(pCM_Get_DevNode_PropertyW(DevInfoData.DevInst, &DEVPKEY_Apitest_Name, &Type,
                                         (PBYTE)Read, &Required, 0) == CR_SUCCESS,
               "CM_Get_DevNode_PropertyW failed\n");
            ok(!wcscmp(Read, Value), "Read = \"%ls\"\n", Read);
        }
    }

    /* Remove the device */
    SetupDiRemoveDevice(DevInfo, &DevInfoData);
    SetupDiDestroyDeviceInfoList(DevInfo);
}

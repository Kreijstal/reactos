/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the registry state of a new device and its software key
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

static const GUID GUID_DEVCLASS_SAMPLE = {
    0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

START_TEST(SetupDiCreateDeviceInfo)
{
    static const WCHAR Hwid[] = L"ApitestCreateDeviceInfo\0";
    static const WCHAR Description[] = L"Apitest device description";
    HDEVINFO DevInfo;
    SP_DEVINFO_DATA DevInfoData = { sizeof(DevInfoData) };
    SP_DEVINSTALL_PARAMS_W InstallParams = { sizeof(InstallParams) };
    WCHAR Buffer[MAX_PATH], Driver[MAX_PATH], KeyPath[MAX_PATH], InstanceId[MAX_DEVICE_ID_LEN];
    DEVINST DevInst;
    DWORD Type, Size, Value;
    ULONG Length;
    CONFIGRET Cr;
    HKEY Key;
    LONG Error;

    DevInfo = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SAMPLE, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        skip("SetupDiCreateDeviceInfoList failed: %lu\n", GetLastError());
        return;
    }
    if (!SetupDiCreateDeviceInfoW(DevInfo, L"ApitestCreateDeviceInfo", &GUID_DEVCLASS_SAMPLE,
                                  Description, NULL, DICD_GENERATE_ID, &DevInfoData))
    {
        skip("SetupDiCreateDeviceInfoW failed: %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(DevInfo);
        return;
    }

    /* The class and the description are in the registry right away */
    ZeroMemory(Buffer, sizeof(Buffer));
    ok(SetupDiGetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_CLASSGUID, &Type,
                                         (PBYTE)Buffer, sizeof(Buffer), NULL),
       "No class GUID: %lu\n", GetLastError());
    ok(!_wcsicmp(Buffer, L"{4d36e978-e325-11ce-bfc1-08002be10318}"), "Class GUID %ls\n", Buffer);
    ZeroMemory(Buffer, sizeof(Buffer));
    ok(SetupDiGetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_DEVICEDESC, &Type,
                                         (PBYTE)Buffer, sizeof(Buffer), NULL),
       "No description: %lu\n", GetLastError());
    ok(!wcscmp(Buffer, Description), "Description %ls\n", Buffer);

    if (!SetupDiSetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_HARDWAREID,
                                           (const BYTE *)Hwid, sizeof(Hwid)) ||
        !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
    {
        skip("Registering the test device failed: %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(DevInfo);
        return;
    }

    /* Without a driver there is no software key yet */
    Length = sizeof(Driver);
    Cr = CM_Get_DevNode_Registry_PropertyW(DevInfoData.DevInst, CM_DRP_DRIVER, NULL, Driver, &Length, 0);
    ok(Cr == CR_NO_SUCH_VALUE, "CM_DRP_DRIVER: %lu\n", Cr);
    Key = NULL;
    Cr = CM_Open_DevNode_Key(DevInfoData.DevInst, KEY_QUERY_VALUE, 0, RegDisposition_OpenExisting,
                             &Key, CM_REGISTRY_SOFTWARE);
    ok(Cr == CR_NO_SUCH_REGISTRY_KEY, "Opening a missing software key: %lu\n", Cr);
    if (Cr == CR_SUCCESS)
        RegCloseKey(Key);

    /* Opening it always creates it, as Wintun does for its stub adapter */
    Key = NULL;
    Cr = CM_Open_DevNode_Key(DevInfoData.DevInst, KEY_SET_VALUE | KEY_QUERY_VALUE, 0, RegDisposition_OpenAlways,
                             &Key, CM_REGISTRY_SOFTWARE);
    ok(Cr == CR_SUCCESS, "CM_Open_DevNode_Key(OpenAlways): %lu\n", Cr);
    if (Cr == CR_SUCCESS)
    {
        Value = 0x1234;
        ok(RegSetValueExW(Key, L"ApitestValue", 0, REG_DWORD, (PBYTE)&Value, sizeof(Value)) == ERROR_SUCCESS,
           "RegSetValueExW failed\n");
        RegCloseKey(Key);
    }

    /* The device now names its software key */
    ZeroMemory(Driver, sizeof(Driver));
    Length = sizeof(Driver);
    Cr = CM_Get_DevNode_Registry_PropertyW(DevInfoData.DevInst, CM_DRP_DRIVER, NULL, Driver, &Length, 0);
    ok(Cr == CR_SUCCESS, "CM_DRP_DRIVER: %lu\n", Cr);
    ok(!_wcsnicmp(Driver, L"{4d36e978-e325-11ce-bfc1-08002be10318}\\", 39) && wcslen(Driver) == 43,
       "Driver %ls\n", Driver);

    Key = NULL;
    Cr = CM_Open_DevNode_Key(DevInfoData.DevInst, KEY_QUERY_VALUE, 0, RegDisposition_OpenExisting,
                             &Key, CM_REGISTRY_SOFTWARE);
    ok(Cr == CR_SUCCESS, "CM_Open_DevNode_Key(OpenExisting): %lu\n", Cr);
    if (Cr == CR_SUCCESS)
    {
        Value = 0;
        Size = sizeof(Value);
        ok(RegQueryValueExW(Key, L"ApitestValue", NULL, &Type, (PBYTE)&Value, &Size) == ERROR_SUCCESS &&
           Value == 0x1234, "Value 0x%lx\n", Value);
        RegCloseKey(Key);
    }

    /* Removing the device removes it with its keys, without a reboot */
    ok(SetupDiGetDeviceInstanceIdW(DevInfo, &DevInfoData, InstanceId, ARRAYSIZE(InstanceId), NULL),
       "SetupDiGetDeviceInstanceIdW failed: %lu\n", GetLastError());
    ok(SetupDiRemoveDevice(DevInfo, &DevInfoData), "SetupDiRemoveDevice failed: %lu\n", GetLastError());
    ok(SetupDiGetDeviceInstallParamsW(DevInfo, &DevInfoData, &InstallParams) &&
       !(InstallParams.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)), "Install flags 0x%lx\n", InstallParams.Flags);
    SetupDiDestroyDeviceInfoList(DevInfo);
    Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok(Cr == CR_NO_SUCH_DEVNODE, "Locating %ls: %lu\n", InstanceId, Cr);
    if (Driver[0])
    {
        _snwprintf(KeyPath, ARRAYSIZE(KeyPath), L"SYSTEM\\CurrentControlSet\\Control\\Class\\%s", Driver);
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyPath, 0, KEY_QUERY_VALUE, &Key);
        ok(Error == ERROR_FILE_NOT_FOUND, "Software key %ls: %ld\n", Driver, Error);
        if (Error == ERROR_SUCCESS)
            RegCloseKey(Key);
    }
}

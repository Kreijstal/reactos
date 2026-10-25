/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the lifetime of software devices
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <swdevice.h>

/* Windows exports these from cfgmgr32 only */
static HRESULT (WINAPI *pSwDeviceCreate)(PCWSTR, PCWSTR, const SW_DEVICE_CREATE_INFO *, ULONG,
                                         const DEVPROPERTY *, SW_DEVICE_CREATE_CALLBACK, PVOID, PHSWDEVICE);
static VOID (WINAPI *pSwDeviceClose)(HSWDEVICE);

/* A device class without a class installer to get in the way */
static const GUID GUID_DEVCLASS_SAMPLE = {
    0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};
static const DEVPROPKEY DEVPKEY_Device_ClassGuid_ = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 10
};

typedef struct _CREATE_CTX
{
    HANDLE Done;
    HRESULT Result;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
} CREATE_CTX;

static
VOID
CALLBACK
CreateCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT CreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId)
{
    CREATE_CTX *Ctx = pContext;

    Ctx->Result = CreateResult;
    if (pszDeviceInstanceId)
        lstrcpynW(Ctx->InstanceId, pszDeviceInstanceId, ARRAYSIZE(Ctx->InstanceId));
    SetEvent(Ctx->Done);
}

/* Make the device, without a driver, as Wintun does with its stub */
static
HSWDEVICE
CreateDevice(
    _Out_ CREATE_CTX *Ctx)
{
    SW_DEVICE_CREATE_INFO CreateInfo = { sizeof(CreateInfo) };
    DEVPROPERTY Property;
    HSWDEVICE SwDevice = NULL;
    HRESULT hr;

    ZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    CreateInfo.pszInstanceId = L"ApitestSwDevice";
    CreateInfo.pszzHardwareIds = L"\0";
    CreateInfo.CapabilityFlags = SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;
    CreateInfo.pszDeviceDescription = L"Apitest software device";
    ZeroMemory(&Property, sizeof(Property));
    Property.CompKey.Key = DEVPKEY_Device_ClassGuid_;
    Property.CompKey.Store = DEVPROP_STORE_SYSTEM;
    Property.Type = DEVPROP_TYPE_GUID;
    Property.Buffer = (PVOID)&GUID_DEVCLASS_SAMPLE;
    Property.BufferSize = sizeof(GUID);

    hr = pSwDeviceCreate(L"ApitestSwd", L"HTREE\\ROOT\\0", &CreateInfo, 1, &Property,
                         CreateCallback, Ctx, &SwDevice);
    ok(hr == S_OK, "SwDeviceCreate failed: 0x%lx\n", hr);
    if (FAILED(hr))
    {
        CloseHandle(Ctx->Done);
        return NULL;
    }
    ok(WaitForSingleObject(Ctx->Done, 30000) == WAIT_OBJECT_0, "No creation callback\n");
    ok(Ctx->Result == S_OK, "Creation result 0x%lx\n", Ctx->Result);
    ok(!_wcsicmp(Ctx->InstanceId, L"SWD\\ApitestSwd\\ApitestSwDevice"), "Instance %ls\n", Ctx->InstanceId);
    CloseHandle(Ctx->Done);
    if (FAILED(Ctx->Result))
    {
        pSwDeviceClose(SwDevice);
        return NULL;
    }
    return SwDevice;
}

/* Whether SetupDiGetClassDevs lists the device, with the enumerator filter Wintun uses */
static
BOOL
ClassDevsHas(
    _In_ PCWSTR InstanceId,
    _In_ DWORD Flags)
{
    SP_DEVINFO_DATA DevInfoData = { sizeof(DevInfoData) };
    WCHAR Id[MAX_DEVICE_ID_LEN];
    HDEVINFO DevInfo;
    BOOL Found = FALSE;
    DWORD Index;

    DevInfo = SetupDiGetClassDevsExW(&GUID_DEVCLASS_SAMPLE, L"SWD\\ApitestSwd", NULL, Flags, NULL, NULL, NULL);
    ok(DevInfo != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsExW failed: %lu\n", GetLastError());
    if (DevInfo == INVALID_HANDLE_VALUE)
        return FALSE;
    for (Index = 0; SetupDiEnumDeviceInfo(DevInfo, Index, &DevInfoData); Index++)
    {
        if (SetupDiGetDeviceInstanceIdW(DevInfo, &DevInfoData, Id, ARRAYSIZE(Id), NULL) &&
            !_wcsicmp(Id, InstanceId))
            Found = TRUE;
    }
    SetupDiDestroyDeviceInfoList(DevInfo);
    return Found;
}

START_TEST(SwDevice)
{
    HMODULE Cfgmgr32 = LoadLibraryW(L"cfgmgr32.dll");
    SP_DEVINFO_DATA DevInfoData = { sizeof(DevInfoData) };
    SP_REMOVEDEVICE_PARAMS RemoveParams;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    CREATE_CTX Ctx;
    HSWDEVICE SwDevice;
    HDEVINFO DevInfo;
    DEVINST DevInst;
    CONFIGRET Cr;
    DWORD Value, Size, i;
    HKEY Key;

    pSwDeviceCreate = (PVOID)GetProcAddress(Cfgmgr32, "SwDeviceCreate");
    pSwDeviceClose = (PVOID)GetProcAddress(Cfgmgr32, "SwDeviceClose");
    if (!pSwDeviceCreate || !pSwDeviceClose)
    {
        skip("The software device functions are not available\n");
        return;
    }

    SwDevice = CreateDevice(&Ctx);
    if (!SwDevice)
        return;
    lstrcpynW(InstanceId, Ctx.InstanceId, ARRAYSIZE(InstanceId));

    Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "The device is not there: %lu\n", Cr);
    ok(ClassDevsHas(InstanceId, 0), "The device is not listed\n");
    ok(ClassDevsHas(InstanceId, DIGCF_PRESENT), "The device is not listed as present\n");

    /* Something to find again in the software key */
    Cr = CM_Open_DevNode_Key(DevInst, KEY_SET_VALUE, 0, RegDisposition_OpenAlways, &Key, CM_REGISTRY_SOFTWARE);
    ok(Cr == CR_SUCCESS, "CM_Open_DevNode_Key failed: %lu\n", Cr);
    if (Cr == CR_SUCCESS)
    {
        Value = 0x5744;
        RegSetValueExW(Key, L"ApitestValue", 0, REG_DWORD, (PBYTE)&Value, sizeof(Value));
        RegCloseKey(Key);
    }

    /* Closing the handle takes the device away (Windows does it in the background), but not its keys */
    pSwDeviceClose(SwDevice);
    for (i = 0; i < 100; i++)
    {
        Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_NORMAL);
        if (Cr != CR_SUCCESS)
            break;
        Sleep(50);
    }
    ok(Cr == CR_NO_SUCH_DEVNODE, "The device is still there: %lu\n", Cr);
    Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok(Cr == CR_SUCCESS, "The device is gone: %lu\n", Cr);
    ok(ClassDevsHas(InstanceId, 0), "The device is not listed\n");
    ok(!ClassDevsHas(InstanceId, DIGCF_PRESENT), "The device is listed as present\n");

    /* Making it again brings it back, with its software key */
    SwDevice = CreateDevice(&Ctx);
    if (SwDevice)
    {
        ok(!_wcsicmp(Ctx.InstanceId, InstanceId), "Instance %ls, expected %ls\n", Ctx.InstanceId, InstanceId);
        Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_NORMAL);
        ok(Cr == CR_SUCCESS, "The device is not there: %lu\n", Cr);
        Cr = CM_Open_DevNode_Key(DevInst, KEY_QUERY_VALUE, 0, RegDisposition_OpenExisting, &Key,
                                 CM_REGISTRY_SOFTWARE);
        ok(Cr == CR_SUCCESS, "No software key: %lu\n", Cr);
        if (Cr == CR_SUCCESS)
        {
            Value = 0;
            Size = sizeof(Value);
            ok(RegQueryValueExW(Key, L"ApitestValue", NULL, NULL, (PBYTE)&Value, &Size) == ERROR_SUCCESS &&
               Value == 0x5744, "Value 0x%lx\n", Value);
            RegCloseKey(Key);
        }
        pSwDeviceClose(SwDevice);
    }

    /* Uninstalling it removes it for good */
    DevInfo = SetupDiCreateDeviceInfoList(NULL, NULL);
    ok(DevInfo != INVALID_HANDLE_VALUE, "SetupDiCreateDeviceInfoList failed: %lu\n", GetLastError());
    if (DevInfo == INVALID_HANDLE_VALUE)
        return;
    ok(SetupDiOpenDeviceInfoW(DevInfo, InstanceId, NULL, 0, &DevInfoData),
       "SetupDiOpenDeviceInfoW failed: %lu\n", GetLastError());
    /* As Wintun does it */
    RemoveParams.ClassInstallHeader.cbSize = sizeof(RemoveParams.ClassInstallHeader);
    RemoveParams.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    RemoveParams.Scope = DI_REMOVEDEVICE_GLOBAL;
    RemoveParams.HwProfile = 0;
    ok(SetupDiSetClassInstallParamsW(DevInfo, &DevInfoData, &RemoveParams.ClassInstallHeader, sizeof(RemoveParams)),
       "SetupDiSetClassInstallParamsW failed: %lu\n", GetLastError());
    ok(SetupDiCallClassInstaller(DIF_REMOVE, DevInfo, &DevInfoData), "DIF_REMOVE failed: %lu\n", GetLastError());
    SetupDiDestroyDeviceInfoList(DevInfo);
    Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok(Cr == CR_NO_SUCH_DEVNODE, "The device is still installed: %lu\n", Cr);
}

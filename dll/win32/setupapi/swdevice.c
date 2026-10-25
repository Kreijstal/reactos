/*
 * PROJECT:     ReactOS Setup API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Software device installation (swdevice.h)
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A software device is a root-enumerated device a user-mode component asks
 * the PnP manager to create, found under SWD\<enumerator>\<instance>. Wintun,
 * for one, creates its network adapter this way. We make the device node
 * through the ordinary SetupAPI install path and, with the default
 * SWDeviceLifetimeHandle, take it out of the device tree when the handle is
 * closed. Its keys stay, and creating it again brings it back with them.
 */

#include "setupapi_private.h"

#include <swdevice.h>
#include <newdev.h>
#include <initguid.h>
#include <devpkey.h>

#define NDEBUG
#include <debug.h>

#define SW_DEVICE_MAGIC 0x53774456 /* "SwDV" */

typedef struct _SW_DEVICE
{
    DWORD Magic;
    SW_DEVICE_LIFETIME Lifetime;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
} SW_DEVICE, *PSW_DEVICE;

static
BOOL
SwpIsPresent(
    _In_ PCWSTR InstanceId)
{
    DEVINST DevInst;

    return CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)InstanceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

/* Take a device out of the device tree, keeping its keys */
static
VOID
SwpRemoveFromTree(
    _In_ PCWSTR InstanceId)
{
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH];
    DEVINST DevInst;
    CONFIGRET Cr;
    ULONG i;

    if (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)InstanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return;

    VetoName[0] = UNICODE_NULL;
    Cr = CM_Query_And_Remove_SubTreeW(DevInst, &VetoType, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART);
    if (Cr != CR_SUCCESS)
    {
        WARN("Removal of %s vetoed (%lu): type %d, %s\n",
             debugstr_w(InstanceId), Cr, VetoType, debugstr_w(VetoName));
        return;
    }

    /* The device node goes away with its last reference */
    for (i = 0; i < 100 && SwpIsPresent(InstanceId); i++)
        Sleep(50);
}

/* Install the best matching driver already in the store onto a new device */
static DWORD
SwpInstallFromStore(
    _In_ HDEVINFO DevInfo,
    _In_ PSP_DEVINFO_DATA DevInfoData)
{
    SP_DEVINSTALL_PARAMS_W InstallParams = { .cbSize = sizeof(InstallParams) };
    SP_DRVINFO_DATA_W DrvInfoData = { .cbSize = sizeof(DrvInfoData) };

    if (SetupDiGetDeviceInstallParamsW(DevInfo, DevInfoData, &InstallParams))
    {
        InstallParams.Flags |= DI_QUIETINSTALL;
        SetupDiSetDeviceInstallParamsW(DevInfo, DevInfoData, &InstallParams);
    }

    if (!SetupDiBuildDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER))
        return GetLastError();

    /* No matching driver in the store: leave the device without one */
    if (!SetupDiSelectBestCompatDrv(DevInfo, DevInfoData))
    {
        SetupDiDestroyDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER);
        return ERROR_NO_DRIVER_SELECTED;
    }

    if (!SetupDiEnumDriverInfoW(DevInfo, DevInfoData, SPDIT_COMPATDRIVER, 0, &DrvInfoData) ||
        !SetupDiSetSelectedDriverW(DevInfo, DevInfoData, &DrvInfoData))
    {
        SetupDiDestroyDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER);
        return GetLastError();
    }

    SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, DevInfo, DevInfoData);
    SetupDiCallClassInstaller(DIF_ALLOW_INSTALL, DevInfo, DevInfoData);
    SetupDiCallClassInstaller(DIF_INSTALLDEVICEFILES, DevInfo, DevInfoData);
    SetupDiCallClassInstaller(DIF_REGISTER_COINSTALLERS, DevInfo, DevInfoData);
    SetupDiCallClassInstaller(DIF_INSTALLINTERFACES, DevInfo, DevInfoData);

    if (!SetupDiCallClassInstaller(DIF_INSTALLDEVICE, DevInfo, DevInfoData))
    {
        DWORD Error = GetLastError();
        SetupDiDestroyDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER);
        return Error;
    }

    SetupDiDestroyDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER);
    return ERROR_SUCCESS;
}

/***********************************************************************
 *		SwDeviceCreate (SETUPAPI.@)
 */
HRESULT
WINAPI
SwDeviceCreate(
    _In_ PCWSTR pszEnumeratorName,
    _In_ PCWSTR pszParentDeviceInstance,
    _In_ const SW_DEVICE_CREATE_INFO *pCreateInfo,
    _In_ ULONG cPropertyCount,
    _In_reads_opt_(cPropertyCount) const DEVPROPERTY *pProperties,
    _In_ SW_DEVICE_CREATE_CALLBACK pCallback,
    _In_opt_ PVOID pContext,
    _Outptr_ PHSWDEVICE phSwDevice)
{
    PSW_DEVICE SwDevice;
    HDEVINFO DevInfo = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    GUID ClassGuid = GUID_NULL;
    WCHAR FullInstanceId[MAX_DEVICE_ID_LEN];
    const GUID *pClassGuid = NULL;
    PWSTR Hwids = NULL;
    DWORD Error = ERROR_SUCCESS, HwidLen, i;
    HRESULT Result;

    TRACE("SwDeviceCreate(%s %s %p %lu %p %p %p %p)\n",
          debugstr_w(pszEnumeratorName), debugstr_w(pszParentDeviceInstance),
          pCreateInfo, cPropertyCount, pProperties, pCallback, pContext, phSwDevice);

    if (!pszEnumeratorName || !pszParentDeviceInstance || !pCreateInfo ||
        !pCreateInfo->pszInstanceId || !pCallback || !phSwDevice)
        return E_INVALIDARG;
    if (pCreateInfo->cbSize != sizeof(*pCreateInfo))
        return E_INVALIDARG;

    *phSwDevice = NULL;

    SwDevice = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*SwDevice));
    if (!SwDevice)
        return E_OUTOFMEMORY;
    SwDevice->Magic = SW_DEVICE_MAGIC;
    SwDevice->Lifetime = SWDeviceLifetimeHandle;

    /* Software devices are enumerated as SWD\<enumerator>\<instance> */
    if (_snwprintf(FullInstanceId, MAX_DEVICE_ID_LEN, L"SWD\\%s\\%s",
                   pszEnumeratorName, pCreateInfo->pszInstanceId) < 0)
    {
        Error = ERROR_INVALID_PARAMETER;
        goto cleanup;
    }
    wcscpy(SwDevice->InstanceId, FullInstanceId);

    /* The device class comes from DEVPKEY_Device_ClassGuid, if given */
    for (i = 0; i < cPropertyCount; i++)
    {
        if (IsEqualGUID(&pProperties[i].CompKey.Key.fmtid, &DEVPKEY_Device_ClassGuid.fmtid) &&
            pProperties[i].CompKey.Key.pid == DEVPKEY_Device_ClassGuid.pid &&
            pProperties[i].Type == DEVPROP_TYPE_GUID && pProperties[i].BufferSize >= sizeof(GUID))
        {
            ClassGuid = *(const GUID *)pProperties[i].Buffer;
            pClassGuid = &ClassGuid;
            break;
        }
    }

    DevInfo = SetupDiCreateDeviceInfoList(pClassGuid, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        goto cleanup;
    }

    /* An explicit instance id creates the phantom devnode */
    if (!SetupDiCreateDeviceInfoW(DevInfo, FullInstanceId, pClassGuid ? pClassGuid : &GUID_NULL,
                                  pCreateInfo->pszDeviceDescription, NULL, 0, &DevInfoData))
    {
        Error = GetLastError();
        if (Error != ERROR_DEVINST_ALREADY_EXISTS)
            goto cleanup;

        /*
         * A software device made before and closed since comes back with
         * the instance and the keys it had, as on Windows. One that is
         * still there cannot be made twice.
         */
        if (SwpIsPresent(FullInstanceId) ||
            !SetupDiOpenDeviceInfoW(DevInfo, FullInstanceId, NULL, 0, &DevInfoData))
        {
            goto cleanup;
        }
        Error = ERROR_SUCCESS;
        if (pCreateInfo->pszDeviceDescription)
        {
            SetupDiSetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_DEVICEDESC,
                                              (const BYTE *)pCreateInfo->pszDeviceDescription,
                                              (DWORD)((wcslen(pCreateInfo->pszDeviceDescription) + 1) * sizeof(WCHAR)));
        }
    }

    /* Hardware IDs (a double-null-terminated multi-string) */
    if (pCreateInfo->pszzHardwareIds && pCreateInfo->pszzHardwareIds[0])
    {
        for (HwidLen = 0; ; HwidLen++)
            if (pCreateInfo->pszzHardwareIds[HwidLen] == L'\0' &&
                pCreateInfo->pszzHardwareIds[HwidLen + 1] == L'\0')
            {
                HwidLen += 2;
                break;
            }
        if (!SetupDiSetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_HARDWAREID,
                                               (const BYTE *)pCreateInfo->pszzHardwareIds,
                                               HwidLen * sizeof(WCHAR)))
        {
            Error = GetLastError();
            goto cleanup;
        }
    }

    /* Register the device so the device node exists in the kernel */
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
    {
        Error = GetLastError();
        goto cleanup;
    }

    /* Apply the properties the caller asked for */
    for (i = 0; i < cPropertyCount; i++)
    {
        SetupDiSetDevicePropertyW(DevInfo, &DevInfoData, &pProperties[i].CompKey.Key,
                                  pProperties[i].Type,
                                  (const BYTE *)pProperties[i].Buffer, pProperties[i].BufferSize, 0);
    }

    /* Install a driver, if the device requires one */
    if ((pCreateInfo->CapabilityFlags & SWDeviceCapabilitiesDriverRequired) &&
        pCreateInfo->pszzHardwareIds && pCreateInfo->pszzHardwareIds[0])
    {
        Error = SwpInstallFromStore(DevInfo, &DevInfoData);
        if (Error != ERROR_SUCCESS)
            goto cleanup;
    }

cleanup:
    HeapFree(GetProcessHeap(), 0, Hwids);

    Result = (Error == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(Error);

    if (DevInfo != INVALID_HANDLE_VALUE)
    {
        if (Error != ERROR_SUCCESS)
            SetupDiCallClassInstaller(DIF_REMOVE, DevInfo, &DevInfoData);
        SetupDiDestroyDeviceInfoList(DevInfo);
    }

    /* Hand the result and the instance id to the caller's callback, as Windows does */
    pCallback((HSWDEVICE)SwDevice, Result, pContext,
              (Error == ERROR_SUCCESS) ? SwDevice->InstanceId : NULL);

    if (Error != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, SwDevice);
        return Result;
    }

    *phSwDevice = (HSWDEVICE)SwDevice;
    return S_OK;
}

/***********************************************************************
 *		SwDeviceClose (SETUPAPI.@)
 */
VOID
WINAPI
SwDeviceClose(
    _In_ HSWDEVICE hSwDevice)
{
    PSW_DEVICE SwDevice = (PSW_DEVICE)hSwDevice;

    TRACE("SwDeviceClose(%p)\n", hSwDevice);

    if (!SwDevice || SwDevice->Magic != SW_DEVICE_MAGIC)
        return;

    /*
     * The default lifetime ends the device with its handle: it leaves the
     * device tree, but keeps its keys until it is uninstalled (DIF_REMOVE).
     */
    if (SwDevice->Lifetime == SWDeviceLifetimeHandle)
        SwpRemoveFromTree(SwDevice->InstanceId);

    SwDevice->Magic = 0;
    HeapFree(GetProcessHeap(), 0, SwDevice);
}

/***********************************************************************
 *		SwDeviceSetLifetime (SETUPAPI.@)
 */
HRESULT
WINAPI
SwDeviceSetLifetime(
    _In_ HSWDEVICE hSwDevice,
    _In_ SW_DEVICE_LIFETIME Lifetime)
{
    PSW_DEVICE SwDevice = (PSW_DEVICE)hSwDevice;

    TRACE("SwDeviceSetLifetime(%p %u)\n", hSwDevice, Lifetime);

    if (!SwDevice || SwDevice->Magic != SW_DEVICE_MAGIC)
        return E_HANDLE;
    if (Lifetime >= SWDeviceLifetimeMax)
        return E_INVALIDARG;

    SwDevice->Lifetime = Lifetime;
    return S_OK;
}

/***********************************************************************
 *		SwDeviceGetLifetime (SETUPAPI.@)
 */
HRESULT
WINAPI
SwDeviceGetLifetime(
    _In_ HSWDEVICE hSwDevice,
    _Out_ PSW_DEVICE_LIFETIME pLifetime)
{
    PSW_DEVICE SwDevice = (PSW_DEVICE)hSwDevice;

    TRACE("SwDeviceGetLifetime(%p %p)\n", hSwDevice, pLifetime);

    if (!SwDevice || SwDevice->Magic != SW_DEVICE_MAGIC)
        return E_HANDLE;
    if (!pLifetime)
        return E_INVALIDARG;

    *pLifetime = SwDevice->Lifetime;
    return S_OK;
}

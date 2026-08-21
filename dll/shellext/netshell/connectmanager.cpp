/*
 * PROJECT:     ReactOS Shell
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     CNetConnectionManager class
 * COPYRIGHT:   Copyright 2008 Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddndis.h>

VOID NormalizeOperStatus(MIB_IFROW *IfEntry, NETCON_PROPERTIES * Props);

/*
 * Fallback for when the miniport device cannot be opened - a legacy miniport
 * with no user-mode surface, or a caller without access to it: use what the
 * adapter's INF declared.  A Native 802.11 miniport is installed with
 * Ndi\Interfaces\LowerRange "wlan", and netcfg records the standardized
 * *PhysicalMediaType keyword for adapters whose INF sets it.  Both describe
 * the medium, not the device's name or its id.
 */
static BOOL
NcIsWirelessAdapterFromRegistry(LPCWSTR pszNetCfgInstanceId)
{
    static const WCHAR szClassKey[] =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
    HKEY hClass, hInstance, hNdi;
    WCHAR szSubKey[64], szValue[64];
    DWORD dwIndex, dwSize, dwType, dwMedium;
    BOOL bWireless = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szClassKey, 0, KEY_READ, &hClass) != ERROR_SUCCESS)
        return FALSE;

    for (dwIndex = 0; ; dwIndex++)
    {
        dwSize = ARRAYSIZE(szSubKey);
        if (RegEnumKeyExW(hClass, dwIndex, szSubKey, &dwSize, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (RegOpenKeyExW(hClass, szSubKey, 0, KEY_READ, &hInstance) != ERROR_SUCCESS)
            continue;

        dwSize = sizeof(szValue);
        if (RegQueryValueExW(hInstance, L"NetCfgInstanceId", NULL, &dwType,
                             (LPBYTE)szValue, &dwSize) != ERROR_SUCCESS ||
            dwSize < sizeof(WCHAR))
        {
            RegCloseKey(hInstance);
            continue;
        }
        szValue[ARRAYSIZE(szValue) - 1] = L'\0';
        if (_wcsicmp(szValue, pszNetCfgInstanceId) != 0)
        {
            RegCloseKey(hInstance);
            continue;
        }

        /* The standardized *PhysicalMediaType keyword, when the INF set it. */
        dwSize = sizeof(dwMedium);
        if (RegQueryValueExW(hInstance, L"*PhysicalMediaType", NULL, &dwType,
                             (LPBYTE)&dwMedium, &dwSize) == ERROR_SUCCESS &&
            dwType == REG_DWORD &&
            (dwMedium == NdisPhysicalMediumWirelessLan ||
             dwMedium == NdisPhysicalMediumNative802_11))
        {
            bWireless = TRUE;
        }

        /* Otherwise the lower binding interface the INF declared. */
        if (!bWireless &&
            RegOpenKeyExW(hInstance, L"Ndi\\Interfaces", 0, KEY_READ, &hNdi) == ERROR_SUCCESS)
        {
            dwSize = sizeof(szValue);
            if (RegQueryValueExW(hNdi, L"LowerRange", NULL, &dwType,
                                 (LPBYTE)szValue, &dwSize) == ERROR_SUCCESS &&
                dwSize >= sizeof(WCHAR))
            {
                szValue[ARRAYSIZE(szValue) - 1] = L'\0';
                bWireless = (StrStrIW(szValue, L"wlan") != NULL ||
                             StrStrIW(szValue, L"nativewifi") != NULL);
            }
            RegCloseKey(hNdi);
        }

        RegCloseKey(hInstance);
        break;
    }

    RegCloseKey(hClass);
    return bWireless;
}

/*
 * Is this adapter an 802.11 radio?
 *
 * The authoritative answer is the adapter's NDIS physical medium, and the
 * only way to get it for *every* kind of adapter is to ask the miniport
 * itself.  GetIfEntry()'s dwType is not enough: tcpip.sys only translates
 * NdisPhysicalMediumNative802_11 into IF_TYPE_IEEE80211 when it is compiled
 * for Vista+, and a Native 802.11 (dot11) miniport is not bound to tcpip at
 * all - the nwifi intermediate driver presents a separate virtual 802.3
 * miniport upwards.  NDISUIO is no help either, it binds NdisMedium802_3
 * only.  Every NDIS miniport FDO on the other hand is named
 * \Device\{NetCfgInstanceId} and answers IOCTL_NDIS_QUERY_GLOBAL_STATS, so
 * query OID_GEN_PHYSICAL_MEDIUM there.  That is the same bit the dot11 OID
 * surface is keyed off, and it involves neither the adapter's name nor a
 * hardcoded device id.
 */
BOOL
NcIsWirelessAdapter(LPCWSTR pszNetCfgInstanceId)
{
    WCHAR szDevice[MAX_PATH];
    HANDLE hAdapter;
    NDIS_OID Oid = OID_GEN_PHYSICAL_MEDIUM;
    ULONG Medium = 0;
    DWORD dwReturned = 0;
    BOOL bWireless = FALSE;
    BOOL bAnswered = FALSE;

    if (!pszNetCfgInstanceId || !*pszNetCfgInstanceId)
        return FALSE;

    /* The miniport FDO lives in the object namespace, not below \??, so it
     * has to be reached through GLOBALROOT. */
    if (FAILED(StringCchPrintfW(szDevice, ARRAYSIZE(szDevice),
                                L"\\\\?\\GLOBALROOT\\Device\\%s",
                                pszNetCfgInstanceId)))
    {
        return FALSE;
    }

    hAdapter = CreateFileW(szDevice, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (hAdapter != INVALID_HANDLE_VALUE)
    {
        if (DeviceIoControl(hAdapter, IOCTL_NDIS_QUERY_GLOBAL_STATS,
                            &Oid, sizeof(Oid), &Medium, sizeof(Medium),
                            &dwReturned, NULL) && dwReturned >= sizeof(ULONG))
        {
            /* NdisPhysicalMediumWirelessLan is the NDIS 5 "802.11 looks like
             * ethernet" model, NdisPhysicalMediumNative802_11 the NDIS 6 one. */
            bWireless = (Medium == NdisPhysicalMediumWirelessLan ||
                         Medium == NdisPhysicalMediumNative802_11);
            bAnswered = TRUE;
        }
        CloseHandle(hAdapter);
    }

    if (!bAnswered)
        bWireless = NcIsWirelessAdapterFromRegistry(pszNetCfgInstanceId);

    return bWireless;
}

BOOL
NcIsWirelessAdapter(REFGUID guidId)
{
    WCHAR szGuid[40];

    if (!StringFromGUID2(guidId, szGuid, ARRAYSIZE(szGuid)))
        return FALSE;

    return NcIsWirelessAdapter(szGuid);
}

/***************************************************************
 * INetConnection Interface
 */

HRESULT
WINAPI
CNetConnection::Initialize(PINetConnectionItem pItem)
{
    m_Props = pItem->Props;
    m_dwAdapterIndex = pItem->dwAdapterIndex;

    if (pItem->Props.pszwName)
    {
        m_Props.pszwName = static_cast<PWSTR>(CoTaskMemAlloc((wcslen(pItem->Props.pszwName)+1)*sizeof(WCHAR)));
        if (m_Props.pszwName)
            wcscpy(m_Props.pszwName, pItem->Props.pszwName);
    }

    if (pItem->Props.pszwDeviceName)
    {
        m_Props.pszwDeviceName = static_cast<PWSTR>(CoTaskMemAlloc((wcslen(pItem->Props.pszwDeviceName)+1)*sizeof(WCHAR)));
        if (m_Props.pszwDeviceName)
            wcscpy(m_Props.pszwDeviceName, pItem->Props.pszwDeviceName);
    }

    return S_OK;
}

CNetConnection::~CNetConnection()
{
    CoTaskMemFree(m_Props.pszwName);
    CoTaskMemFree(m_Props.pszwDeviceName);
}

HRESULT
WINAPI
CNetConnection::Connect()
{
    return E_NOTIMPL;
}

BOOL
FindNetworkAdapter(HDEVINFO hInfo, SP_DEVINFO_DATA *pDevInfo, LPWSTR pGuid)
{
    DWORD dwIndex, dwSize;
    HKEY hSubKey;
    WCHAR szNetCfg[50];
    WCHAR szDetail[200] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";

    dwIndex = 0;
    do
    {
        ZeroMemory(pDevInfo, sizeof(SP_DEVINFO_DATA));
        pDevInfo->cbSize = sizeof(SP_DEVINFO_DATA);

        /* get device info */
        if (!SetupDiEnumDeviceInfo(hInfo, dwIndex++, pDevInfo))
            break;

        /* get device software registry path */
        if (!SetupDiGetDeviceRegistryPropertyW(hInfo, pDevInfo, SPDRP_DRIVER, NULL, (LPBYTE)&szDetail[39], sizeof(szDetail)/sizeof(WCHAR) - 40, &dwSize))
            break;

        /* open device registry key */
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szDetail, 0, KEY_READ, &hSubKey) != ERROR_SUCCESS)
            break;

        /* query NetCfgInstanceId for current device */
        dwSize = sizeof(szNetCfg);
        if (RegQueryValueExW(hSubKey, L"NetCfgInstanceId", NULL, NULL, (LPBYTE)szNetCfg, &dwSize) != ERROR_SUCCESS)
        {
            RegCloseKey(hSubKey);
            break;
        }
        RegCloseKey(hSubKey);
        if (!_wcsicmp(pGuid, szNetCfg))
        {
            return TRUE;
        }
    } while (TRUE);

    return FALSE;
}

HRESULT
WINAPI
CNetConnection::Disconnect()
{
    HKEY hKey;
    NETCON_PROPERTIES * pProperties;
    LPOLESTR pDisplayName;
    WCHAR szPath[200];
    DWORD dwSize, dwType;
    LPWSTR pPnp;
    HDEVINFO hInfo;
    SP_DEVINFO_DATA DevInfo;
    SP_PROPCHANGE_PARAMS PropChangeParams;
    HRESULT hr;

    hr = GetProperties(&pProperties);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    hInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT );
    if (!hInfo)
    {
        NcFreeNetconProperties(pProperties);
        return E_FAIL;
    }

    if (FAILED(StringFromCLSID((CLSID)pProperties->guidId, &pDisplayName)))
    {
        NcFreeNetconProperties(pProperties);
        SetupDiDestroyDeviceInfoList(hInfo);
        return E_FAIL;
    }
    NcFreeNetconProperties(pProperties);

    if (FindNetworkAdapter(hInfo, &DevInfo, pDisplayName))
    {
        PropChangeParams.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        PropChangeParams.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE; //;
        PropChangeParams.StateChange = DICS_DISABLE;
        PropChangeParams.Scope = DICS_FLAG_CONFIGSPECIFIC;
        PropChangeParams.HwProfile = 0;

        if (!SetupDiSetClassInstallParams(hInfo, &DevInfo, &PropChangeParams.ClassInstallHeader, sizeof(SP_PROPCHANGE_PARAMS)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hInfo, &DevInfo))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    SetupDiDestroyDeviceInfoList(hInfo);

    _swprintf(szPath, L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%s\\Connection", pDisplayName);
    CoTaskMemFree(pDisplayName);

    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return E_FAIL;

    dwSize = 0;
    if (RegQueryValueExW(hKey, L"PnpInstanceID", NULL, &dwType, NULL, &dwSize) != ERROR_SUCCESS || dwType != REG_SZ)
    {
        RegCloseKey(hKey);
        return E_FAIL;
    }

    pPnp = static_cast<PWSTR>(CoTaskMemAlloc(dwSize));
    if (!pPnp)
    {
        RegCloseKey(hKey);
        return E_FAIL;
    }

    if (RegQueryValueExW(hKey, L"PnpInstanceID", NULL, &dwType, (LPBYTE)pPnp, &dwSize) != ERROR_SUCCESS)
    {
        CoTaskMemFree(pPnp);
        RegCloseKey(hKey);
        return E_FAIL;
    }
    RegCloseKey(hKey);

    _swprintf(szPath, L"System\\CurrentControlSet\\Hardware Profiles\\Current\\System\\CurrentControlSet\\Enum\\%s", pPnp);
    CoTaskMemFree(pPnp);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return E_FAIL;

    dwSize = 1; /* enable = 0, disable = 1 */
    RegSetValueExW(hKey, L"CSConfigFlags", 0, REG_DWORD, (LPBYTE)&dwSize, sizeof(DWORD));
    RegCloseKey(hKey);

    return S_OK;
}

HRESULT
WINAPI
CNetConnection::Delete()
{
    return E_NOTIMPL;
}

HRESULT
WINAPI
CNetConnection::Duplicate(
    LPCWSTR pszwDuplicateName,
    INetConnection **ppCon)
{
    return E_NOTIMPL;
}

HRESULT
WINAPI
CNetConnection::GetProperties(NETCON_PROPERTIES **ppProps)
{
    MIB_IFROW IfEntry;
    HKEY hKey;
    LPOLESTR pStr;
    WCHAR szName[140];
    DWORD dwShowIcon, dwNotifyDisconnect, dwType, dwSize;
    NETCON_PROPERTIES * pProperties;
    HRESULT hr;

    if (!ppProps)
        return E_POINTER;

    pProperties = static_cast<NETCON_PROPERTIES*>(CoTaskMemAlloc(sizeof(NETCON_PROPERTIES)));
    if (!pProperties)
        return E_OUTOFMEMORY;

    CopyMemory(pProperties, &m_Props, sizeof(NETCON_PROPERTIES));
    pProperties->pszwName = NULL;

    /* Hand out our own copy of the cached name. Every exit path below - and
     * there are several that leave the registry lookup untouched - must return
     * a complete NETCON_PROPERTIES, because callers dereference pszwName. */
    if (m_Props.pszwName)
    {
        pProperties->pszwName = static_cast<LPWSTR>(CoTaskMemAlloc((wcslen(m_Props.pszwName)+1)*sizeof(WCHAR)));
        if (pProperties->pszwName)
            wcscpy(pProperties->pszwName, m_Props.pszwName);
    }

    if (m_Props.pszwDeviceName)
    {
        pProperties->pszwDeviceName = static_cast<LPWSTR>(CoTaskMemAlloc((wcslen(m_Props.pszwDeviceName)+1)*sizeof(WCHAR)));
        if (pProperties->pszwDeviceName)
            wcscpy(pProperties->pszwDeviceName, m_Props.pszwDeviceName);
    }

    *ppProps = pProperties;

    /* get updated adapter characteristics */
    ZeroMemory(&IfEntry, sizeof(IfEntry));
    IfEntry.dwIndex = m_dwAdapterIndex;
    if (GetIfEntry(&IfEntry) != NO_ERROR)
        return NOERROR;

    NormalizeOperStatus(&IfEntry, pProperties);


    hr = StringFromCLSID((CLSID)m_Props.guidId, &pStr);
    if (SUCCEEDED(hr))
    {
        wcscpy(szName, L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\");
        wcscat(szName, pStr);
        wcscat(szName, L"\\Connection");

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szName, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            dwSize = sizeof(dwShowIcon);
            if (RegQueryValueExW(hKey, L"ShowIcon", NULL, &dwType, (LPBYTE)&dwShowIcon, &dwSize) == ERROR_SUCCESS && dwType == REG_DWORD)
            {
                if (dwShowIcon)
                    pProperties->dwCharacter |= NCCF_SHOW_ICON;
                else
                    pProperties->dwCharacter &= ~NCCF_SHOW_ICON;
            }

            dwSize = sizeof(dwNotifyDisconnect);
            if (RegQueryValueExW(hKey, L"IpCheckingEnabled", NULL, &dwType, (LPBYTE)&dwNotifyDisconnect, &dwSize) == ERROR_SUCCESS && dwType == REG_DWORD)
            {
                if (dwNotifyDisconnect)
                    pProperties->dwCharacter |= NCCF_NOTIFY_DISCONNECTED;
                else
                    pProperties->dwCharacter &= ~NCCF_NOTIFY_DISCONNECTED;
            }

            dwSize = sizeof(szName);
            if (RegQueryValueExW(hKey, L"Name", NULL, &dwType, (LPBYTE)szName, &dwSize) == ERROR_SUCCESS)
            {
                /* use updated name, keep the cached one if we cannot store it */
                PWSTR pszUpdatedName;

                dwSize = wcslen(szName) + 1;
                pszUpdatedName = static_cast<PWSTR>(CoTaskMemAlloc(dwSize * sizeof(WCHAR)));
                if (pszUpdatedName)
                {
                    CopyMemory(pszUpdatedName, szName, dwSize * sizeof(WCHAR));
                    CoTaskMemFree(pProperties->pszwName);
                    pProperties->pszwName = pszUpdatedName;
                }
            }
            RegCloseKey(hKey);
        }
        CoTaskMemFree(pStr);
    }

    /* Enable 'Rename' and 'Delete' for Adminstrators only */
    if (IsUserAdmin())
    {
        pProperties->dwCharacter |= NCCF_ALLOW_RENAME;

        /* Virtual network interfaces can be deleted */
        if (IfEntry.dwType == IF_TYPE_TUNNEL)
        {
            pProperties->dwCharacter |= NCCF_ALLOW_REMOVAL;
        }
    }
    else
    {
        pProperties->dwCharacter &= ~(NCCF_ALLOW_RENAME | NCCF_ALLOW_REMOVAL);
    }

    return S_OK;
}

HRESULT
WINAPI
CNetConnection::GetUiObjectClassId(CLSID *pclsid)
{
    if (m_Props.MediaType == NCM_LAN)
    {
        CopyMemory(pclsid, &CLSID_LanConnectionUi, sizeof(CLSID));
        return S_OK;
    }

    return E_NOTIMPL;
}

HRESULT
WINAPI
CNetConnection::Rename(LPCWSTR pszwDuplicateName)
{
    WCHAR szName[140];
    LPOLESTR pStr;
    DWORD dwSize;
    HKEY hKey;
    HRESULT hr;

    if (pszwDuplicateName == NULL || wcslen(pszwDuplicateName) == 0)
        return S_OK;

    if (m_Props.pszwName)
    {
        CoTaskMemFree(m_Props.pszwName);
        m_Props.pszwName = NULL;
    }

    dwSize = (wcslen(pszwDuplicateName) + 1) * sizeof(WCHAR);
    m_Props.pszwName = static_cast<PWSTR>(CoTaskMemAlloc(dwSize));
    if (m_Props.pszwName == NULL)
        return E_OUTOFMEMORY;

    wcscpy(m_Props.pszwName, pszwDuplicateName);

    hr = StringFromCLSID((CLSID)m_Props.guidId, &pStr);
    if (SUCCEEDED(hr))
    {
        wcscpy(szName, L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\");
        wcscat(szName, pStr);
        wcscat(szName, L"\\Connection");

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szName, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS)
        {
            RegSetValueExW(hKey, L"Name", NULL, REG_SZ, (LPBYTE)m_Props.pszwName, dwSize);
            RegCloseKey(hKey);
        }

        CoTaskMemFree(pStr);
    }

    return hr;
}

HRESULT WINAPI CNetConnection_CreateInstance(PINetConnectionItem pItem, REFIID riid, LPVOID * ppv)
{
    return ShellObjectCreatorInit<CNetConnection>(pItem, riid, ppv);
}



CNetConnectionManager::CNetConnectionManager() :
    m_pHead(NULL),
    m_pCurrent(NULL)
{
}

HRESULT
WINAPI
CNetConnectionManager::EnumConnections(
    NETCONMGR_ENUM_FLAGS Flags,
    IEnumNetConnection **ppEnum)
{
    TRACE("EnumConnections\n");

    if (!ppEnum)
        return E_POINTER;

    if (Flags != NCME_DEFAULT)
        return E_FAIL;

    *ppEnum = static_cast<IEnumNetConnection*>(this);
    AddRef();
    return S_OK;
}

/***************************************************************
 * IEnumNetConnection Interface
 */

HRESULT
WINAPI
CNetConnectionManager::Next(
    ULONG celt,
    INetConnection **rgelt,
    ULONG *pceltFetched)
{
    HRESULT hr;

    if (!pceltFetched || !rgelt)
        return E_POINTER;

    if (celt != 1)
        return E_FAIL;

    if (!m_pCurrent)
        return S_FALSE;

    hr = CNetConnection_CreateInstance(m_pCurrent, IID_PPV_ARG(INetConnection, rgelt));
    m_pCurrent = m_pCurrent->Next;

    return hr;
}

HRESULT
WINAPI
CNetConnectionManager::Skip(ULONG celt)
{
    while (m_pCurrent && celt-- > 0)
        m_pCurrent = m_pCurrent->Next;

    if (celt)
       return S_FALSE;
    else
       return S_OK;

}

HRESULT
WINAPI
CNetConnectionManager::Reset()
{
    m_pCurrent = m_pHead;
    return S_OK;
}

HRESULT
WINAPI
CNetConnectionManager::Clone(IEnumNetConnection **ppenum)
{
    return E_NOTIMPL;
}

BOOL
GetAdapterIndexFromNetCfgInstanceId(PIP_ADAPTER_INFO pAdapterInfo, LPWSTR szNetCfg, PDWORD pIndex)
{
    WCHAR szBuffer[50];
    IP_ADAPTER_INFO * pCurrentAdapter;

    pCurrentAdapter = pAdapterInfo;
    while (pCurrentAdapter)
    {
        szBuffer[0] = L'\0';
        if (MultiByteToWideChar(CP_ACP, 0, pCurrentAdapter->AdapterName, -1, szBuffer, sizeof(szBuffer)/sizeof(szBuffer[0])))
        {
            szBuffer[(sizeof(szBuffer)/sizeof(WCHAR))-1] = L'\0';
        }
        if (!_wcsicmp(szBuffer, szNetCfg))
        {
            *pIndex = pCurrentAdapter->Index;
            return TRUE;
        }
        pCurrentAdapter = pCurrentAdapter->Next;
    }
    return FALSE;
}

VOID
NormalizeOperStatus(
    MIB_IFROW *IfEntry,
    NETCON_PROPERTIES    * Props)
{
    switch (IfEntry->dwOperStatus)
    {
        case MIB_IF_OPER_STATUS_NON_OPERATIONAL:
            Props->Status = NCS_HARDWARE_DISABLED;
            break;
        case MIB_IF_OPER_STATUS_UNREACHABLE:
            Props->Status = NCS_DISCONNECTED;
            break;
        case MIB_IF_OPER_STATUS_DISCONNECTED:
            Props->Status = NCS_MEDIA_DISCONNECTED;
            break;
        case MIB_IF_OPER_STATUS_CONNECTING:
            Props->Status = NCS_CONNECTING;
            break;
        case MIB_IF_OPER_STATUS_CONNECTED:
            Props->Status = NCS_CONNECTED;
            break;
        case MIB_IF_OPER_STATUS_OPERATIONAL:
            Props->Status = NCS_CONNECTED;
            break;
        default:
            break;
    }
}

HRESULT
CNetConnectionManager::EnumerateINetConnections()
{
    DWORD dwSize, dwResult, dwIndex, dwAdapterIndex, dwShowIcon, dwNotifyDisconnect;
    MIB_IFTABLE *pIfTable;
    MIB_IFROW IfEntry;
    IP_ADAPTER_INFO * pAdapterInfo;
    HDEVINFO hInfo;
    SP_DEVINFO_DATA DevInfo;
    HKEY hSubKey;
    WCHAR szNetCfg[50];
    WCHAR szAdapterNetCfg[50];
    WCHAR szDetail[200] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
    WCHAR szName[130] = L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
    PINetConnectionItem pCurrent = NULL;

    /* get the IfTable */
    dwSize = 0;
    if (GetIfTable(NULL, &dwSize, TRUE) != ERROR_INSUFFICIENT_BUFFER)
        return E_FAIL;

    pIfTable = static_cast<PMIB_IFTABLE>(CoTaskMemAlloc(dwSize));
    if (!pIfTable)
        return E_OUTOFMEMORY;

    dwResult = GetIfTable(pIfTable, &dwSize, TRUE);
    if (dwResult != NO_ERROR)
    {
        CoTaskMemFree(pIfTable);
        return HRESULT_FROM_WIN32(dwResult);
    }

    dwSize = 0;
    dwResult = GetAdaptersInfo(NULL, &dwSize);
    if (dwResult!= ERROR_BUFFER_OVERFLOW)
    {
        CoTaskMemFree(pIfTable);
        return HRESULT_FROM_WIN32(dwResult);
    }

    pAdapterInfo = static_cast<PIP_ADAPTER_INFO>(CoTaskMemAlloc(dwSize));
    if (!pAdapterInfo)
    {
        CoTaskMemFree(pIfTable);
        return E_OUTOFMEMORY;
    }

    dwResult = GetAdaptersInfo(pAdapterInfo, &dwSize);
    if (dwResult != NO_ERROR)
    {
        CoTaskMemFree(pIfTable);
        CoTaskMemFree(pAdapterInfo);
        return HRESULT_FROM_WIN32(dwResult);
    }

    hInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT );
    if (!hInfo)
    {
        CoTaskMemFree(pIfTable);
        CoTaskMemFree(pAdapterInfo);
        return E_FAIL;
    }

    dwIndex = 0;
    do
    {
        ZeroMemory(&DevInfo, sizeof(SP_DEVINFO_DATA));
        DevInfo.cbSize = sizeof(DevInfo);

        /* get device info */
        if (!SetupDiEnumDeviceInfo(hInfo, dwIndex++, &DevInfo))
            break;

        /* get device software registry path */
        if (!SetupDiGetDeviceRegistryPropertyW(hInfo, &DevInfo, SPDRP_DRIVER, NULL, (LPBYTE)&szDetail[39], sizeof(szDetail)/sizeof(WCHAR) - 40, &dwSize))
            break;

        /* open device registry key */
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szDetail, 0, KEY_READ, &hSubKey) != ERROR_SUCCESS)
            break;

        /* query NetCfgInstanceId for current device */
        dwSize = sizeof(szNetCfg);
        if (RegQueryValueExW(hSubKey, L"NetCfgInstanceId", NULL, NULL, (LPBYTE)szNetCfg, &dwSize) != ERROR_SUCCESS)
        {
            RegCloseKey(hSubKey);
            break;
        }
        RegCloseKey(hSubKey);

        /* get the current adapter index from NetCfgInstanceId */
        if (!GetAdapterIndexFromNetCfgInstanceId(pAdapterInfo, szNetCfg, &dwAdapterIndex))
            continue;

        /* get detailed adapter info */
        ZeroMemory(&IfEntry, sizeof(IfEntry));
        IfEntry.dwIndex = dwAdapterIndex;
        if (GetIfEntry(&IfEntry) != NO_ERROR)
            break;

        /* allocate new INetConnectionItem */
        PINetConnectionItem pNew = static_cast<PINetConnectionItem>(CoTaskMemAlloc(sizeof(INetConnectionItem)));
        if (!pNew)
            break;

        ZeroMemory(pNew, sizeof(INetConnectionItem));
        pNew->dwAdapterIndex = dwAdapterIndex;
        /* store NetCfgInstanceId */
        CLSIDFromString(szNetCfg, &pNew->Props.guidId);
        NormalizeOperStatus(&IfEntry, &pNew->Props);

        switch (IfEntry.dwType)
        {
            case IF_TYPE_ETHERNET_CSMACD:
                pNew->Props.MediaType = NCM_LAN;
                break;
            case IF_TYPE_IEEE80211:
                /* Windows reports a wireless connection as NCM_LAN and
                 * distinguishes it by its sub media type. */
                pNew->Props.MediaType = NCM_LAN;
                break;
            default:
                break;
        }
        /*  open network connections details */
        wcscpy(&szName[80], szNetCfg);
        wcscpy(&szName[118], L"\\Connection");

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS)
        {
            /* retrieve name of connection */
            dwSize = sizeof(szAdapterNetCfg);
            if (RegQueryValueExW(hSubKey, L"Name", NULL, NULL, (LPBYTE)szAdapterNetCfg, &dwSize) == ERROR_SUCCESS)
            {
                pNew->Props.pszwName = static_cast<PWSTR>(CoTaskMemAlloc((wcslen(szAdapterNetCfg)+1) * sizeof(WCHAR)));
                if (pNew->Props.pszwName)
                    wcscpy(pNew->Props.pszwName, szAdapterNetCfg);
            }
            dwSize = sizeof(dwShowIcon);
            if (RegQueryValueExW(hSubKey, L"ShowIcon", NULL, NULL, (LPBYTE)&dwShowIcon, &dwSize) == ERROR_SUCCESS)
            {
                if (dwShowIcon)
                    pNew->Props.dwCharacter |= NCCF_SHOW_ICON;
            }
            dwSize = sizeof(dwNotifyDisconnect);
            if (RegQueryValueExW(hSubKey, L"IpCheckingEnabled", NULL, NULL, (LPBYTE)&dwNotifyDisconnect, &dwSize) == ERROR_SUCCESS)
            {
                if (dwNotifyDisconnect)
                    pNew->Props.dwCharacter |= NCCF_NOTIFY_DISCONNECTED;
            }
            RegCloseKey(hSubKey);
        }

        /* Get the adapter device description */
        dwSize = 0;
        SetupDiGetDeviceRegistryPropertyW(hInfo, &DevInfo, SPDRP_DEVICEDESC, NULL, NULL, 0, &dwSize);
        if (dwSize != 0)
        {
            pNew->Props.pszwDeviceName = static_cast<PWSTR>(CoTaskMemAlloc(dwSize));
            if (pNew->Props.pszwDeviceName)
                SetupDiGetDeviceRegistryPropertyW(hInfo, &DevInfo, SPDRP_DEVICEDESC, NULL, (PBYTE)pNew->Props.pszwDeviceName, dwSize, &dwSize);
        }

        if (pCurrent)
            pCurrent->Next = pNew;
        else
            m_pHead = pNew;

        pCurrent = pNew;
    } while (TRUE);

    CoTaskMemFree(pIfTable);
    CoTaskMemFree(pAdapterInfo);
    SetupDiDestroyDeviceInfoList(hInfo);

    m_pCurrent = m_pHead;
    return (m_pHead != NULL ? S_OK : S_FALSE);
}

HRESULT CNetConnectionManager::Initialize()
{
    HRESULT hr = EnumerateINetConnections();
    if (FAILED_UNEXPECTEDLY(hr))
    {
        /* If something went wrong during the enumeration print an error don't enumerate anything */
        m_pCurrent = m_pHead = NULL;
        return S_FALSE;
    }
    return S_OK;
}

HRESULT WINAPI CNetConnectionManager_CreateInstance(REFIID riid, LPVOID * ppv)
{
#if USE_CUSTOM_CONMGR
    return ShellObjectCreatorInit<CNetConnectionManager>(riid, ppv);
#else
    return CoCreateInstance(CLSID_ConnectionManager, NULL, CLSCTX_ALL, riid, ppv);
#endif
}

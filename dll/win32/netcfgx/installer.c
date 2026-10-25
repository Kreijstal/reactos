/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Configuration of network devices
 * FILE:            dll/win32/netcfgx/installer.c
 * PURPOSE:         Network devices installer
 *
 * PROGRAMMERS:     Hervé Poussineau (hpoussin@reactos.org)
 */

#include "precomp.h"
#include <strsafe.h>


/* Append a REG_SZ to an existing REG_MULTI_SZ string in the registry.
 * If the value doesn't exist, create it.
 * Returns ERROR_SUCCESS if success. Otherwise, returns an error code
 */
static
LONG
AppendStringToMultiSZ(
    IN HKEY hKey,
    IN PCWSTR ValueName,
    IN PCWSTR ValueToAppend)
{
    PWSTR Buffer = NULL;
    DWORD dwRegType;
    DWORD dwRequired, dwLength;
    DWORD dwTmp;
    LONG rc;

    rc = RegQueryValueExW(hKey,
                          ValueName,
                          NULL,
                          &dwRegType,
                          NULL,
                          &dwRequired);
    if (rc != ERROR_FILE_NOT_FOUND)
    {
        if (rc != ERROR_SUCCESS)
            goto cleanup;
        if (dwRegType != REG_MULTI_SZ)
        {
            rc = ERROR_GEN_FAILURE;
            goto cleanup;
        }

        dwTmp = dwLength = dwRequired + wcslen(ValueToAppend) * sizeof(WCHAR) + sizeof(UNICODE_NULL);
        Buffer = HeapAlloc(GetProcessHeap(), 0, dwLength);
        if (!Buffer)
        {
            rc = ERROR_NOT_ENOUGH_MEMORY;
            goto cleanup;
        }

        rc = RegQueryValueExW(hKey,
                              ValueName,
                              NULL,
                              NULL,
                              (BYTE*)Buffer,
                              &dwTmp);
        if (rc != ERROR_SUCCESS)
            goto cleanup;
    }
    else
    {
        dwRequired = sizeof(WCHAR);
        dwLength = wcslen(ValueToAppend) * sizeof(WCHAR) + 2 * sizeof(UNICODE_NULL);
        Buffer = HeapAlloc(GetProcessHeap(), 0, dwLength);
        if (!Buffer)
        {
            rc = ERROR_NOT_ENOUGH_MEMORY;
            goto cleanup;
        }
    }

    /* Append the value */
    wcscpy(&Buffer[dwRequired / sizeof(WCHAR) - 1], ValueToAppend);
    /* Terminate the REG_MULTI_SZ string */
    Buffer[dwLength / sizeof(WCHAR) - 1] = UNICODE_NULL;

    rc = RegSetValueExW(hKey,
                        ValueName,
                        0,
                        REG_MULTI_SZ,
                        (const BYTE*)Buffer,
                        dwLength);

cleanup:
    HeapFree(GetProcessHeap(), 0, Buffer);
    return rc;
}

static
DWORD
GetUniqueConnectionName(
    _In_ HKEY hNetworkKey,
    _Out_ PWSTR *ppszNameBuffer)
{
    int Length = 0;
    PWSTR pszDefaultName = NULL;
    PWSTR pszNameBuffer = NULL;
    DWORD dwSubKeys = 0;
    DWORD dwError;

    TRACE("GetNewConnectionName()\n");

    Length = LoadStringW(netcfgx_hInstance, IDS_NET_CONNECT, (LPWSTR)&pszDefaultName, 0);
    if (Length == 0)
    {
        pszDefaultName = L"Network Connection";
        Length = wcslen(pszDefaultName);
    }

    TRACE("Length %d\n", Length);

    dwError = RegQueryInfoKeyW(hNetworkKey,
                               NULL, NULL, NULL, &dwSubKeys,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    if (dwError != ERROR_SUCCESS)
    {
        ERR("RegQueryInfoKeyW: Error %lu\n", dwError);
        return dwError;
    }

    TRACE("Adapter Count: %lu\n", dwSubKeys);

    pszNameBuffer = HeapAlloc(GetProcessHeap(),
                              HEAP_ZERO_MEMORY,
                              (Length + ((dwSubKeys != 0) ? 6 : 1)) * sizeof(WCHAR));
    if (pszNameBuffer == NULL)
    {
        return ERROR_OUTOFMEMORY;
    }

    if (dwSubKeys != 0)
        StringCchPrintfW(pszNameBuffer, Length + 6, L"%.*s %lu", Length, pszDefaultName, dwSubKeys + 1);
    else
        StringCchPrintfW(pszNameBuffer, Length + 1, L"%.*s", Length, pszDefaultName);

    TRACE("Adapter Name: %S\n", pszNameBuffer);

    *ppszNameBuffer = pszNameBuffer;

    return ERROR_SUCCESS;
}

/*
 * Standardized keywords a network adapter INF sets in its DDInstall section
 * rather than with AddReg. They go to the driver key, where NDIS and the
 * network adapter class extension read them; a NetAdapterCx client like
 * ovpn-dco tells its medium (*MediaType = NdisMediumIP) this way.
 */
static const PCWSTR NetDDInstallKeywords[] =
{
    L"*IfType",
    L"*MediaType",
    L"*PhysicalMediaType",
    L"*IfConnectorPresent",
    L"*ConnectionType",
    L"*DirectionType",
    L"*AccessType",
    L"*HardwareLoopback",
};

#define NET_DDINSTALL_KEYWORD_COUNT (sizeof(NetDDInstallKeywords) / sizeof(NetDDInstallKeywords[0]))

typedef struct _NET_DDINSTALL_KEYWORDS
{
    BOOL Present[NET_DDINSTALL_KEYWORD_COUNT];
    DWORD Value[NET_DDINSTALL_KEYWORD_COUNT];
} NET_DDINSTALL_KEYWORDS, *PNET_DDINSTALL_KEYWORDS;

static
VOID
ReadNetDDInstallKeywords(
    IN HINF hInf,
    IN PCWSTR SectionName,
    OUT PNET_DDINSTALL_KEYWORDS Keywords)
{
    INFCONTEXT InfContext;
    INT Value;
    ULONG i;

    ZeroMemory(Keywords, sizeof(*Keywords));
    for (i = 0; i < NET_DDINSTALL_KEYWORD_COUNT; i++)
    {
        if (SetupFindFirstLineW(hInf, SectionName, NetDDInstallKeywords[i], &InfContext) &&
            SetupGetIntField(&InfContext, 1, &Value))
        {
            Keywords->Present[i] = TRUE;
            Keywords->Value[i] = (DWORD)Value;
        }
    }
}

#define NET_LUID_INDEX_FIRST 0x8000 /* the first one Windows gives, too */
#define NET_LUID_INDEX_LAST  0xFFFFFF

/*
 * Open the software key of every other network adapter in turn:
 * Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\nnnn, except
 * OwnKeyName (the nnnn of the adapter being installed).
 */
static
HKEY
OpenNextNetAdapterKey(
    IN HKEY ClassKey,
    IN PCWSTR OwnKeyName OPTIONAL,
    IN OUT PDWORD Index)
{
    WCHAR Name[32];
    DWORD Length;
    HKEY Key;

    for (;;)
    {
        Length = ARRAYSIZE(Name);
        if (RegEnumKeyExW(ClassKey, (*Index)++, Name, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            return NULL;
        if (OwnKeyName && !_wcsicmp(Name, OwnKeyName))
            continue;
        if (RegOpenKeyExW(ClassKey, Name, 0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
            return Key;
    }
}

static
HKEY
OpenNetClassKey(VOID)
{
    HKEY ClassKey;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
                      0, KEY_ENUMERATE_SUB_KEYS, &ClassKey) != ERROR_SUCCESS)
        return NULL;
    return ClassKey;
}

static
DWORD
QueryDword(
    IN HKEY Key,
    IN PCWSTR Name,
    IN DWORD Default)
{
    DWORD Type, Value, Size = sizeof(Value);

    if (RegQueryValueExW(Key, Name, NULL, &Type, (PBYTE)&Value, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD || Size != sizeof(Value))
        return Default;
    return Value;
}

/* The nnnn of the software key of the device */
static
BOOL
GetOwnKeyName(
    IN HDEVINFO DeviceInfoSet,
    IN PSP_DEVINFO_DATA DeviceInfoData,
    OUT PWSTR Name,
    IN DWORD Chars)
{
    WCHAR Driver[MAX_PATH];
    PWSTR Slash;

    if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, SPDRP_DRIVER, NULL,
                                           (PBYTE)Driver, sizeof(Driver), NULL))
        return FALSE;
    Slash = wcsrchr(Driver, L'\\');
    if (!Slash || wcslen(Slash + 1) >= Chars)
        return FALSE;
    wcscpy(Name, Slash + 1);
    return TRUE;
}

/* Whether another adapter has this NetCfgInstanceId already */
static
BOOL
IsNetCfgInstanceIdTaken(
    IN PCWSTR OwnKeyName OPTIONAL,
    IN PCWSTR InstanceId)
{
    WCHAR Value[64];
    DWORD Index = 0, Type, Size;
    HKEY ClassKey, Key;
    BOOL Taken = FALSE;

    ClassKey = OpenNetClassKey();
    if (!ClassKey)
        return FALSE;
    while (!Taken && (Key = OpenNextNetAdapterKey(ClassKey, OwnKeyName, &Index)) != NULL)
    {
        Size = sizeof(Value) - sizeof(WCHAR);
        ZeroMemory(Value, sizeof(Value));
        if (RegQueryValueExW(Key, L"NetCfgInstanceId", NULL, &Type, (PBYTE)Value, &Size) == ERROR_SUCCESS &&
            Type == REG_SZ && !_wcsicmp(Value, InstanceId))
        {
            Taken = TRUE;
        }
        RegCloseKey(Key);
    }
    RegCloseKey(ClassKey);
    return Taken;
}

/*
 * The NetCfgInstanceId of a network adapter, as Windows picks it: the one
 * it has from an earlier installation, else the SuggestedInstanceId that
 * its installer stored in its software key (Wintun does), else a new one.
 * Returns FALSE to make a new one.
 */
static
BOOL
GetExistingNetCfgInstanceId(
    IN HDEVINFO DeviceInfoSet,
    IN PSP_DEVINFO_DATA DeviceInfoData,
    OUT PWSTR InstanceId,
    IN DWORD Chars)
{
    WCHAR OwnKeyName[32];
    PCWSTR OwnName = NULL;
    DWORD Type, Size;
    GUID Suggested;
    HKEY Key;
    BOOL Found = FALSE;

    Key = SetupDiOpenDevRegKey(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (Key == INVALID_HANDLE_VALUE)
        return FALSE;
    if (GetOwnKeyName(DeviceInfoSet, DeviceInfoData, OwnKeyName, ARRAYSIZE(OwnKeyName)))
        OwnName = OwnKeyName;

    Size = (Chars - 1) * sizeof(WCHAR);
    ZeroMemory(InstanceId, Chars * sizeof(WCHAR));
    if (RegQueryValueExW(Key, L"NetCfgInstanceId", NULL, &Type, (PBYTE)InstanceId, &Size) == ERROR_SUCCESS &&
        Type == REG_SZ && InstanceId[0] == L'{')
    {
        Found = TRUE;
    }
    else
    {
        Size = sizeof(Suggested);
        if (RegQueryValueExW(Key, L"SuggestedInstanceId", NULL, &Type, (PBYTE)&Suggested, &Size) == ERROR_SUCCESS &&
            Type == REG_BINARY && Size == sizeof(Suggested) &&
            StringFromGUID2(&Suggested, InstanceId, Chars))
        {
            Found = TRUE;
        }
    }
    RegCloseKey(Key);

    if (Found && IsNetCfgInstanceIdTaken(OwnName, InstanceId))
    {
        WARN("NetCfgInstanceId %S is taken\n", InstanceId);
        Found = FALSE;
    }
    return Found;
}

/*
 * Give the adapter the NetLuidIndex of its interface LUID, unless it has
 * one: the lowest that no other adapter of the same *IfType has.
 */
static
DWORD
AssignNetLuidIndex(
    IN HDEVINFO DeviceInfoSet,
    IN PSP_DEVINFO_DATA DeviceInfoData,
    IN HKEY hKey)
{
    WCHAR OwnKeyName[32];
    PCWSTR OwnName = NULL;
    DWORD IfType, Candidate, Index;
    HKEY ClassKey, Key;
    BOOL Taken;

    if (QueryDword(hKey, L"NetLuidIndex", 0) != 0)
        return ERROR_SUCCESS;

    IfType = QueryDword(hKey, L"*IfType", 6 /* IF_TYPE_ETHERNET_CSMACD */);
    if (GetOwnKeyName(DeviceInfoSet, DeviceInfoData, OwnKeyName, ARRAYSIZE(OwnKeyName)))
        OwnName = OwnKeyName;

    ClassKey = OpenNetClassKey();
    for (Candidate = NET_LUID_INDEX_FIRST; Candidate <= NET_LUID_INDEX_LAST; Candidate++)
    {
        Taken = FALSE;
        Index = 0;
        while (ClassKey && !Taken && (Key = OpenNextNetAdapterKey(ClassKey, OwnName, &Index)) != NULL)
        {
            Taken = QueryDword(Key, L"*IfType", 6 /* IF_TYPE_ETHERNET_CSMACD */) == IfType &&
                    QueryDword(Key, L"NetLuidIndex", 0) == Candidate;
            RegCloseKey(Key);
        }
        if (!Taken)
            break;
    }
    if (ClassKey)
        RegCloseKey(ClassKey);
    if (Candidate > NET_LUID_INDEX_LAST)
        return ERROR_NO_MORE_ITEMS;

    TRACE("NetLuidIndex 0x%lx for interface type %lu\n", Candidate, IfType);
    return RegSetValueExW(hKey, L"NetLuidIndex", 0, REG_DWORD, (const BYTE *)&Candidate, sizeof(Candidate));
}

static
DWORD
InstallNetDevice(
    IN HDEVINFO DeviceInfoSet,
    IN PSP_DEVINFO_DATA DeviceInfoData,
    LPCWSTR UuidString,
    DWORD Characteristics,
    LPCWSTR BusType,
    PNET_DDINSTALL_KEYWORDS Keywords)
{
    SP_DEVINSTALL_PARAMS_W DeviceInstallParams;
    LPWSTR InstanceId = NULL;
    LPWSTR ComponentId = NULL;
    LPWSTR DeviceName = NULL;
    LPWSTR ExportName = NULL;
    LONG rc;
    HKEY hInterfacesKey = NULL, hInterfaceKey = NULL;
    HKEY hKey = NULL;
    HKEY hNetworkKey = NULL;
    HKEY hLinkageKey = NULL;
    HKEY hConnectionKey = NULL;
    DWORD dwShowIcon, dwLength, dwValue;
    ULONG i;
    PWSTR pszNameBuffer = NULL;
    DWORD dwRegType;

    DeviceInstallParams.cbSize = sizeof(DeviceInstallParams);
    if (!SetupDiGetDeviceInstallParamsW(DeviceInfoSet,
                                        DeviceInfoData,
                                        &DeviceInstallParams))
    {
        rc = GetLastError();
        ERR("SetupDiGetDeviceInstallParamsW() failed (Error %lu)\n", rc);
        goto cleanup;
    }

    /* Do not start the adapter in the call to SetupDiInstallDevice */
    DeviceInstallParams.Flags |= DI_DONOTCALLCONFIGMG;

    if (!SetupDiSetDeviceInstallParamsW(DeviceInfoSet,
                                        DeviceInfoData,
                                        &DeviceInstallParams))
    {
        rc = GetLastError();
        ERR("SetupDiSetDeviceInstallParamsW() failed (Error %lu)\n", rc);
        goto cleanup;
    }

    /* Install the adapter */
    if (!SetupDiInstallDevice(DeviceInfoSet, DeviceInfoData))
    {
        rc = GetLastError();
        ERR("SetupDiInstallDevice() failed (Error %lu)\n", rc);
        goto cleanup;
    }

    /* Get Instance ID */
    if (SetupDiGetDeviceInstanceIdW(DeviceInfoSet, DeviceInfoData, NULL, 0, &dwLength))
    {
        ERR("SetupDiGetDeviceInstanceIdW() returned TRUE. FALSE expected\n");
        rc = ERROR_GEN_FAILURE;
        goto cleanup;
    }

    InstanceId = HeapAlloc(GetProcessHeap(), 0, dwLength * sizeof(WCHAR));
    if (!InstanceId)
    {
        ERR("HeapAlloc() failed\n");
        rc = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, DeviceInfoData, InstanceId, dwLength, NULL))
    {
        rc = GetLastError();
        ERR("SetupDiGetDeviceInstanceIdW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    /* Create device name */
    DeviceName = HeapAlloc(GetProcessHeap(), 0, (wcslen(L"\\Device\\") + wcslen(UuidString)) * sizeof(WCHAR) + sizeof(UNICODE_NULL));
    if (!DeviceName)
    {
        ERR("HeapAlloc() failed\n");
        rc = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    wcscpy(DeviceName, L"\\Device\\");
    wcscat(DeviceName, UuidString);

    /* Create export name */
    ExportName = HeapAlloc(GetProcessHeap(), 0, (wcslen(L"\\Device\\Tcpip_") + wcslen(UuidString)) * sizeof(WCHAR) + sizeof(UNICODE_NULL));
    if (!ExportName)
    {
        ERR("HeapAlloc() failed\n");
        rc = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    wcscpy(ExportName, L"\\Device\\Tcpip_");
    wcscat(ExportName, UuidString);

    /* Write Tcpip parameters in new service Key */
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hInterfacesKey, NULL);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegCreateKeyExW(hInterfacesKey, UuidString, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hInterfaceKey, NULL);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hInterfaceKey, L"DefaultGateway", 0, REG_MULTI_SZ, (const BYTE*)L"0.0.0.0\0", (wcslen(L"0.0.0.0") + 2) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hInterfaceKey, L"IPAddress", 0, REG_MULTI_SZ, (const BYTE*)L"0.0.0.0\0", (wcslen(L"0.0.0.0") + 2) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hInterfaceKey, L"SubnetMask", 0, REG_MULTI_SZ, (const BYTE*)L"0.0.0.0\0", (wcslen(L"0.0.0.0") + 2) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    dwValue = 1;
    rc = RegSetValueExW(hInterfaceKey, L"EnableDHCP", 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(DWORD));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    /* Write 'Linkage' key in hardware key */
#if _WIN32_WINNT >= 0x502
    hKey = SetupDiOpenDevRegKey(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ | KEY_WRITE);
#else
    hKey = SetupDiOpenDevRegKey(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_ALL_ACCESS);
#endif
    if (hKey == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND)
        hKey = SetupDiCreateDevRegKeyW(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, NULL, NULL);
    if (hKey == INVALID_HANDLE_VALUE)
    {
        hKey = NULL;
        rc = GetLastError();
        ERR("SetupDiCreateDevRegKeyW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hKey, L"NetCfgInstanceId", 0, REG_SZ, (const BYTE*)UuidString, (wcslen(UuidString) + 1) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hKey, L"Characteristics", 0, REG_DWORD, (const BYTE*)&Characteristics, sizeof(DWORD));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    for (i = 0; i < NET_DDINSTALL_KEYWORD_COUNT; i++)
    {
        if (!Keywords->Present[i])
            continue;

        rc = RegSetValueExW(hKey, NetDDInstallKeywords[i], 0, REG_DWORD,
                            (const BYTE*)&Keywords->Value[i], sizeof(DWORD));
        if (rc != ERROR_SUCCESS)
        {
            ERR("RegSetValueExW(%S) failed with error 0x%lx\n", NetDDInstallKeywords[i], rc);
            goto cleanup;
        }
    }

    /* The interface LUID of the adapter is its *IfType and NetLuidIndex */
    rc = AssignNetLuidIndex(DeviceInfoSet, DeviceInfoData, hKey);
    if (rc != ERROR_SUCCESS)
    {
        ERR("AssignNetLuidIndex() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    /* Get ComponentId (from MatchingDeviceId value) */
    rc = RegQueryValueExW(hKey, L"MatchingDeviceId", 0, &dwRegType, NULL, &dwLength);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }
    if (dwRegType != REG_SZ)
    {
        ERR("Invalid registry type for 'MatchingDeviceId'\n");
        rc = ERROR_GEN_FAILURE;
        goto cleanup;
    }
    ComponentId = HeapAlloc(GetProcessHeap(), 0, dwLength);
    if (!ComponentId)
    {
        ERR("HeapAlloc() failed\n");
        rc = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    rc = RegQueryValueExW(hKey, L"MatchingDeviceId", 0, &dwRegType, (BYTE*)ComponentId, &dwLength);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hKey, L"ComponentId", 0, REG_SZ, (const BYTE*)ComponentId, dwLength);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    if (BusType)
    {
        rc = RegSetValueExW(hKey, L"BusType", 0, REG_SZ, (const BYTE*)BusType, (wcslen(BusType) + 1) * sizeof(WCHAR));
        if (rc != ERROR_SUCCESS)
        {
            ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
            goto cleanup;
        }
    }

    rc = RegCreateKeyExW(hKey, L"Linkage", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hLinkageKey, NULL);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hLinkageKey, L"Export", 0, REG_SZ, (const BYTE*)DeviceName, (wcslen(DeviceName) + 1) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hLinkageKey, L"RootDevice", 0, REG_SZ, (const BYTE*)UuidString, (wcslen(UuidString) + 1) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hLinkageKey, L"UpperBind", 0, REG_SZ, (const BYTE*)L"Tcpip", (wcslen(L"Tcpip") + 1) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }
    RegCloseKey(hKey);
    hKey = NULL;

    /* Write connection information in network subkey */
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
                         0,
                         NULL,
                         REG_OPTION_NON_VOLATILE,
                         KEY_CREATE_SUB_KEY | KEY_QUERY_VALUE,
                         NULL,
                         &hNetworkKey,
                         NULL);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = GetUniqueConnectionName(hNetworkKey, &pszNameBuffer);
    if (rc != ERROR_SUCCESS)
    {
        ERR("GetUniqueConnectionName() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegCreateKeyExW(hNetworkKey, UuidString, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_CREATE_SUB_KEY, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegCreateKeyExW(hKey, L"Connection", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hConnectionKey, NULL);
    RegCloseKey(hKey);
    hKey = NULL;
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hConnectionKey, L"Name", 0, REG_SZ, (const BYTE*)pszNameBuffer, (wcslen(pszNameBuffer) + 1) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    rc = RegSetValueExW(hConnectionKey, L"PnpInstanceID", 0, REG_SZ, (const BYTE*)InstanceId, (wcslen(InstanceId) + 1) * sizeof(WCHAR));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    dwShowIcon = 1;
    rc = RegSetValueExW(hConnectionKey, L"ShowIcon", 0, REG_DWORD, (const BYTE*)&dwShowIcon, sizeof(dwShowIcon));
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegSetValueExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    /* Write linkage information in Tcpip service */
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Linkage", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
    {
        ERR("RegCreateKeyExW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }
    rc = AppendStringToMultiSZ(hKey, L"Bind", DeviceName);
    if (rc != ERROR_SUCCESS)
    {
        ERR("AppendStringToMultiSZ() failed with error 0x%lx\n", rc);
        goto cleanup;
    }
    rc = AppendStringToMultiSZ(hKey, L"Export", ExportName);
    if (rc != ERROR_SUCCESS)
    {
        ERR("AppendStringToMultiSZ() failed with error 0x%lx\n", rc);
        goto cleanup;
    }
    rc = AppendStringToMultiSZ(hKey, L"Route", UuidString);
    if (rc != ERROR_SUCCESS)
    {
        ERR("AppendStringToMultiSZ() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    /*
     * A NIC may be installed after the DHCP service has already enumerated
     * adapters. Notify it through the same API used by the TCP/IP property
     * page so it can activate DHCP for this new interface without requiring a
     * reboot or reinstall.
     */
    rc = DhcpNotifyConfigChange(NULL, (LPWSTR)UuidString, FALSE, 0, 0, 0, 1);
    if (rc != ERROR_SUCCESS)
        WARN("DhcpNotifyConfigChange(%S) failed with error 0x%lx\n", UuidString, rc);

    /* Restart the device so NDIS re-reads Tcpip\Linkage\Bind and binds
     * the protocol to this adapter. We need a full disable/enable cycle
     * to trigger the miniport start path which calls
     * ndisBindMiniportsToProtocol with the updated linkage.
     * Use CM_Disable_DevNode/CM_Enable_DevNode directly for a real
     * PnP stop/start cycle. */
    {
        DEVINST dnDevInst = DeviceInfoData->DevInst;
        ULONG ulStatus, ulProblem;

        /* A device that has not started yet reads the linkage when it starts */
        if (CM_Get_DevNode_Status(&ulStatus, &ulProblem, dnDevInst, 0) == CR_SUCCESS &&
            (ulStatus & DN_STARTED))
        {
            CM_Disable_DevNode(dnDevInst, 0);
        }
        CM_Enable_DevNode(dnDevInst, 0);
    }

    rc = ERROR_SUCCESS;

cleanup:
    HeapFree(GetProcessHeap(), 0, pszNameBuffer);
    HeapFree(GetProcessHeap(), 0, InstanceId);
    HeapFree(GetProcessHeap(), 0, ComponentId);
    HeapFree(GetProcessHeap(), 0, DeviceName);
    HeapFree(GetProcessHeap(), 0, ExportName);
    if (hInterfaceKey != NULL)
        RegCloseKey(hInterfaceKey);
    if (hInterfacesKey != NULL)
        RegCloseKey(hInterfacesKey);
    if (hKey != NULL)
        RegCloseKey(hKey);
    if (hNetworkKey != NULL)
        RegCloseKey(hNetworkKey);
    if (hLinkageKey != NULL)
        RegCloseKey(hLinkageKey);
    if (hConnectionKey != NULL)
        RegCloseKey(hConnectionKey);

    return rc;
}

static
DWORD
InstallNetClient(VOID)
{
    FIXME("Installation of network clients is not yet supported\n");
    return ERROR_GEN_FAILURE;
}

static
DWORD
InstallNetService(VOID)
{
    FIXME("Installation of network services is not yet supported\n");
    return ERROR_GEN_FAILURE;
}

static
DWORD
InstallNetTransport(VOID)
{
    FIXME("Installation of network protocols is not yet supported\n");
    return ERROR_GEN_FAILURE;
}

DWORD
WINAPI
NetClassInstaller(
    IN DI_FUNCTION InstallFunction,
    IN HDEVINFO DeviceInfoSet,
    IN PSP_DEVINFO_DATA DeviceInfoData OPTIONAL)
{
    SP_DRVINFO_DATA_W DriverInfoData;
    SP_DRVINFO_DETAIL_DATA_W DriverInfoDetail;
    WCHAR SectionName[LINE_LEN];
    HINF hInf = INVALID_HANDLE_VALUE;
    INFCONTEXT InfContext;
    UINT ErrorLine;
    INT CharacteristicsInt;
    DWORD Characteristics;
    NET_DDINSTALL_KEYWORDS Keywords;
    LPWSTR BusType = NULL;
    RPC_STATUS RpcStatus;
    UUID Uuid;
    LPWSTR UuidRpcString = NULL;
    LPWSTR UuidString = NULL;
    LONG rc;
    DWORD dwLength;

    if (InstallFunction != DIF_INSTALLDEVICE)
        return ERROR_DI_DO_DEFAULT;

    TRACE("%lu %p %p\n", InstallFunction, DeviceInfoSet, DeviceInfoData);

    /* Get driver info details */
    DriverInfoData.cbSize = sizeof(SP_DRVINFO_DATA_W);
    if (!SetupDiGetSelectedDriverW(DeviceInfoSet, DeviceInfoData, &DriverInfoData))
    {
        rc = GetLastError();
        ERR("SetupDiGetSelectedDriverW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    DriverInfoDetail.cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
    if (!SetupDiGetDriverInfoDetailW(DeviceInfoSet, DeviceInfoData, &DriverInfoData, &DriverInfoDetail, sizeof(DriverInfoDetail), NULL)
     && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        rc = GetLastError();
        ERR("SetupDiGetDriverInfoDetailW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    hInf = SetupOpenInfFileW(DriverInfoDetail.InfFileName, NULL, INF_STYLE_WIN4, &ErrorLine);
    if (hInf == INVALID_HANDLE_VALUE)
    {
        rc = GetLastError();
        ERR("SetupOpenInfFileW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    if (!SetupDiGetActualSectionToInstallW(hInf, DriverInfoDetail.SectionName, SectionName, LINE_LEN, NULL, NULL))
    {
        rc = GetLastError();
        ERR("SetupDiGetActualSectionToInstallW() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    /* Get Characteristics and BusType (optional) from .inf file */
    if (!SetupFindFirstLineW(hInf, SectionName, L"Characteristics", &InfContext))
    {
        rc = GetLastError();
        ERR("Unable to find key %S in section %S of file %S (error 0x%lx)\n",
            L"Characteristics", SectionName, DriverInfoDetail.InfFileName, rc);
        goto cleanup;
    }

    if (!SetupGetIntField(&InfContext, 1, &CharacteristicsInt))
    {
        rc = GetLastError();
        ERR("SetupGetIntField() failed with error 0x%lx\n", rc);
        goto cleanup;
    }

    Characteristics = (DWORD)CharacteristicsInt;
    if (IsEqualIID(&DeviceInfoData->ClassGuid, &GUID_DEVCLASS_NET))
    {
        ReadNetDDInstallKeywords(hInf, SectionName, &Keywords);

        if (SetupFindFirstLineW(hInf, SectionName, L"BusType", &InfContext))
        {
            if (!SetupGetStringFieldW(&InfContext, 1, NULL, 0, &dwLength))
            {
                rc = GetLastError();
                ERR("SetupGetStringFieldW() failed with error 0x%lx\n", rc);
                goto cleanup;
            }

            BusType = HeapAlloc(GetProcessHeap(), 0, dwLength * sizeof(WCHAR));
            if (!BusType)
            {
                ERR("HeapAlloc() failed\n");
                rc = ERROR_NOT_ENOUGH_MEMORY;
                goto cleanup;
            }

            if (!SetupGetStringFieldW(&InfContext, 1, BusType, dwLength, NULL))
            {
                rc = GetLastError();
                ERR("SetupGetStringFieldW() failed with error 0x%lx\n", rc);
                goto cleanup;
            }
        }
    }

    /* An adapter keeps the instance id it has or was suggested */
    if (IsEqualIID(&DeviceInfoData->ClassGuid, &GUID_DEVCLASS_NET))
    {
        UuidString = HeapAlloc(GetProcessHeap(), 0, MAX_GUID_STRING_LEN * sizeof(WCHAR));
        if (!UuidString)
        {
            ERR("HeapAlloc() failed\n");
            rc = ERROR_NOT_ENOUGH_MEMORY;
            goto cleanup;
        }
        if (GetExistingNetCfgInstanceId(DeviceInfoSet, DeviceInfoData, UuidString, MAX_GUID_STRING_LEN))
        {
            TRACE("Using NetCfgInstanceId %S\n", UuidString);
            goto install;
        }
        HeapFree(GetProcessHeap(), 0, UuidString);
        UuidString = NULL;
    }

    /* Create a new UUID */
    RpcStatus = UuidCreate(&Uuid);
    if (RpcStatus != RPC_S_OK && RpcStatus != RPC_S_UUID_LOCAL_ONLY)
    {
        ERR("UuidCreate() failed with RPC status 0x%lx\n", RpcStatus);
        rc = ERROR_GEN_FAILURE;
        goto cleanup;
    }

    RpcStatus = UuidToStringW(&Uuid, &UuidRpcString);
    if (RpcStatus != RPC_S_OK)
    {
        ERR("UuidToStringW() failed with RPC status 0x%lx\n", RpcStatus);
        rc = ERROR_GEN_FAILURE;
        goto cleanup;
    }

    /* Add curly braces around Uuid */
    UuidString = HeapAlloc(GetProcessHeap(), 0, (2 + wcslen(UuidRpcString)) * sizeof(WCHAR) + sizeof(UNICODE_NULL));
    if (!UuidString)
    {
        ERR("HeapAlloc() failed\n");
        rc = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    wcscpy(UuidString, L"{");
    wcscat(UuidString, UuidRpcString);
    wcscat(UuidString, L"}");

install:
    if (IsEqualIID(&DeviceInfoData->ClassGuid, &GUID_DEVCLASS_NET))
        rc = InstallNetDevice(DeviceInfoSet, DeviceInfoData, UuidString, Characteristics, BusType, &Keywords);
    else if (IsEqualIID(&DeviceInfoData->ClassGuid, &GUID_DEVCLASS_NETCLIENT))
        rc = InstallNetClient();
    else if (IsEqualIID(&DeviceInfoData->ClassGuid, &GUID_DEVCLASS_NETSERVICE))
        rc = InstallNetService();
    else if (IsEqualIID(&DeviceInfoData->ClassGuid, &GUID_DEVCLASS_NETTRANS))
        rc = InstallNetTransport();
    else
    {
        ERR("Invalid class guid\n");
        rc = ERROR_GEN_FAILURE;
    }

cleanup:
    if (hInf != INVALID_HANDLE_VALUE)
        SetupCloseInfFile(hInf);
    if (UuidRpcString != NULL)
        RpcStringFreeW(&UuidRpcString);
    HeapFree(GetProcessHeap(), 0, BusType);
    HeapFree(GetProcessHeap(), 0, UuidString);

    TRACE("Returning 0x%lx\n", rc);
    return rc;
}

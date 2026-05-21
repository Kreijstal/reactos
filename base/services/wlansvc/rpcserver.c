/*
 * PROJECT:     ReactOS WLAN Service
 * LICENSE:     GPL-2.0-or-later
 * FILE:        base/services/wlansvc/rpcserver.c
 * PURPOSE:     RPC server interface
 * COPYRIGHT:   Copyright 2009 Christoph von Wittich
 */

#include "precomp.h"
#include <winreg.h>

#define NDEBUG
#include <debug.h>

/*
 * No native NDIS 802.11 miniport has been ported to ReactOS yet, so the
 * driver-dependent surface (enumerate interfaces, scan, connect, query
 * radio state, ...) cannot deliver real data.  The implementations in
 * this file fall into three buckets:
 *
 *   - Enumeration surface (interfaces, available networks, BSS list):
 *     return an empty list with ERROR_SUCCESS, so apps that just want
 *     to "list Wi-Fi adapters" get a successful empty result instead
 *     of failing with ERROR_CALL_NOT_IMPLEMENTED.
 *
 *   - Per-interface operations on an unknown InterfaceGuid (scan,
 *     connect, query interface, ...): return ERROR_NOT_FOUND, which
 *     is the documented error WlanScan etc. emit for a stale GUID.
 *
 *   - Profile / parameter / filter-list / security storage: fully
 *     implemented against the registry, since none of that requires
 *     a driver.  WlanSetProfile, WlanGetProfileList, etc. round-trip
 *     correctly even with no Wi-Fi hardware present.
 *
 * Once a miniport (and the OID_DOT11_* SDK headers it requires) lands,
 * the empty-list / ERROR_NOT_FOUND branches will become NDIS handle
 * lookups and OID requests.
 */

LIST_ENTRY WlanSvcHandleListHead;

static DWORD
WlanSvcDeleteProfileKey(HKEY hProfiles, LPCWSTR ProfileName)
{
#if _WIN32_WINNT >= 0x0600
    return RegDeleteTreeW(hProfiles, ProfileName);
#else
    HKEY hProfile;
    DWORD Status;
    WCHAR SubKeyName[256];

    Status = RegOpenKeyExW(hProfiles,
                           ProfileName,
                           0,
                           KEY_ENUMERATE_SUB_KEYS | DELETE,
                           &hProfile);
    if (Status != ERROR_SUCCESS)
        return Status;

    for (;;)
    {
        DWORD NameLength = ARRAYSIZE(SubKeyName);

        Status = RegEnumKeyExW(hProfile,
                               0,
                               SubKeyName,
                               &NameLength,
                               NULL,
                               NULL,
                               NULL,
                               NULL);
        if (Status == ERROR_NO_MORE_ITEMS)
            break;
        if (Status != ERROR_SUCCESS)
        {
            RegCloseKey(hProfile);
            return Status;
        }

        Status = WlanSvcDeleteProfileKey(hProfile, SubKeyName);
        if (Status != ERROR_SUCCESS)
        {
            RegCloseKey(hProfile);
            return Status;
        }
    }

    RegCloseKey(hProfile);
    return RegDeleteKeyW(hProfiles, ProfileName);
#endif
}

DWORD WINAPI RpcThreadRoutine(LPVOID lpParameter)
{
    RPC_STATUS Status;

    InitializeListHead(&WlanSvcHandleListHead);

    Status = RpcServerUseProtseqEpW(L"ncalrpc", 20, L"wlansvc", NULL);
    if (Status != RPC_S_OK)
    {
        DPRINT("RpcServerUseProtseqEpW() failed (Status %lx)\n", Status);
        return 0;
    }

    Status = RpcServerRegisterIf(wlansvc_interface_v1_0_s_ifspec, NULL, NULL);
    if (Status != RPC_S_OK)
    {
        DPRINT("RpcServerRegisterIf() failed (Status %lx)\n", Status);
        return 0;
    }

    Status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, 0);
    if (Status != RPC_S_OK)
    {
        DPRINT("RpcServerListen() failed (Status %lx)\n", Status);
    }

    DPRINT("RpcServerListen finished\n");
    return 0;
}

PWLANSVCHANDLE WlanSvcGetHandleEntry(LPWLANSVC_RPC_HANDLE ClientHandle)
{
    PLIST_ENTRY CurrentEntry;
    PWLANSVCHANDLE lpWlanSvcHandle;

    CurrentEntry = WlanSvcHandleListHead.Flink;
    while (CurrentEntry != &WlanSvcHandleListHead)
    {
        lpWlanSvcHandle = CONTAINING_RECORD(CurrentEntry,
                                        WLANSVCHANDLE,
                                        WlanSvcHandleListEntry);
        CurrentEntry = CurrentEntry->Flink;

        if (lpWlanSvcHandle == (PWLANSVCHANDLE) ClientHandle)
            return lpWlanSvcHandle;
    }

    return NULL;
}

DWORD _RpcOpenHandle(
    wchar_t *arg_1,
    DWORD dwClientVersion,
    DWORD *pdwNegotiatedVersion,
    LPWLANSVC_RPC_HANDLE phClientHandle)
{
    PWLANSVCHANDLE lpWlanSvcHandle;

    lpWlanSvcHandle = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WLANSVCHANDLE));
    if (lpWlanSvcHandle == NULL)
    {
        DPRINT1("Failed to allocate Heap!\n");
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    if (dwClientVersion > 2)
        dwClientVersion = 2;

    if (dwClientVersion < 1)
        dwClientVersion = 1;

    lpWlanSvcHandle->dwClientVersion = dwClientVersion;
    *pdwNegotiatedVersion = dwClientVersion;

    InsertTailList(&WlanSvcHandleListHead, &lpWlanSvcHandle->WlanSvcHandleListEntry);
    *phClientHandle = lpWlanSvcHandle;

    return ERROR_SUCCESS;
}

DWORD _RpcCloseHandle(
    LPWLANSVC_RPC_HANDLE phClientHandle)
{
    PWLANSVCHANDLE lpWlanSvcHandle;

    lpWlanSvcHandle = WlanSvcGetHandleEntry(*phClientHandle);
    if (!lpWlanSvcHandle)
    {
        return ERROR_INVALID_HANDLE;
    }

    RemoveEntryList(&lpWlanSvcHandle->WlanSvcHandleListEntry);
    HeapFree(GetProcessHeap(), 0, lpWlanSvcHandle);
    *phClientHandle = NULL;

    return ERROR_SUCCESS;
}

DWORD _RpcEnumInterfaces(
    WLANSVC_RPC_HANDLE hClientHandle,
    PWLAN_INTERFACE_INFO_LIST *ppInterfaceList)
{
    PWLAN_INTERFACE_INFO_LIST List;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;

    /* No native 802.11 miniport exists in tree, so the interface list is
     * always empty.  Returning success with dwNumberOfItems=0 matches what
     * Windows does on a machine with no Wi-Fi radio. */
    List = midl_user_allocate(sizeof(WLAN_INTERFACE_INFO_LIST));
    if (List == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    RtlZeroMemory(List, sizeof(WLAN_INTERFACE_INFO_LIST));
    *ppInterfaceList = List;
    return ERROR_SUCCESS;
}

DWORD _RpcSetAutoConfigParameter(
    WLANSVC_RPC_HANDLE hClientHandle,
    long OpCode,
    DWORD dwDataSize,
    LPBYTE pData)
{
    HKEY hParams;
    WCHAR Value[32];
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pData == NULL && dwDataSize != 0)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenParametersKey(KEY_SET_VALUE, &hParams);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"AutoConfig_%lu", (DWORD)OpCode);
    Status = RegSetValueExW(hParams, Value, 0, REG_BINARY, pData, dwDataSize);
    RegCloseKey(hParams);
    return Status;
}

DWORD _RpcQueryAutoConfigParameter(
    WLANSVC_RPC_HANDLE hClientHandle,
    DWORD OpCode,
    LPDWORD pdwDataSize,
    char **ppData,
    DWORD *pWlanOpcodeValueType)
{
    HKEY hParams;
    WCHAR Value[32];
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pdwDataSize == NULL || ppData == NULL || pWlanOpcodeValueType == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenParametersKey(KEY_QUERY_VALUE, &hParams);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"AutoConfig_%lu", OpCode);
    Status = WlanSvcReadAllocBinary(hParams, Value, pdwDataSize, (LPBYTE *)ppData);
    RegCloseKey(hParams);

    if (Status == ERROR_SUCCESS)
        *pWlanOpcodeValueType = 2 /* wlan_opcode_value_type_set_by_user */;
    return Status;
}

DWORD _RpcGetInterfaceCapability(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    PWLAN_INTERFACE_CAPABILITY *ppCapability)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;

    /* GUID can't match any present interface (we have none). */
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(ppCapability);
    return ERROR_NOT_FOUND;
}

DWORD _RpcSetInterface(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    DWORD OpCode,
    DWORD dwDataSize,
    LPBYTE pData)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(OpCode);
    UNREFERENCED_PARAMETER(dwDataSize);
    UNREFERENCED_PARAMETER(pData);
    return ERROR_NOT_FOUND;
}

DWORD _RpcQueryInterface(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    long OpCode,
    LPDWORD pdwDataSize,
    LPBYTE *ppData,
    LPDWORD pWlanOpcodeValueType)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(OpCode);
    UNREFERENCED_PARAMETER(pdwDataSize);
    UNREFERENCED_PARAMETER(ppData);
    UNREFERENCED_PARAMETER(pWlanOpcodeValueType);
    return ERROR_NOT_FOUND;
}

DWORD _RpcIhvControl(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    DWORD Type,
    DWORD dwInBufferSize,
    LPBYTE pInBuffer,
    DWORD dwOutBufferSize,
    LPBYTE pOutBuffer,
    LPDWORD pdwBytesReturned)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(dwInBufferSize);
    UNREFERENCED_PARAMETER(pInBuffer);
    UNREFERENCED_PARAMETER(dwOutBufferSize);
    UNREFERENCED_PARAMETER(pOutBuffer);
    if (pdwBytesReturned != NULL)
        *pdwBytesReturned = 0;
    return ERROR_NOT_FOUND;
}

DWORD _RpcScan(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    PDOT11_SSID pDot11Ssid,
    PWLAN_RAW_DATA pIeData)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(pDot11Ssid);
    UNREFERENCED_PARAMETER(pIeData);
    return ERROR_NOT_FOUND;
}

DWORD _RpcGetAvailableNetworkList(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    DWORD dwFlags,
    WLAN_AVAILABLE_NETWORK_LIST **ppAvailableNetworkList)
{
    WLAN_AVAILABLE_NETWORK_LIST *List;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(dwFlags);

    /* Empty list: GUID can't match a real interface yet. */
    List = midl_user_allocate(sizeof(WLAN_AVAILABLE_NETWORK_LIST));
    if (List == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    RtlZeroMemory(List, sizeof(WLAN_AVAILABLE_NETWORK_LIST));
    *ppAvailableNetworkList = List;
    return ERROR_SUCCESS;
}

DWORD _RpcGetNetworkBssList(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    PDOT11_SSID pDot11Ssid,
    short dot11BssType,
    DWORD bSecurityEnabled,
    LPDWORD dwBssListSize,
    LPBYTE *ppWlanBssList)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(pDot11Ssid);
    UNREFERENCED_PARAMETER(dot11BssType);
    UNREFERENCED_PARAMETER(bSecurityEnabled);

    /* The on-wire shape is a DWORD count followed by an array of
     * WLAN_BSS_ENTRY (currently absent from ROS' windot11.h).  Until
     * the BSS_ENTRY struct ships we return an empty (zero-count)
     * blob, which client wrappers walk safely. */
    *dwBssListSize = sizeof(DWORD);
    *ppWlanBssList = midl_user_allocate(sizeof(DWORD));
    if (*ppWlanBssList == NULL)
    {
        *dwBssListSize = 0;
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    RtlZeroMemory(*ppWlanBssList, sizeof(DWORD));
    return ERROR_SUCCESS;
}

DWORD _RpcConnect(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    const PWLAN_CONNECTION_PARAMETERS *pConnectionParameters)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(pConnectionParameters);
    return ERROR_NOT_FOUND;
}

DWORD _RpcDisconnect(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGUID)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGUID);
    return ERROR_NOT_FOUND;
}

DWORD _RpcRegisterNotification(
    WLANSVC_RPC_HANDLE hClientHandle,
    DWORD dwNotifSource,
    LPDWORD pdwPrevNotifSource)
{
    PWLANSVCHANDLE Handle = WlanSvcGetHandleEntry(hClientHandle);

    if (Handle == NULL)
        return ERROR_INVALID_HANDLE;
    if (pdwPrevNotifSource != NULL)
        *pdwPrevNotifSource = Handle->dwNotifSource;

    /* No driver means there's nothing producing events, so we just
     * record the subscription mask for future query. */
    Handle->dwNotifSource = dwNotifSource;
    return ERROR_SUCCESS;
}

DWORD _RpcAsyncGetNotification(
    WLANSVC_RPC_HANDLE hClientHandle,
    PWLAN_NOTIFICATION_DATA *NotificationData)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    *NotificationData = NULL;
    /* The contract is "blocks until an event arrives or the handle is
     * closed".  Without a driver no event will ever arrive, so we
     * return INVALID_STATE rather than blocking the RPC thread. */
    return ERROR_INVALID_STATE;
}

DWORD _RpcSetProfileEapUserData(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    wchar_t *strProfileName,
    EAP_METHOD_TYPE MethodType,
    DWORD dwFlags,
    DWORD dwEapUserDataSize,
    LPBYTE pbEapUserData)
{
    HKEY hProfile;
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileName == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenProfileKey(pInterfaceGuid, strProfileName, FALSE,
                                   KEY_SET_VALUE, &hProfile);
    if (Status != ERROR_SUCCESS)
        return Status;

    RegSetValueExW(hProfile, L"EapMethodType", 0, REG_BINARY,
                   (LPBYTE)&MethodType, sizeof(MethodType));
    RegSetValueExW(hProfile, L"EapFlags", 0, REG_DWORD,
                   (LPBYTE)&dwFlags, sizeof(dwFlags));
    Status = RegSetValueExW(hProfile, L"EapUserData", 0, REG_BINARY,
                            pbEapUserData, dwEapUserDataSize);
    RegCloseKey(hProfile);
    return Status;
}

DWORD _RpcSetProfile(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    DWORD dwFlags,
    wchar_t *strProfileXml,
    wchar_t *strAllUserProfileSecurity,
    BOOL bOverwrite,
    LPDWORD pdwReasonCode)
{
    HKEY hProfile = NULL, hProfiles = NULL;
    LPWSTR ProfileName = NULL;
    LPWSTR Cursor;
    DWORD Status;
    DWORD GrantedAccess = WLAN_READ_ACCESS | WLAN_EXECUTE_ACCESS | WLAN_WRITE_ACCESS;
    DWORD ExistsDisp;

    if (pdwReasonCode != NULL)
        *pdwReasonCode = 0;
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileXml == NULL)
        return ERROR_INVALID_PARAMETER;

    /* Extract <name>X</name> from the XML so callers don't have to
     * pre-parse just to pick a registry subkey.  This is the same
     * convention Windows uses. */
    Cursor = wcsstr(strProfileXml, L"<name>");
    if (Cursor != NULL)
    {
        LPWSTR End = wcsstr(Cursor + 6, L"</name>");
        if (End != NULL)
        {
            SIZE_T Len = End - (Cursor + 6);
            if (Len > 0 && Len < 256)
            {
                ProfileName = HeapAlloc(GetProcessHeap(), 0, (Len + 1) * sizeof(WCHAR));
                if (ProfileName == NULL)
                    return ERROR_NOT_ENOUGH_MEMORY;
                RtlCopyMemory(ProfileName, Cursor + 6, Len * sizeof(WCHAR));
                ProfileName[Len] = L'\0';
            }
        }
    }
    if (ProfileName == NULL)
    {
        if (pdwReasonCode != NULL)
            *pdwReasonCode = ERROR_BAD_PROFILE;
        return ERROR_BAD_PROFILE;
    }

    Status = WlanSvcOpenProfilesKey(pInterfaceGuid,
                                    KEY_READ | KEY_CREATE_SUB_KEY,
                                    &hProfiles);
    if (Status != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, ProfileName);
        return Status;
    }

    /* Honour bOverwrite. */
    if (!bOverwrite)
    {
        Status = RegOpenKeyExW(hProfiles, ProfileName, 0, KEY_READ, &hProfile);
        if (Status == ERROR_SUCCESS)
        {
            RegCloseKey(hProfile);
            RegCloseKey(hProfiles);
            HeapFree(GetProcessHeap(), 0, ProfileName);
            return ERROR_ALREADY_EXISTS;
        }
    }

    Status = RegCreateKeyExW(hProfiles, ProfileName, 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &hProfile, &ExistsDisp);
    RegCloseKey(hProfiles);
    if (Status != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, ProfileName);
        return Status;
    }

    RegSetValueExW(hProfile, L"Xml", 0, REG_SZ, (LPBYTE)strProfileXml,
                   (DWORD)((wcslen(strProfileXml) + 1) * sizeof(WCHAR)));
    RegSetValueExW(hProfile, L"Flags", 0, REG_DWORD,
                   (LPBYTE)&dwFlags, sizeof(dwFlags));
    RegSetValueExW(hProfile, L"GrantedAccess", 0, REG_DWORD,
                   (LPBYTE)&GrantedAccess, sizeof(GrantedAccess));
    if (strAllUserProfileSecurity != NULL)
    {
        RegSetValueExW(hProfile, L"Sddl", 0, REG_SZ,
                       (LPBYTE)strAllUserProfileSecurity,
                       (DWORD)((wcslen(strAllUserProfileSecurity) + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(hProfile);
    HeapFree(GetProcessHeap(), 0, ProfileName);
    return ERROR_SUCCESS;
}

DWORD _RpcGetProfile(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    wchar_t *strProfileName,
    wchar_t **pstrProfileXml,
    LPDWORD pdwFlags,
    LPDWORD pdwGrantedAccess)
{
    HKEY hProfile;
    DWORD Status;

    if (pstrProfileXml != NULL)
        *pstrProfileXml = NULL;
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileName == NULL || pstrProfileXml == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenProfileKey(pInterfaceGuid, strProfileName, FALSE,
                                   KEY_QUERY_VALUE, &hProfile);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcReadAllocString(hProfile, L"Xml", pstrProfileXml);
    if (Status == ERROR_SUCCESS)
    {
        if (pdwFlags != NULL)
        {
            *pdwFlags = 0;
            WlanSvcReadDword(hProfile, L"Flags", pdwFlags);
        }
        if (pdwGrantedAccess != NULL)
        {
            *pdwGrantedAccess = WLAN_READ_ACCESS;
            WlanSvcReadDword(hProfile, L"GrantedAccess", pdwGrantedAccess);
        }
    }
    RegCloseKey(hProfile);
    return Status;
}

DWORD _RpcDeleteProfile(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    const wchar_t *strProfileName)
{
    HKEY hProfiles;
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileName == NULL ||
        wcschr(strProfileName, L'\\') != NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Status = WlanSvcOpenProfilesKey(pInterfaceGuid, KEY_WRITE, &hProfiles);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcDeleteProfileKey(hProfiles, strProfileName);
    RegCloseKey(hProfiles);
    return Status == ERROR_FILE_NOT_FOUND ? ERROR_NOT_FOUND : Status;
}

DWORD _RpcRenameProfile(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    const wchar_t *strOldProfileName,
    const wchar_t *strNewProfileName)
{
    HKEY hProfiles, hSrc = NULL;
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strOldProfileName == NULL || strNewProfileName == NULL)
        return ERROR_INVALID_PARAMETER;
    if (wcschr(strOldProfileName, L'\\') != NULL ||
        wcschr(strNewProfileName, L'\\') != NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Status = WlanSvcOpenProfilesKey(pInterfaceGuid,
                                    KEY_READ | KEY_WRITE | KEY_CREATE_SUB_KEY,
                                    &hProfiles);
    if (Status != ERROR_SUCCESS)
        return Status;

    /* Reject if destination already present. */
    if (RegOpenKeyExW(hProfiles, strNewProfileName, 0, KEY_READ, &hSrc) == ERROR_SUCCESS)
    {
        RegCloseKey(hSrc);
        RegCloseKey(hProfiles);
        return ERROR_ALREADY_EXISTS;
    }

#if _WIN32_WINNT >= 0x0600
    Status = RegRenameKey(hProfiles, strOldProfileName, strNewProfileName);
    if (Status == ERROR_PROC_NOT_FOUND || Status == ERROR_CALL_NOT_IMPLEMENTED)
#else
    Status = ERROR_CALL_NOT_IMPLEMENTED;
#endif
    {
        /* RegRenameKey isn't available on older runtimes - fall back to
         * read-then-recreate-then-delete. */
        WCHAR ValueName[256];
        DWORD Index, NameLen, Type, DataSize;
        HKEY hDst = NULL;

        Status = RegOpenKeyExW(hProfiles, strOldProfileName, 0, KEY_READ, &hSrc);
        if (Status == ERROR_SUCCESS)
        {
            Status = RegCreateKeyExW(hProfiles, strNewProfileName, 0, NULL, 0,
                                     KEY_SET_VALUE, NULL, &hDst, NULL);
        }
        if (Status == ERROR_SUCCESS)
        {
            for (Index = 0; ; Index++)
            {
                LPBYTE Buf;
                NameLen = ARRAYSIZE(ValueName);
                DataSize = 0;
                if (RegEnumValueW(hSrc, Index, ValueName, &NameLen,
                                  NULL, &Type, NULL, &DataSize) != ERROR_SUCCESS)
                    break;
                Buf = HeapAlloc(GetProcessHeap(), 0, DataSize);
                if (Buf == NULL)
                {
                    Status = ERROR_NOT_ENOUGH_MEMORY;
                    break;
                }
                NameLen = ARRAYSIZE(ValueName);
                if (RegEnumValueW(hSrc, Index, ValueName, &NameLen,
                                  NULL, &Type, Buf, &DataSize) == ERROR_SUCCESS)
                {
                    RegSetValueExW(hDst, ValueName, 0, Type, Buf, DataSize);
                }
                HeapFree(GetProcessHeap(), 0, Buf);
            }
        }
        if (hSrc) RegCloseKey(hSrc);
        if (hDst) RegCloseKey(hDst);
        if (Status == ERROR_SUCCESS)
            Status = WlanSvcDeleteProfileKey(hProfiles, strOldProfileName);
    }

    RegCloseKey(hProfiles);
    return Status == ERROR_FILE_NOT_FOUND ? ERROR_NOT_FOUND : Status;
}

DWORD _RpcSetProfileList(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    DWORD dwItems,
    BYTE **strProfileNames)
{
    HKEY hProfiles;
    DWORD Status, i, TotalLen = 1;
    LPWSTR MultiSz, Cursor;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || (dwItems != 0 && strProfileNames == NULL))
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenProfilesKey(pInterfaceGuid, KEY_SET_VALUE, &hProfiles);
    if (Status != ERROR_SUCCESS)
        return Status;

    /* Serialise dwItems names to a REG_MULTI_SZ so subsequent
     * GetProfileList enumerations can recover the order. */
    for (i = 0; i < dwItems; i++)
        TotalLen += (DWORD)wcslen((LPCWSTR)strProfileNames[i]) + 1;
    MultiSz = HeapAlloc(GetProcessHeap(), 0, TotalLen * sizeof(WCHAR));
    if (MultiSz == NULL)
    {
        RegCloseKey(hProfiles);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    Cursor = MultiSz;
    for (i = 0; i < dwItems; i++)
    {
        SIZE_T Len = wcslen((LPCWSTR)strProfileNames[i]) + 1;
        RtlCopyMemory(Cursor, strProfileNames[i], Len * sizeof(WCHAR));
        Cursor += Len;
    }
    *Cursor = L'\0';

    Status = RegSetValueExW(hProfiles, L"Order", 0, REG_MULTI_SZ,
                            (LPBYTE)MultiSz, TotalLen * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, MultiSz);
    RegCloseKey(hProfiles);
    return Status;
}

DWORD _RpcGetProfileList(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    PWLAN_PROFILE_INFO_LIST *ppProfileList)
{
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || ppProfileList == NULL)
        return ERROR_INVALID_PARAMETER;

    return WlanSvcEnumProfileNames(pInterfaceGuid, ppProfileList);
}

DWORD _RpcSetProfilePosition(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    wchar_t *strProfileName,
    DWORD dwPosition)
{
    HKEY hProfile;
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileName == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenProfileKey(pInterfaceGuid, strProfileName, FALSE,
                                   KEY_SET_VALUE, &hProfile);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = RegSetValueExW(hProfile, L"Position", 0, REG_DWORD,
                            (LPBYTE)&dwPosition, sizeof(dwPosition));
    RegCloseKey(hProfile);
    return Status;
}

DWORD _RpcSetProfileCustomUserData(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    wchar_t *strProfileName,
    DWORD dwDataSize,
    LPBYTE pData)
{
    HKEY hProfile;
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileName == NULL)
        return ERROR_INVALID_PARAMETER;
    if (dwDataSize != 0 && pData == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenProfileKey(pInterfaceGuid, strProfileName, FALSE,
                                   KEY_SET_VALUE, &hProfile);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = RegSetValueExW(hProfile, L"CustomUserData", 0, REG_BINARY,
                            pData, dwDataSize);
    RegCloseKey(hProfile);
    return Status;
}

DWORD _RpcGetProfileCustomUserData(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    wchar_t *strProfileName,
    LPDWORD dwDataSize,
    LPBYTE *pData)
{
    HKEY hProfile;
    DWORD Status;

    if (pData != NULL)
        *pData = NULL;
    if (dwDataSize != NULL)
        *dwDataSize = 0;
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pInterfaceGuid == NULL || strProfileName == NULL || dwDataSize == NULL || pData == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenProfileKey(pInterfaceGuid, strProfileName, FALSE,
                                   KEY_QUERY_VALUE, &hProfile);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcReadAllocBinary(hProfile, L"CustomUserData", dwDataSize, pData);
    RegCloseKey(hProfile);
    return Status;
}

DWORD _RpcSetFilterList(
    WLANSVC_RPC_HANDLE hClientHandle,
    short wlanFilterListType,
    PDOT11_NETWORK_LIST pNetworkList)
{
    HKEY hFilters;
    WCHAR Value[32];
    DWORD Status, Count, Size;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;

    Status = WlanSvcOpenFiltersKey(KEY_SET_VALUE, &hFilters);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"Filter_%hd", wlanFilterListType);

    if (pNetworkList == NULL)
    {
        Status = RegDeleteValueW(hFilters, Value);
    }
    else
    {
        Count = pNetworkList->dwNumberOfItems;
        if (Count > 1024)
            Count = 1024;
        Size = FIELD_OFFSET(DOT11_NETWORK_LIST, Network[Count]);
        Status = RegSetValueExW(hFilters, Value, 0, REG_BINARY,
                                (LPBYTE)pNetworkList, Size);
    }
    RegCloseKey(hFilters);
    return Status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : Status;
}

DWORD _RpcGetFilterList(
    WLANSVC_RPC_HANDLE hClientHandle,
    short wlanFilterListType,
    PDOT11_NETWORK_LIST *pNetworkList)
{
    HKEY hFilters;
    WCHAR Value[32];
    DWORD Status, Size = 0;
    LPBYTE Buf = NULL;

    if (pNetworkList != NULL)
        *pNetworkList = NULL;
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (pNetworkList == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenFiltersKey(KEY_QUERY_VALUE, &hFilters);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"Filter_%hd", wlanFilterListType);
    Status = WlanSvcReadAllocBinary(hFilters, Value, &Size, &Buf);
    RegCloseKey(hFilters);

    if (Status == ERROR_NOT_FOUND)
    {
        /* No filter set: synthesise an empty list. */
        PDOT11_NETWORK_LIST Empty = midl_user_allocate(sizeof(DOT11_NETWORK_LIST));
        if (Empty == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;
        RtlZeroMemory(Empty, sizeof(DOT11_NETWORK_LIST));
        *pNetworkList = Empty;
        return ERROR_SUCCESS;
    }

    if (Status == ERROR_SUCCESS)
        *pNetworkList = (PDOT11_NETWORK_LIST)Buf;
    return Status;
}

DWORD _RpcSetPsdIEDataList(
    WLANSVC_RPC_HANDLE hClientHandle,
    wchar_t *strFormat,
    DWORD dwDataListSize,
    LPBYTE pPsdIEDataList)
{
    HKEY hParams;
    WCHAR Value[128];
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (strFormat == NULL || wcschr(strFormat, L'\\') != NULL)
        return ERROR_INVALID_PARAMETER;
    if (dwDataListSize != 0 && pPsdIEDataList == NULL)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenParametersKey(KEY_SET_VALUE, &hParams);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"PsdIE_%.100ls", strFormat);
    Status = RegSetValueExW(hParams, Value, 0, REG_BINARY,
                            pPsdIEDataList, dwDataListSize);
    RegCloseKey(hParams);
    return Status;
}

DWORD _RpcSaveTemporaryProfile(
    WLANSVC_RPC_HANDLE hClientHandle,
    const GUID *pInterfaceGuid,
    wchar_t *strProfileName,
    wchar_t *strAllUserProfileSecurity,
    DWORD dwFlags,
    BOOL bOverWrite)
{
    /* Temporary profile semantics: persist the named in-memory profile
     * to the regular profile store.  Without a driver we never have a
     * cached temporary profile, so report NOT_FOUND. */
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(strProfileName);
    UNREFERENCED_PARAMETER(strAllUserProfileSecurity);
    UNREFERENCED_PARAMETER(dwFlags);
    UNREFERENCED_PARAMETER(bOverWrite);
    return ERROR_NOT_FOUND;
}

DWORD _RpcIsUIRequestPending(
    wchar_t *arg_1,
    const GUID *pInterfaceGuid,
    struct_C *arg_3,
    LPDWORD arg_4)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(arg_3);
    if (arg_4 != NULL)
        *arg_4 = 0;
    return ERROR_SUCCESS;
}

DWORD _RpcSetUIForwardingNetworkList(
    wchar_t *arg_1,
    GUID *arg_2,
    DWORD dwSize,
    GUID *arg_4)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(arg_2);
    UNREFERENCED_PARAMETER(dwSize);
    UNREFERENCED_PARAMETER(arg_4);
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcIsNetworkSuppressed(
    wchar_t *arg_1,
    DWORD arg_2,
    const GUID *pInterfaceGuid,
    LPDWORD arg_4)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(arg_2);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    if (arg_4 != NULL)
        *arg_4 = 0;
    return ERROR_SUCCESS;
}

DWORD _RpcRemoveUIForwardingNetworkList(
    wchar_t *arg_1,
    const GUID *pInterfaceGuid)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcQueryExtUIRequest(
    wchar_t *arg_1,
    GUID *arg_2,
    GUID *arg_3,
    short arg_4,
    GUID *pInterfaceGuid,
    struct_C **arg_6)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(arg_2);
    UNREFERENCED_PARAMETER(arg_3);
    UNREFERENCED_PARAMETER(arg_4);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    if (arg_6 != NULL)
        *arg_6 = NULL;
    return ERROR_NOT_FOUND;
}

DWORD _RpcUIResponse(
    wchar_t *arg_1,
    struct_C *arg_2,
    struct_D *arg_3)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(arg_2);
    UNREFERENCED_PARAMETER(arg_3);
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcGetProfileKeyInfo(
    wchar_t *arg_1,
    DWORD arg_2,
    const GUID *pInterfaceGuid,
    wchar_t *arg_4,
    DWORD arg_5,
    LPDWORD arg_6,
    char *arg_7,
    LPDWORD arg_8)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(arg_2);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(arg_4);
    UNREFERENCED_PARAMETER(arg_5);
    UNREFERENCED_PARAMETER(arg_7);
    if (arg_6 != NULL)
        *arg_6 = 0;
    if (arg_8 != NULL)
        *arg_8 = 0;
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcAsyncDoPlap(
    wchar_t *arg_1,
    const GUID *pInterfaceGuid,
    wchar_t *arg_3,
    DWORD dwSize,
    struct_E arg_5[])
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    UNREFERENCED_PARAMETER(arg_3);
    UNREFERENCED_PARAMETER(dwSize);
    UNREFERENCED_PARAMETER(arg_5);
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcQueryPlapCredentials(
    wchar_t *arg_1,
    LPDWORD dwSize,
    struct_E **arg_3,
    wchar_t **arg_4,
    GUID *pInterfaceGuid,
    LPDWORD arg_6,
    LPDWORD arg_7,
    LPDWORD arg_8,
    LPDWORD arg_9)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    if (dwSize) *dwSize = 0;
    if (arg_3) *arg_3 = NULL;
    if (arg_4) *arg_4 = NULL;
    if (arg_6) *arg_6 = 0;
    if (arg_7) *arg_7 = 0;
    if (arg_8) *arg_8 = 0;
    if (arg_9) *arg_9 = 0;
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcCancelPlap(
    wchar_t *arg_1,
    const GUID *pInterfaceGuid)
{
    UNREFERENCED_PARAMETER(arg_1);
    UNREFERENCED_PARAMETER(pInterfaceGuid);
    return ERROR_NOT_SUPPORTED;
}

DWORD _RpcSetSecuritySettings(
    WLANSVC_RPC_HANDLE hClientHandle,
    WLAN_SECURABLE_OBJECT SecurableObject,
    const wchar_t *strModifiedSDDL)
{
    HKEY hSec;
    WCHAR Value[32];
    DWORD Status;

    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if (strModifiedSDDL == NULL || (DWORD)SecurableObject >= WLAN_SECURABLE_OBJECT_COUNT)
        return ERROR_INVALID_PARAMETER;

    Status = WlanSvcOpenSecurityKey(KEY_SET_VALUE, &hSec);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"Object_%lu", (DWORD)SecurableObject);
    Status = RegSetValueExW(hSec, Value, 0, REG_SZ, (LPBYTE)strModifiedSDDL,
                            (DWORD)((wcslen(strModifiedSDDL) + 1) * sizeof(WCHAR)));
    RegCloseKey(hSec);
    return Status;
}

DWORD _RpcGetSecuritySettings(
    WLANSVC_RPC_HANDLE hClientHandle,
    WLAN_SECURABLE_OBJECT SecurableObject,
    WLAN_OPCODE_VALUE_TYPE *pValueType,
    wchar_t **pstrCurrentSDDL,
    LPDWORD pdwGrantedAccess)
{
    HKEY hSec;
    WCHAR Value[32];
    DWORD Status;

    if (pstrCurrentSDDL != NULL)
        *pstrCurrentSDDL = NULL;
    if (!WlanSvcGetHandleEntry(hClientHandle))
        return ERROR_INVALID_HANDLE;
    if ((DWORD)SecurableObject >= WLAN_SECURABLE_OBJECT_COUNT ||
        pstrCurrentSDDL == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Status = WlanSvcOpenSecurityKey(KEY_QUERY_VALUE, &hSec);
    if (Status != ERROR_SUCCESS)
        return Status;

    swprintf(Value, ARRAYSIZE(Value), L"Object_%lu", (DWORD)SecurableObject);
    Status = WlanSvcReadAllocString(hSec, Value, pstrCurrentSDDL);
    RegCloseKey(hSec);

    if (Status == ERROR_SUCCESS)
    {
        if (pValueType != NULL)
            *pValueType = wlan_opcode_value_type_set_by_user;
        if (pdwGrantedAccess != NULL)
            *pdwGrantedAccess = WLAN_READ_ACCESS | WLAN_WRITE_ACCESS;
    }
    return Status;
}

void __RPC_FAR * __RPC_USER midl_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}


void __RPC_USER midl_user_free(void __RPC_FAR * ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}


void __RPC_USER WLANSVC_RPC_HANDLE_rundown(WLANSVC_RPC_HANDLE hClientHandle)
{
    /* Client connection torn down without a matching WlanCloseHandle:
     * drop the handle ourselves so the list does not grow unbounded. */
    PWLANSVCHANDLE Handle = WlanSvcGetHandleEntry(hClientHandle);
    if (Handle != NULL)
    {
        RemoveEntryList(&Handle->WlanSvcHandleListEntry);
        HeapFree(GetProcessHeap(), 0, Handle);
    }
}

/*
 * PROJECT:     ReactOS WLAN Service
 * LICENSE:     GPL-2.0-or-later
 * FILE:        base/services/wlansvc/regkey.c
 * PURPOSE:     Registry-backed persistence for WLAN profiles, parameters,
 *              filter lists, and security descriptors.
 */

#include "precomp.h"
#include <winreg.h>

#define NDEBUG
#include <debug.h>

#define WLANSVC_BASE_KEY        L"SOFTWARE\\Microsoft\\WlanSvc"
#define WLANSVC_INTERFACES_SUB  L"Interfaces"
#define WLANSVC_PARAMETERS_SUB  L"Parameters"
#define WLANSVC_FILTERS_SUB     L"FilterLists"
#define WLANSVC_SECURITY_SUB    L"Security"
#define WLANSVC_PROFILES_SUB    L"Profiles"
#define WLANSVC_PROFILE_ORDER   L"Order"
#define WLANSVC_PROFILE_XML     L"Xml"
#define WLANSVC_PROFILE_FLAGS   L"Flags"
#define WLANSVC_PROFILE_GRANTED L"GrantedAccess"
#define WLANSVC_PROFILE_CUSTOM  L"CustomUserData"
#define WLANSVC_PROFILE_EAP     L"EapUserData"
#define WLANSVC_AUTOCONFIG_VAL  L"Value"

VOID
WlanSvcGuidToString(
    const GUID *Guid,
    LPWSTR Buffer)
{
    swprintf(Buffer, 40,
             L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             Guid->Data1, Guid->Data2, Guid->Data3,
             Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
             Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

static
DWORD
WlanSvcOpenOrCreate(
    HKEY Parent,
    LPCWSTR SubKey,
    REGSAM SamDesired,
    PHKEY pKey)
{
    DWORD Disposition;
    return RegCreateKeyExW(Parent, SubKey, 0, NULL, 0, SamDesired, NULL,
                           pKey, &Disposition);
}

DWORD
WlanSvcOpenServiceKey(
    REGSAM SamDesired,
    PHKEY pKey)
{
    return WlanSvcOpenOrCreate(HKEY_LOCAL_MACHINE, WLANSVC_BASE_KEY,
                               SamDesired, pKey);
}

DWORD
WlanSvcOpenInterfaceKey(
    const GUID *InterfaceGuid,
    REGSAM SamDesired,
    PHKEY pKey)
{
    HKEY hSvc = NULL, hIfaces = NULL;
    WCHAR GuidStr[40];
    DWORD Status;

    Status = WlanSvcOpenServiceKey(KEY_READ | KEY_CREATE_SUB_KEY, &hSvc);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcOpenOrCreate(hSvc, WLANSVC_INTERFACES_SUB,
                                 KEY_READ | KEY_CREATE_SUB_KEY, &hIfaces);
    RegCloseKey(hSvc);
    if (Status != ERROR_SUCCESS)
        return Status;

    WlanSvcGuidToString(InterfaceGuid, GuidStr);
    Status = WlanSvcOpenOrCreate(hIfaces, GuidStr, SamDesired, pKey);
    RegCloseKey(hIfaces);
    return Status;
}

DWORD
WlanSvcOpenProfilesKey(
    const GUID *InterfaceGuid,
    REGSAM SamDesired,
    PHKEY pKey)
{
    HKEY hIface = NULL;
    DWORD Status;

    Status = WlanSvcOpenInterfaceKey(InterfaceGuid,
                                     KEY_READ | KEY_CREATE_SUB_KEY,
                                     &hIface);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcOpenOrCreate(hIface, WLANSVC_PROFILES_SUB, SamDesired, pKey);
    RegCloseKey(hIface);
    return Status;
}

DWORD
WlanSvcOpenProfileKey(
    const GUID *InterfaceGuid,
    LPCWSTR ProfileName,
    BOOL Create,
    REGSAM SamDesired,
    PHKEY pKey)
{
    HKEY hProfiles = NULL;
    DWORD Status;

    if (ProfileName == NULL || *ProfileName == L'\0' ||
        wcschr(ProfileName, L'\\') != NULL || wcschr(ProfileName, L'/') != NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Status = WlanSvcOpenProfilesKey(InterfaceGuid,
                                    KEY_READ | KEY_CREATE_SUB_KEY,
                                    &hProfiles);
    if (Status != ERROR_SUCCESS)
        return Status;

    if (Create)
        Status = WlanSvcOpenOrCreate(hProfiles, ProfileName, SamDesired, pKey);
    else
        Status = RegOpenKeyExW(hProfiles, ProfileName, 0, SamDesired, pKey);

    RegCloseKey(hProfiles);
    return Status == ERROR_FILE_NOT_FOUND ? ERROR_NOT_FOUND : Status;
}

DWORD
WlanSvcOpenParametersKey(
    REGSAM SamDesired,
    PHKEY pKey)
{
    HKEY hSvc = NULL;
    DWORD Status;

    Status = WlanSvcOpenServiceKey(KEY_READ | KEY_CREATE_SUB_KEY, &hSvc);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcOpenOrCreate(hSvc, WLANSVC_PARAMETERS_SUB, SamDesired, pKey);
    RegCloseKey(hSvc);
    return Status;
}

DWORD
WlanSvcOpenFiltersKey(
    REGSAM SamDesired,
    PHKEY pKey)
{
    HKEY hSvc = NULL;
    DWORD Status;

    Status = WlanSvcOpenServiceKey(KEY_READ | KEY_CREATE_SUB_KEY, &hSvc);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcOpenOrCreate(hSvc, WLANSVC_FILTERS_SUB, SamDesired, pKey);
    RegCloseKey(hSvc);
    return Status;
}

DWORD
WlanSvcOpenSecurityKey(
    REGSAM SamDesired,
    PHKEY pKey)
{
    HKEY hSvc = NULL;
    DWORD Status;

    Status = WlanSvcOpenServiceKey(KEY_READ | KEY_CREATE_SUB_KEY, &hSvc);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = WlanSvcOpenOrCreate(hSvc, WLANSVC_SECURITY_SUB, SamDesired, pKey);
    RegCloseKey(hSvc);
    return Status;
}

DWORD
WlanSvcReadAllocBinary(
    HKEY Key,
    LPCWSTR ValueName,
    LPDWORD pDataSize,
    LPBYTE *ppData)
{
    DWORD Status, Type, Size = 0;
    LPBYTE Buffer;

    *pDataSize = 0;
    *ppData = NULL;

    Status = RegQueryValueExW(Key, ValueName, NULL, &Type, NULL, &Size);
    if (Status != ERROR_SUCCESS)
        return Status == ERROR_FILE_NOT_FOUND ? ERROR_NOT_FOUND : Status;

    if (Size == 0)
        return ERROR_SUCCESS;

    Buffer = midl_user_allocate(Size);
    if (Buffer == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    Status = RegQueryValueExW(Key, ValueName, NULL, NULL, Buffer, &Size);
    if (Status != ERROR_SUCCESS)
    {
        midl_user_free(Buffer);
        return Status;
    }

    *pDataSize = Size;
    *ppData = Buffer;
    return ERROR_SUCCESS;
}

DWORD
WlanSvcReadAllocString(
    HKEY Key,
    LPCWSTR ValueName,
    LPWSTR *ppString)
{
    DWORD Status, Size;

    Status = WlanSvcReadAllocBinary(Key, ValueName, &Size, (LPBYTE *)ppString);
    if (Status != ERROR_SUCCESS)
        return Status;

    /* RegQueryValueExW does not guarantee NUL-termination on REG_SZ values
     * that were stored without one (rare, but cheap to defend against). */
    if (*ppString != NULL && Size >= sizeof(WCHAR) &&
        (*ppString)[(Size / sizeof(WCHAR)) - 1] != L'\0')
    {
        LPWSTR Bigger = midl_user_allocate(Size + sizeof(WCHAR));
        if (Bigger == NULL)
        {
            midl_user_free(*ppString);
            *ppString = NULL;
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        RtlCopyMemory(Bigger, *ppString, Size);
        Bigger[Size / sizeof(WCHAR)] = L'\0';
        midl_user_free(*ppString);
        *ppString = Bigger;
    }

    return ERROR_SUCCESS;
}

DWORD
WlanSvcReadDword(
    HKEY Key,
    LPCWSTR ValueName,
    LPDWORD pValue)
{
    DWORD Type, Size = sizeof(DWORD), Status;

    Status = RegQueryValueExW(Key, ValueName, NULL, &Type,
                              (LPBYTE)pValue, &Size);
    if (Status == ERROR_FILE_NOT_FOUND)
        return ERROR_NOT_FOUND;
    if (Status != ERROR_SUCCESS)
        return Status;
    if (Type != REG_DWORD || Size != sizeof(DWORD))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

DWORD
WlanSvcEnumProfileNames(
    const GUID *InterfaceGuid,
    PWLAN_PROFILE_INFO_LIST *ppList)
{
    HKEY hProfiles = NULL;
    DWORD Status, Index, NameLen, Count = 0;
    WCHAR Name[256];
    PWLAN_PROFILE_INFO_LIST List;
    DWORD AllocSize;

    *ppList = NULL;

    Status = WlanSvcOpenProfilesKey(InterfaceGuid, KEY_READ, &hProfiles);
    if (Status != ERROR_SUCCESS && Status != ERROR_FILE_NOT_FOUND)
        return Status;

    if (Status == ERROR_SUCCESS)
    {
        for (Index = 0; ; Index++)
        {
            NameLen = ARRAYSIZE(Name);
            if (RegEnumKeyExW(hProfiles, Index, Name, &NameLen,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            Count++;
        }
    }

    AllocSize = FIELD_OFFSET(WLAN_PROFILE_INFO_LIST, ProfileInfo[Count]);
    if (Count == 0)
        AllocSize = sizeof(WLAN_PROFILE_INFO_LIST);

    List = midl_user_allocate(AllocSize);
    if (List == NULL)
    {
        if (hProfiles)
            RegCloseKey(hProfiles);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    RtlZeroMemory(List, AllocSize);

    if (hProfiles != NULL)
    {
        for (Index = 0; Index < Count; Index++)
        {
            HKEY hProfile;
            DWORD Flags = 0;
            NameLen = ARRAYSIZE(Name);
            if (RegEnumKeyExW(hProfiles, Index, Name, &NameLen,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            wcsncpy(List->ProfileInfo[List->dwNumberOfItems].strProfileName,
                    Name,
                    ARRAYSIZE(List->ProfileInfo[List->dwNumberOfItems].strProfileName) - 1);

            if (RegOpenKeyExW(hProfiles, Name, 0, KEY_READ, &hProfile) == ERROR_SUCCESS)
            {
                WlanSvcReadDword(hProfile, WLANSVC_PROFILE_FLAGS, &Flags);
                RegCloseKey(hProfile);
            }
            List->ProfileInfo[List->dwNumberOfItems].dwFlags = Flags;
            List->dwNumberOfItems++;
        }
        RegCloseKey(hProfiles);
    }

    *ppList = List;
    return ERROR_SUCCESS;
}

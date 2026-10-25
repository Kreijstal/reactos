/*
 * PROJECT:     ReactOS Network Connection Interface
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Network connection names
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * The name of a network connection, the interface alias that netsh and the
 * network folder show, is the Name value of
 * Control\Network\{4D36E972-E325-11CE-BFC1-08002BE10318}\{guid}\Connection,
 * which the network class installer creates when it installs an adapter.
 */

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winerror.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>

#define NETWORK_KEY L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}"

static
DWORD
NcipOpenConnectionKey(
    _In_ const GUID *Guid,
    _In_ REGSAM Access,
    _Out_ PHKEY ConnectionKey)
{
    WCHAR KeyName[ARRAYSIZE(NETWORK_KEY) + 39 + ARRAYSIZE(L"\\\\Connection")];
    WCHAR GuidString[39];

    if (!Guid)
        return ERROR_INVALID_PARAMETER;
    if (!StringFromGUID2(Guid, GuidString, ARRAYSIZE(GuidString)))
        return ERROR_INVALID_PARAMETER;

    _snwprintf(KeyName, ARRAYSIZE(KeyName), L"%s\\%s\\Connection", NETWORK_KEY, GuidString);
    KeyName[ARRAYSIZE(KeyName) - 1] = UNICODE_NULL;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, Access, ConnectionKey);
}

/* Whether a connection other than Guid already carries Name */
static
BOOL
NcipNameIsTaken(
    _In_ const GUID *Guid,
    _In_ LPCWSTR Name)
{
    WCHAR SubKeyName[64], OtherName[256];
    WCHAR GuidString[39];
    HKEY NetworkKey, ConnectionKey;
    DWORD Index, Size, Type;
    BOOL Taken = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NETWORK_KEY, 0, KEY_ENUMERATE_SUB_KEYS, &NetworkKey) != ERROR_SUCCESS)
        return FALSE;
    StringFromGUID2(Guid, GuidString, ARRAYSIZE(GuidString));

    for (Index = 0; !Taken; Index++)
    {
        Size = ARRAYSIZE(SubKeyName);
        if (RegEnumKeyExW(NetworkKey, Index, SubKeyName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (!_wcsicmp(SubKeyName, GuidString))
            continue;

        wcscat(SubKeyName, L"\\Connection");
        if (RegOpenKeyExW(NetworkKey, SubKeyName, 0, KEY_QUERY_VALUE, &ConnectionKey) != ERROR_SUCCESS)
            continue;
        Size = sizeof(OtherName) - sizeof(WCHAR);
        if (RegQueryValueExW(ConnectionKey, L"Name", NULL, &Type, (LPBYTE)OtherName, &Size) == ERROR_SUCCESS &&
            Type == REG_SZ)
        {
            OtherName[Size / sizeof(WCHAR)] = UNICODE_NULL;
            Taken = !_wcsicmp(OtherName, Name);
        }
        RegCloseKey(ConnectionKey);
    }

    RegCloseKey(NetworkKey);
    return Taken;
}

/*
 * Returns the connection name of the adapter Guid. A NULL Name or a buffer
 * of InDestNameBytes = 0 queries the size; *OutDestNameBytes is the size of
 * the name with its terminator. ERROR_MORE_DATA if the buffer is too small,
 * ERROR_FILE_NOT_FOUND for an unknown adapter.
 */
DWORD
WINAPI
NciGetConnectionName(
    _In_ const GUID *Guid,
    _Out_writes_bytes_opt_(InDestNameBytes) LPWSTR Name,
    _In_ DWORD InDestNameBytes,
    _Out_opt_ DWORD *OutDestNameBytes)
{
    HKEY ConnectionKey;
    DWORD Error, Size = 0, Type;

    Error = NcipOpenConnectionKey(Guid, KEY_QUERY_VALUE, &ConnectionKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegQueryValueExW(ConnectionKey, L"Name", NULL, &Type, NULL, &Size);
    if (Error == ERROR_SUCCESS && Type != REG_SZ)
        Error = ERROR_FILE_NOT_FOUND;

    if (Error == ERROR_SUCCESS && Name && InDestNameBytes)
    {
        if (InDestNameBytes < Size)
        {
            Error = ERROR_MORE_DATA;
        }
        else
        {
            Error = RegQueryValueExW(ConnectionKey, L"Name", NULL, &Type, (LPBYTE)Name, &Size);
            /* Make sure the name is terminated */
            if (Error == ERROR_SUCCESS && Size >= sizeof(WCHAR) && Name[Size / sizeof(WCHAR) - 1] != UNICODE_NULL)
            {
                if (Size + sizeof(WCHAR) > InDestNameBytes)
                    Error = ERROR_MORE_DATA;
                else
                    Name[Size / sizeof(WCHAR)] = UNICODE_NULL;
                Size += sizeof(WCHAR);
            }
        }
    }

    RegCloseKey(ConnectionKey);

    if ((Error == ERROR_SUCCESS || Error == ERROR_MORE_DATA) && OutDestNameBytes)
        *OutDestNameBytes = Size;
    return Error;
}

/*
 * Renames the connection of the adapter Guid. ERROR_DUP_NAME if another
 * connection already has that name, ERROR_FILE_NOT_FOUND for an unknown
 * adapter.
 */
DWORD
WINAPI
NciSetConnectionName(
    _In_ const GUID *Guid,
    _In_z_ LPCWSTR NewName)
{
    HKEY ConnectionKey;
    DWORD Error;
    SIZE_T Length;

    if (!NewName || !*NewName)
        return ERROR_INVALID_PARAMETER;
    Length = wcslen(NewName);
    if (Length >= 256)
        return ERROR_INVALID_PARAMETER;

    Error = NcipOpenConnectionKey(Guid, KEY_QUERY_VALUE, &ConnectionKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Keeping the current name needs no write access */
    {
        WCHAR CurrentName[256];
        DWORD Size = sizeof(CurrentName) - sizeof(WCHAR), Type;

        if (RegQueryValueExW(ConnectionKey, L"Name", NULL, &Type, (LPBYTE)CurrentName, &Size) == ERROR_SUCCESS &&
            Type == REG_SZ)
        {
            CurrentName[Size / sizeof(WCHAR)] = UNICODE_NULL;
            if (!wcscmp(CurrentName, NewName))
            {
                RegCloseKey(ConnectionKey);
                return ERROR_SUCCESS;
            }
        }
    }
    RegCloseKey(ConnectionKey);

    Error = NcipOpenConnectionKey(Guid, KEY_SET_VALUE, &ConnectionKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (NcipNameIsTaken(Guid, NewName))
        Error = ERROR_DUP_NAME;
    else
        Error = RegSetValueExW(ConnectionKey, L"Name", 0, REG_SZ, (const BYTE *)NewName,
                               (DWORD)((Length + 1) * sizeof(WCHAR)));

    RegCloseKey(ConnectionKey);
    return Error;
}

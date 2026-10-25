/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the network connection names of nci.dll and
 *              ConvertInterfaceAliasToLuid
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <stdio.h>

typedef DWORD (WINAPI *FN_NciGetConnectionName)(const GUID *, LPWSTR, DWORD, DWORD *);
typedef DWORD (WINAPI *FN_NciSetConnectionName)(const GUID *, LPCWSTR);
typedef DWORD (WINAPI *FN_ConvertInterfaceAliasToLuid)(const WCHAR *, NET_LUID *);

static FN_NciGetConnectionName pNciGetConnectionName;
static FN_NciSetConnectionName pNciSetConnectionName;
static FN_ConvertInterfaceAliasToLuid pConvertInterfaceAliasToLuid;

/* A GUID no adapter has */
static const GUID BogusGuid = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };

/* Reads the connection name of a real adapter; returns FALSE if there is none */
static BOOL
TestRealAdapter(GUID *Guid, WCHAR *Name, DWORD NameChars)
{
    IP_ADAPTER_ADDRESSES *Addresses, *Adapter;
    ULONG Size = 0;
    BOOL Found = FALSE;
    WCHAR GuidString[64];
    DWORD Error, Out;
    NET_LUID Luid;

    GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &Size);
    Addresses = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Addresses || GetAdaptersAddresses(AF_UNSPEC, 0, NULL, Addresses, &Size) != NO_ERROR)
    {
        skip("GetAdaptersAddresses failed\n");
        HeapFree(GetProcessHeap(), 0, Addresses);
        return FALSE;
    }

    for (Adapter = Addresses; Adapter && !Found; Adapter = Adapter->Next)
    {
        MultiByteToWideChar(CP_ACP, 0, Adapter->AdapterName, -1, GuidString, ARRAYSIZE(GuidString));
        if (CLSIDFromString(GuidString, Guid) != NOERROR)
            continue;

        /* NULL buffer: size query */
        Out = 0xdeadbeef;
        Error = pNciGetConnectionName(Guid, NULL, 0, &Out);
        if (Error == ERROR_FILE_NOT_FOUND)
            continue; /* no network connection, such as the loopback interface */
        Found = TRUE;
        ok(Error == ERROR_SUCCESS, "Size query: error %lu\n", Error);
        ok(Out >= 2 * sizeof(WCHAR) && Out <= NameChars * sizeof(WCHAR), "Size query: %lu bytes\n", Out);

        /* Too small a buffer */
        Out = 0xdeadbeef;
        Error = pNciGetConnectionName(Guid, Name, sizeof(WCHAR), &Out);
        ok(Error == ERROR_MORE_DATA, "Small buffer: error %lu\n", Error);
        ok(Out >= 2 * sizeof(WCHAR), "Small buffer: %lu bytes\n", Out);

        /* The name, terminated, and its size with the terminator */
        Out = 0xdeadbeef;
        ZeroMemory(Name, NameChars * sizeof(WCHAR));
        Error = pNciGetConnectionName(Guid, Name, NameChars * sizeof(WCHAR), &Out);
        ok(Error == ERROR_SUCCESS, "Name: error %lu\n", Error);
        ok(Out == (wcslen(Name) + 1) * sizeof(WCHAR), "Name \"%ls\": %lu bytes\n", Name, Out);

        /* The name is the interface alias */
        if (pConvertInterfaceAliasToLuid)
        {
            Luid.Value = 0;
            Error = pConvertInterfaceAliasToLuid(Name, &Luid);
            ok(Error == NO_ERROR, "ConvertInterfaceAliasToLuid(\"%ls\"): error %lu\n", Name, Error);
            ok(Luid.Value == Adapter->Luid.Value, "LUID 0x%I64x, expected 0x%I64x\n",
               Luid.Value, Adapter->Luid.Value);

            /* Case-insensitively */
            CharUpperW(Name);
            Luid.Value = 0;
            Error = pConvertInterfaceAliasToLuid(Name, &Luid);
            ok(Error == NO_ERROR, "ConvertInterfaceAliasToLuid(\"%ls\"): error %lu\n", Name, Error);
            ok(Luid.Value == Adapter->Luid.Value, "LUID 0x%I64x, expected 0x%I64x\n",
               Luid.Value, Adapter->Luid.Value);

        }

        /* Keeping the name as stored succeeds, even without write access.
           Never pass anything but the name just read back: that would rename
           a real adapter. */
        if (pNciGetConnectionName(Guid, Name, NameChars * sizeof(WCHAR), NULL) == ERROR_SUCCESS)
        {
            Error = pNciSetConnectionName(Guid, Name);
            ok(Error == ERROR_SUCCESS, "Keeping the name: error %lu\n", Error);
        }
    }

    HeapFree(GetProcessHeap(), 0, Addresses);
    return Found;
}

START_TEST(NciConnectionName)
{
    HMODULE Nci = LoadLibraryW(L"nci.dll");
    WCHAR Name[256];
    GUID AdapterGuid;
    BOOL HaveAdapter;
    DWORD Error, Out;
    NET_LUID Luid;

    pNciGetConnectionName = (FN_NciGetConnectionName)GetProcAddress(Nci, "NciGetConnectionName");
    pNciSetConnectionName = (FN_NciSetConnectionName)GetProcAddress(Nci, "NciSetConnectionName");
    pConvertInterfaceAliasToLuid = (FN_ConvertInterfaceAliasToLuid)
        GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "ConvertInterfaceAliasToLuid");
    if (!pNciGetConnectionName || !pNciSetConnectionName)
    {
        skip("nci.dll is not available\n");
        return;
    }

    /* An adapter without a connection */
    Out = 0xdeadbeef;
    Error = pNciGetConnectionName(&BogusGuid, Name, sizeof(Name), &Out);
    ok(Error == ERROR_FILE_NOT_FOUND, "Unknown adapter: error %lu\n", Error);
    ok(Out == 0xdeadbeef, "Unknown adapter: %lu bytes\n", Out);
    Error = pNciSetConnectionName(&BogusGuid, L"NciApitestBogus");
    ok(Error == ERROR_FILE_NOT_FOUND, "Unknown adapter: error %lu\n", Error);

    /* An unknown alias */
    if (pConvertInterfaceAliasToLuid)
    {
        Luid.Value = 0xdeadbeef;
        Error = pConvertInterfaceAliasToLuid(L"NciApitestNoSuchAlias", &Luid);
        ok(Error == ERROR_INVALID_PARAMETER, "Unknown alias: error %lu\n", Error);
        ok(Luid.Value == 0, "Unknown alias: LUID 0x%I64x\n", Luid.Value);
    }

    HaveAdapter = TestRealAdapter(&AdapterGuid, Name, ARRAYSIZE(Name));
    if (!HaveAdapter)
        skip("No adapter with a network connection\n");

    /* Renaming is not tested here: it would rename a real adapter of the
       machine the test runs on. */
}

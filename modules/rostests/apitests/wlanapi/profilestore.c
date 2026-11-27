/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that WLAN profiles outlive the service
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * A saved profile has to be there again after the WLAN service restarts
 * (or the machine reboots), and its file must not hold the key in the
 * clear: the service keeps each profile as an XML file under
 * ProgramData\Microsoft\Wlansvc\Profiles\Interfaces\{interface}, the key
 * material protected.  Restarts WlanSvc, so it needs the rights to; needs a
 * wireless interface.  Skipped otherwise.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <winreg.h>
#include <winsvc.h>
#include <stdio.h>

static const WCHAR ProfileName[] = L"rostest-store";
static const WCHAR Secret[] = L"rostest-store-passphrase";

static BOOL
WaitServiceState(SC_HANDLE Service, DWORD State)
{
    SERVICE_STATUS Status;
    ULONG i;

    for (i = 0; i < 300; i++)
    {
        if (!QueryServiceStatus(Service, &Status))
            return FALSE;
        if (Status.dwCurrentState == State)
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static BOOL
RestartWlanSvc(VOID)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS Status;
    BOOL Ok = FALSE;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (Manager == NULL)
        return FALSE;
    Service = OpenServiceW(Manager, L"WlanSvc",
                           SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (Service != NULL)
    {
        if (ControlService(Service, SERVICE_CONTROL_STOP, &Status) &&
            WaitServiceState(Service, SERVICE_STOPPED) &&
            StartServiceW(Service, 0, NULL) &&
            WaitServiceState(Service, SERVICE_RUNNING))
        {
            Ok = TRUE;
        }
        CloseServiceHandle(Service);
    }
    CloseServiceHandle(Manager);
    return Ok;
}

static BOOL
FirstInterface(HANDLE Client, GUID *Interface)
{
    PWLAN_INTERFACE_INFO_LIST List = NULL;
    BOOL Found = FALSE;

    if (WlanEnumInterfaces(Client, NULL, &List) == ERROR_SUCCESS &&
        List != NULL && List->dwNumberOfItems != 0)
    {
        *Interface = List->InterfaceInfo[0].InterfaceGuid;
        Found = TRUE;
    }
    if (List != NULL)
        WlanFreeMemory(List);
    return Found;
}

/* Read the profile's file: is it there, protected, and without the key? */
static VOID
CheckProfileFile(const GUID *Interface)
{
    WCHAR Raw[MAX_PATH], Dir[MAX_PATH], Path[MAX_PATH * 2];
    WIN32_FIND_DATAW Find;
    HANDLE Search, File;
    DWORD Size = sizeof(Raw), Read;
    HKEY Key;
    CHAR Text[8192], Name[64], SecretA[64];
    BOOL Found = FALSE;

    wcscpy(Raw, L"%SystemDrive%\\ProgramData");
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                      0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(Key, L"ProgramData", NULL, NULL, (LPBYTE)Raw,
                             &Size) != ERROR_SUCCESS)
            wcscpy(Raw, L"%SystemDrive%\\ProgramData");
        RegCloseKey(Key);
    }
    ExpandEnvironmentStringsW(Raw, Dir, ARRAYSIZE(Dir));

    _snwprintf(Path, ARRAYSIZE(Path),
               L"%s\\Microsoft\\Wlansvc\\Profiles\\Interfaces\\"
               L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}\\*.xml",
               Dir, Interface->Data1, Interface->Data2, Interface->Data3,
               Interface->Data4[0], Interface->Data4[1], Interface->Data4[2],
               Interface->Data4[3], Interface->Data4[4], Interface->Data4[5],
               Interface->Data4[6], Interface->Data4[7]);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    sprintf(Name, "<name>%ls</name>", ProfileName);
    sprintf(SecretA, "%ls", Secret);

    Search = FindFirstFileW(Path, &Find);
    ok(Search != INVALID_HANDLE_VALUE, "No profile files at %ls\n", Path);
    if (Search == INVALID_HANDLE_VALUE)
        return;
    *wcsrchr(Path, L'\\') = UNICODE_NULL;
    do
    {
        WCHAR FilePath[MAX_PATH * 2];

        _snwprintf(FilePath, ARRAYSIZE(FilePath), L"%s\\%s", Path, Find.cFileName);
        FilePath[ARRAYSIZE(FilePath) - 1] = UNICODE_NULL;
        File = CreateFileW(FilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (File == INVALID_HANDLE_VALUE)
            continue;
        Read = 0;
        ReadFile(File, Text, sizeof(Text) - 1, &Read, NULL);
        CloseHandle(File);
        Text[Read] = '\0';
        if (strstr(Text, Name) == NULL)
            continue;

        Found = TRUE;
        ok(strstr(Text, SecretA) == NULL, "%ls holds the key in the clear\n",
           Find.cFileName);
        ok(strstr(Text, "<protected>true</protected>") != NULL,
           "%ls does not mark its key protected\n", Find.cFileName);
    } while (FindNextFileW(Search, &Find));
    FindClose(Search);
    ok(Found, "No profile file names %ls\n", ProfileName);
}

START_TEST(profilestore)
{
    WCHAR Xml[2048];
    HANDLE Client;
    DWORD Negotiated, Ret, Reason = 0, Flags = 0, Granted = 0;
    GUID Interface;
    LPWSTR Stored = NULL;
    ULONG i;

    Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
    if (Ret != ERROR_SUCCESS)
    {
        skip("WlanSvc is not running (%lu)\n", Ret);
        return;
    }
    if (!FirstInterface(Client, &Interface))
    {
        skip("No wireless interface\n");
        WlanCloseHandle(Client, NULL);
        return;
    }

    _snwprintf(Xml, ARRAYSIZE(Xml),
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s-ssid</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>manual</connectionMode>"
        L"<MSM><security>"
        L"<authEncryption><authentication>WPA2PSK</authentication>"
        L"<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
        L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
        L"<keyMaterial>%s</keyMaterial></sharedKey>"
        L"</security></MSM>"
        L"</WLANProfile>",
        ProfileName, ProfileName, Secret);
    Xml[ARRAYSIZE(Xml) - 1] = UNICODE_NULL;

    WlanDeleteProfile(Client, &Interface, ProfileName, NULL);
    Ret = WlanSetProfile(Client, &Interface, 0, Xml, NULL, TRUE, NULL, &Reason);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfile returned %lu (%lu)\n", Ret, Reason);
    WlanCloseHandle(Client, NULL);

    CheckProfileFile(&Interface);

    if (!RestartWlanSvc())
    {
        skip("Cannot restart WlanSvc (%lu)\n", GetLastError());
        Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
        if (Ret == ERROR_SUCCESS)
        {
            WlanDeleteProfile(Client, &Interface, ProfileName, NULL);
            WlanCloseHandle(Client, NULL);
        }
        return;
    }

    /* The service finds its interfaces again as it starts. */
    for (i = 0; i < 100; i++)
    {
        Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
        if (Ret == ERROR_SUCCESS)
            break;
        Sleep(100);
    }
    ok(Ret == ERROR_SUCCESS, "WlanOpenHandle after the restart returned %lu\n", Ret);
    if (Ret != ERROR_SUCCESS)
        return;
    for (i = 0; i < 100; i++)
    {
        GUID Again;
        if (FirstInterface(Client, &Again) && IsEqualGUID(&Again, &Interface))
            break;
        Sleep(100);
    }

    Ret = WlanGetProfile(Client, &Interface, ProfileName, NULL, &Stored,
                         &Flags, &Granted);
    ok(Ret == ERROR_SUCCESS, "Profile gone after the restart: %lu\n", Ret);
    if (Ret == ERROR_SUCCESS && Stored != NULL)
    {
        ok(wcsstr(Stored, L"<name>rostest-store-ssid</name>") != NULL,
           "Restored profile lost its SSID\n");
        /* A caller allowed to see the key sees the original one. */
        if ((Granted & WLAN_WRITE_ACCESS) == WLAN_WRITE_ACCESS)
        {
            ok(wcsstr(Stored, Secret) != NULL,
               "Restored profile does not carry its key\n");
            ok(wcsstr(Stored, L"<protected>false</protected>") != NULL,
               "Restored key not handed out in the clear\n");
        }
        WlanFreeMemory(Stored);
    }

    Ret = WlanDeleteProfile(Client, &Interface, ProfileName, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanDeleteProfile returned %lu\n", Ret);
    WlanCloseHandle(Client, NULL);
}

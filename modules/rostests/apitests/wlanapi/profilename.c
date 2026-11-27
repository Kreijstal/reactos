/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for WLAN profile names: case and XML escaping
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * Profile names are case-sensitive: two names that differ only in case are
 * two profiles.  The name is the text of the profile's <name> element, so
 * the characters XML escapes (& < > " ') are written as entity references and
 * read back as the characters, also after the service has reloaded its store.
 * Needs a wireless interface; the reload part also needs the rights to
 * restart WlanSvc.  Skipped otherwise.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <winsvc.h>
#include <stdio.h>

static const WCHAR Upper[] = L"rostest-Name-Case";
static const WCHAR Lower[] = L"rostest-name-case";
/* As written in the XML, and as the name it stands for. */
static const WCHAR EscapedXmlName[] = L"rostest-a&amp;b&lt;c&gt;d&quot;e&apos;f&#x41;&#66;";
static const WCHAR EscapedName[] = L"rostest-a&b<c>d\"e'fAB";
static const WCHAR Renamed[] = L"rostest-<renamed> & \"quoted\" 'too'";

static DWORD
SetProfile(HANDLE Client, const GUID *Interface, PCWSTR XmlName, BOOL Overwrite)
{
    WCHAR Xml[1024];
    DWORD Reason = 0;

    _snwprintf(Xml, ARRAYSIZE(Xml),
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>rostest-name-ssid</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>manual</connectionMode>"
        L"<MSM><security><authEncryption>"
        L"<authentication>open</authentication><encryption>none</encryption>"
        L"<useOneX>false</useOneX>"
        L"</authEncryption></security></MSM>"
        L"</WLANProfile>",
        XmlName);
    Xml[ARRAYSIZE(Xml) - 1] = UNICODE_NULL;
    return WlanSetProfile(Client, Interface, 0, Xml, NULL, Overwrite, NULL, &Reason);
}

/* How many profiles on the interface are called exactly Name; -1 without a
 * list.  Checks nothing itself, so it can stand in an ok(). */
static LONG
CountNamed(HANDLE Client, const GUID *Interface, PCWSTR Name)
{
    PWLAN_PROFILE_INFO_LIST List = NULL;
    LONG Count = 0;
    DWORD i;

    if (WlanGetProfileList(Client, Interface, NULL, &List) != ERROR_SUCCESS ||
        List == NULL)
    {
        return -1;
    }
    for (i = 0; i < List->dwNumberOfItems; i++)
    {
        if (wcscmp(List->ProfileInfo[i].strProfileName, Name) == 0)
            Count++;
    }
    WlanFreeMemory(List);
    return Count;
}

static DWORD
GetProfileXml(HANDLE Client, const GUID *Interface, PCWSTR Name, LPWSTR *Xml)
{
    DWORD Flags = 0, Granted = 0;

    *Xml = NULL;
    return WlanGetProfile(Client, Interface, Name, NULL, Xml, &Flags, &Granted);
}

static VOID
CleanUp(HANDLE Client, const GUID *Interface)
{
    WlanDeleteProfile(Client, Interface, Upper, NULL);
    WlanDeleteProfile(Client, Interface, Lower, NULL);
    WlanDeleteProfile(Client, Interface, EscapedName, NULL);
    WlanDeleteProfile(Client, Interface, Renamed, NULL);
    WlanDeleteProfile(Client, Interface, L"rostest-name-none", NULL);
}

static VOID
TestCase(HANDLE Client, const GUID *Interface)
{
    LPWSTR Xml;
    DWORD Ret;

    Ret = SetProfile(Client, Interface, Upper, FALSE);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfile(%ls) returned %lu\n", Upper, Ret);

    /* The name in another case is no name of a profile. */
    Ret = GetProfileXml(Client, Interface, Lower, &Xml);
    ok(Ret == ERROR_NOT_FOUND, "WlanGetProfile(%ls) returned %lu\n", Lower, Ret);
    if (Ret == ERROR_SUCCESS && Xml != NULL)
        WlanFreeMemory(Xml);
    Ret = WlanSetProfilePosition(Client, Interface, Lower, 0, NULL);
    ok(Ret == ERROR_NOT_FOUND, "WlanSetProfilePosition(%ls) returned %lu\n", Lower, Ret);
    Ret = WlanRenameProfile(Client, Interface, Lower, L"rostest-name-none", NULL);
    ok(Ret == ERROR_NOT_FOUND, "WlanRenameProfile(%ls) returned %lu\n", Lower, Ret);
    Ret = WlanDeleteProfile(Client, Interface, Lower, NULL);
    ok(Ret == ERROR_NOT_FOUND, "WlanDeleteProfile(%ls) returned %lu\n", Lower, Ret);

    /* ... so it can be a second profile, which does not replace the first. */
    Ret = SetProfile(Client, Interface, Lower, FALSE);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfile(%ls) returned %lu\n", Lower, Ret);
    ok(CountNamed(Client, Interface, Upper) == 1, "%ld profiles '%ls'\n",
       CountNamed(Client, Interface, Upper), Upper);
    ok(CountNamed(Client, Interface, Lower) == 1, "%ld profiles '%ls'\n",
       CountNamed(Client, Interface, Lower), Lower);

    /* Deleting one leaves the other. */
    Ret = WlanDeleteProfile(Client, Interface, Lower, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanDeleteProfile(%ls) returned %lu\n", Lower, Ret);
    ok(CountNamed(Client, Interface, Upper) == 1, "%ld profiles '%ls' after the delete\n",
       CountNamed(Client, Interface, Upper), Upper);
    ok(CountNamed(Client, Interface, Lower) == 0, "%ld profiles '%ls' after the delete\n",
       CountNamed(Client, Interface, Lower), Lower);
}

/* The escaped profile is known by its name, before and after a rename. */
static VOID
TestEscaped(HANDLE Client, const GUID *Interface, PCWSTR Name, PCWSTR Escaped)
{
    LPWSTR Xml;
    DWORD Ret;

    ok(CountNamed(Client, Interface, Name) == 1, "%ld profiles '%ls'\n",
       CountNamed(Client, Interface, Name), Name);

    Ret = GetProfileXml(Client, Interface, Name, &Xml);
    ok(Ret == ERROR_SUCCESS, "WlanGetProfile(%ls) returned %lu\n", Name, Ret);
    if (Ret == ERROR_SUCCESS && Xml != NULL)
    {
        /* Still escaped in the XML. */
        ok(wcsstr(Xml, Escaped) != NULL, "Profile XML does not carry '%ls'\n", Escaped);
        WlanFreeMemory(Xml);
    }
}

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
        Ok = ControlService(Service, SERVICE_CONTROL_STOP, &Status) &&
             WaitServiceState(Service, SERVICE_STOPPED) &&
             StartServiceW(Service, 0, NULL) &&
             WaitServiceState(Service, SERVICE_RUNNING);
        CloseServiceHandle(Service);
    }
    CloseServiceHandle(Manager);
    return Ok;
}

/* The first wireless interface, waiting for the service to see it. */
static BOOL
FirstInterface(HANDLE Client, GUID *Interface)
{
    PWLAN_INTERFACE_INFO_LIST List = NULL;
    BOOL Found = FALSE;
    ULONG i;

    for (i = 0; i < 100 && !Found; i++)
    {
        if (WlanEnumInterfaces(Client, NULL, &List) == ERROR_SUCCESS &&
            List != NULL && List->dwNumberOfItems != 0)
        {
            *Interface = List->InterfaceInfo[0].InterfaceGuid;
            Found = TRUE;
        }
        if (List != NULL)
        {
            WlanFreeMemory(List);
            List = NULL;
        }
        if (!Found)
            Sleep(100);
    }
    return Found;
}

START_TEST(profilename)
{
    HANDLE Client;
    DWORD Negotiated, Ret;
    GUID Interface, Again;
    WCHAR EscapedRenamed[256];
    PCWSTR p;
    PWSTR q;
    BOOL Restarted;
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
    CleanUp(Client, &Interface);

    TestCase(Client, &Interface);

    /* Renamed, escaped by hand the way the service writes it. */
    for (p = Renamed, q = EscapedRenamed; *p != UNICODE_NULL; p++)
    {
        switch (*p)
        {
            case L'&':  wcscpy(q, L"&amp;");  break;
            case L'<':  wcscpy(q, L"&lt;");   break;
            case L'>':  wcscpy(q, L"&gt;");   break;
            case L'"':  wcscpy(q, L"&quot;"); break;
            case L'\'': wcscpy(q, L"&apos;"); break;
            default:    q[0] = *p; q[1] = UNICODE_NULL; break;
        }
        q += wcslen(q);
    }

    Ret = SetProfile(Client, &Interface, EscapedXmlName, FALSE);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfile(%ls) returned %lu\n", EscapedXmlName, Ret);
    ok(CountNamed(Client, &Interface, EscapedXmlName) == 0,
       "The profile is known by its escaped name\n");
    TestEscaped(Client, &Interface, EscapedName, EscapedXmlName);

    Ret = WlanRenameProfile(Client, &Interface, EscapedName, Renamed, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanRenameProfile returned %lu\n", Ret);
    TestEscaped(Client, &Interface, Renamed, EscapedRenamed);

    /* The names are the same once the service has read its store again. */
    WlanCloseHandle(Client, NULL);
    Restarted = RestartWlanSvc();
    if (!Restarted)
        skip("Cannot restart WlanSvc (%lu)\n", GetLastError());
    Ret = ERROR_SERVICE_NOT_ACTIVE;
    for (i = 0; i < 100 && Ret != ERROR_SUCCESS; i++)
    {
        Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
        if (Ret != ERROR_SUCCESS)
            Sleep(100);
    }
    ok(Ret == ERROR_SUCCESS, "WlanOpenHandle after the restart returned %lu\n", Ret);
    if (Ret != ERROR_SUCCESS)
        return;
    ok(FirstInterface(Client, &Again) && IsEqualGUID(&Again, &Interface),
       "The interface did not come back\n");
    if (!Restarted)
    {
        CleanUp(Client, &Interface);
        WlanCloseHandle(Client, NULL);
        return;
    }

    TestEscaped(Client, &Interface, Renamed, EscapedRenamed);
    ok(CountNamed(Client, &Interface, Upper) == 1, "%ld profiles '%ls' after the restart\n",
       CountNamed(Client, &Interface, Upper), Upper);
    ok(CountNamed(Client, &Interface, Lower) == 0, "%ld profiles '%ls' after the restart\n",
       CountNamed(Client, &Interface, Lower), Lower);

    CleanUp(Client, &Interface);
    WlanCloseHandle(Client, NULL);
}

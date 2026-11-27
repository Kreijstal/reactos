/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the WLAN profile preference order
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * WlanSetProfile documents the order: a new profile goes to the top of the
 * list after the group policy profiles, and overwriting an existing profile
 * does not change its position.  Needs a wireless interface; skipped
 * without one.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <stdio.h>

static const WCHAR *ProfileNames[] =
{
    L"rostest-order-a", L"rostest-order-b", L"rostest-order-c"
};

static void
BuildProfileXml(PWCHAR Buffer, SIZE_T Count, PCWSTR Name, PCWSTR Mode)
{
    _snwprintf(Buffer, Count,
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s-ssid</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>%s</connectionMode>"
        L"<MSM><security><authEncryption>"
        L"<authentication>open</authentication><encryption>none</encryption>"
        L"<useOneX>false</useOneX>"
        L"</authEncryption></security></MSM>"
        L"</WLANProfile>",
        Name, Name, Mode);
    Buffer[Count - 1] = UNICODE_NULL;
}

/* Position of Name in the interface's profile list, or -1. */
static LONG
ProfilePosition(HANDLE Client, const GUID *Interface, PCWSTR Name)
{
    PWLAN_PROFILE_INFO_LIST List = NULL;
    LONG Position = -1;
    DWORD i, Ret;

    Ret = WlanGetProfileList(Client, Interface, NULL, &List);
    ok(Ret == ERROR_SUCCESS, "WlanGetProfileList returned %lu\n", Ret);
    if (Ret != ERROR_SUCCESS || List == NULL)
        return -1;

    for (i = 0; i < List->dwNumberOfItems; i++)
    {
        if (wcscmp(List->ProfileInfo[i].strProfileName, Name) == 0)
        {
            Position = (LONG)i;
            break;
        }
    }
    WlanFreeMemory(List);
    return Position;
}

static DWORD
SetProfile(HANDLE Client, const GUID *Interface, PCWSTR Name, PCWSTR Mode,
           BOOL Overwrite)
{
    WCHAR Xml[1024];
    DWORD Reason = 0;

    BuildProfileXml(Xml, ARRAYSIZE(Xml), Name, Mode);
    return WlanSetProfile(Client, Interface, 0, Xml, NULL, Overwrite, NULL,
                          &Reason);
}

START_TEST(profileorder)
{
    HANDLE Client;
    DWORD Negotiated, Ret, i;
    PWLAN_INTERFACE_INFO_LIST Interfaces = NULL;
    GUID Interface;
    LONG A, B, C;
    LPWSTR Xml = NULL;
    DWORD Flags = 0, Granted = 0;

    Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
    if (Ret != ERROR_SUCCESS)
    {
        skip("WlanSvc is not running (%lu)\n", Ret);
        return;
    }

    Ret = WlanEnumInterfaces(Client, NULL, &Interfaces);
    if (Ret != ERROR_SUCCESS || Interfaces == NULL ||
        Interfaces->dwNumberOfItems == 0)
    {
        skip("No wireless interface\n");
        if (Interfaces != NULL)
            WlanFreeMemory(Interfaces);
        WlanCloseHandle(Client, NULL);
        return;
    }
    Interface = Interfaces->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(Interfaces);

    for (i = 0; i < ARRAYSIZE(ProfileNames); i++)
        WlanDeleteProfile(Client, &Interface, ProfileNames[i], NULL);

    /* New profiles: each one lands on top of the ones before it. */
    for (i = 0; i < ARRAYSIZE(ProfileNames); i++)
    {
        Ret = SetProfile(Client, &Interface, ProfileNames[i], L"manual", FALSE);
        ok(Ret == ERROR_SUCCESS, "WlanSetProfile(%ls) returned %lu\n",
           ProfileNames[i], Ret);
    }
    A = ProfilePosition(Client, &Interface, ProfileNames[0]);
    B = ProfilePosition(Client, &Interface, ProfileNames[1]);
    C = ProfilePosition(Client, &Interface, ProfileNames[2]);
    ok(A >= 0 && B >= 0 && C >= 0, "Profiles missing: %ld %ld %ld\n", A, B, C);
    ok(C < B && B < A, "New profiles not added at the top: a %ld b %ld c %ld\n",
       A, B, C);

    /* Overwriting keeps the position. */
    Ret = SetProfile(Client, &Interface, ProfileNames[1], L"auto", TRUE);
    ok(Ret == ERROR_SUCCESS, "Overwriting WlanSetProfile returned %lu\n", Ret);
    ok(ProfilePosition(Client, &Interface, ProfileNames[1]) == B,
       "Overwritten profile moved from %ld to %ld\n",
       B, ProfilePosition(Client, &Interface, ProfileNames[1]));
    ok(ProfilePosition(Client, &Interface, ProfileNames[0]) == A &&
       ProfilePosition(Client, &Interface, ProfileNames[2]) == C,
       "Overwrite moved the other profiles\n");

    /* ... and the stored profile is the new one. */
    Ret = WlanGetProfile(Client, &Interface, ProfileNames[1], NULL, &Xml,
                         &Flags, &Granted);
    ok(Ret == ERROR_SUCCESS, "WlanGetProfile returned %lu\n", Ret);
    if (Ret == ERROR_SUCCESS && Xml != NULL)
    {
        ok(wcsstr(Xml, L"<connectionMode>auto</connectionMode>") != NULL,
           "Overwrite did not replace the profile\n");
        WlanFreeMemory(Xml);
    }

    /* The last profile on the list, overwritten, stays the last. */
    Ret = SetProfile(Client, &Interface, ProfileNames[0], L"manual", TRUE);
    ok(Ret == ERROR_SUCCESS, "Overwriting WlanSetProfile returned %lu\n", Ret);
    ok(ProfilePosition(Client, &Interface, ProfileNames[0]) == A,
       "Overwritten last profile moved from %ld to %ld\n",
       A, ProfilePosition(Client, &Interface, ProfileNames[0]));

    /* Without bOverwrite an existing profile is refused, and nothing moves. */
    Ret = SetProfile(Client, &Interface, ProfileNames[2], L"manual", FALSE);
    ok(Ret == ERROR_ALREADY_EXISTS, "WlanSetProfile returned %lu\n", Ret);
    ok(ProfilePosition(Client, &Interface, ProfileNames[2]) == C,
       "Refused WlanSetProfile moved the profile\n");

    for (i = 0; i < ARRAYSIZE(ProfileNames); i++)
    {
        Ret = WlanDeleteProfile(Client, &Interface, ProfileNames[i], NULL);
        ok(Ret == ERROR_SUCCESS, "WlanDeleteProfile(%ls) returned %lu\n",
           ProfileNames[i], Ret);
    }

    WlanCloseHandle(Client, NULL);
}

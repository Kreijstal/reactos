/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for WlanSetProfilePosition, WlanSetProfileList and
 *              WlanRenameProfile
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * The preference list can be reordered one profile at a time or as a whole,
 * and a profile can be renamed in place.  The profiles already on the
 * interface keep their order.  Needs a wireless interface; skipped without
 * one.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <stdio.h>

static const WCHAR *Names[] =
{
    L"rostest-edit-a", L"rostest-edit-b", L"rostest-edit-c"
};
static const WCHAR Renamed[] = L"rostest-edit-renamed";

static DWORD
SetProfile(HANDLE Client, const GUID *Interface, PCWSTR Name)
{
    WCHAR Xml[1024];
    DWORD Reason = 0;

    _snwprintf(Xml, ARRAYSIZE(Xml),
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s-ssid</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>manual</connectionMode>"
        L"<MSM><security><authEncryption>"
        L"<authentication>open</authentication><encryption>none</encryption>"
        L"<useOneX>false</useOneX>"
        L"</authEncryption></security></MSM>"
        L"</WLANProfile>",
        Name, Name);
    Xml[ARRAYSIZE(Xml) - 1] = UNICODE_NULL;
    return WlanSetProfile(Client, Interface, 0, Xml, NULL, FALSE, NULL, &Reason);
}

/* The interface's profile list; WlanFreeMemory it. */
static PWLAN_PROFILE_INFO_LIST
GetList(HANDLE Client, const GUID *Interface)
{
    PWLAN_PROFILE_INFO_LIST List = NULL;
    DWORD Ret;

    Ret = WlanGetProfileList(Client, Interface, NULL, &List);
    ok(Ret == ERROR_SUCCESS, "WlanGetProfileList returned %lu\n", Ret);
    return (Ret == ERROR_SUCCESS) ? List : NULL;
}

/* Position of Name in the list, -1 if it is not on it, -2 if there is no
 * list.  Checks nothing itself, so it can stand in an ok(). */
static LONG
Position(HANDLE Client, const GUID *Interface, PCWSTR Name)
{
    PWLAN_PROFILE_INFO_LIST List = NULL;
    LONG Found = -1;
    DWORD i;

    if (WlanGetProfileList(Client, Interface, NULL, &List) != ERROR_SUCCESS ||
        List == NULL)
    {
        return -2;
    }
    for (i = 0; i < List->dwNumberOfItems; i++)
    {
        if (wcscmp(List->ProfileInfo[i].strProfileName, Name) == 0)
            Found = (LONG)i;
    }
    WlanFreeMemory(List);
    return Found;
}

static VOID
TestPosition(HANDLE Client, const GUID *Interface)
{
    LONG A, B, C;
    DWORD Ret;

    /* Added in turn: c b a, on top. */
    A = Position(Client, Interface, Names[0]);
    B = Position(Client, Interface, Names[1]);
    C = Position(Client, Interface, Names[2]);
    ok(C >= 0 && B == C + 1 && A == C + 2, "Unexpected order a %ld b %ld c %ld\n",
       A, B, C);
    if (C < 0)
        return;

    Ret = WlanSetProfilePosition(Client, Interface, NULL, 0, NULL);
    ok(Ret == ERROR_INVALID_PARAMETER, "NULL name: %lu\n", Ret);
    Ret = WlanSetProfilePosition(Client, Interface, Names[0], C, (PVOID)1);
    ok(Ret == ERROR_INVALID_PARAMETER, "pReserved: %lu\n", Ret);

    /* a to the front of the three: a c b. */
    Ret = WlanSetProfilePosition(Client, Interface, Names[0], C, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfilePosition returned %lu\n", Ret);
    ok(Position(Client, Interface, Names[0]) == C &&
       Position(Client, Interface, Names[2]) == C + 1 &&
       Position(Client, Interface, Names[1]) == C + 2,
       "a c b expected, got a %ld b %ld c %ld\n",
       Position(Client, Interface, Names[0]),
       Position(Client, Interface, Names[1]),
       Position(Client, Interface, Names[2]));

    /* c down one: a b c. */
    Ret = WlanSetProfilePosition(Client, Interface, Names[2], C + 2, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfilePosition returned %lu\n", Ret);
    ok(Position(Client, Interface, Names[0]) == C &&
       Position(Client, Interface, Names[1]) == C + 1 &&
       Position(Client, Interface, Names[2]) == C + 2,
       "a b c expected, got a %ld b %ld c %ld\n",
       Position(Client, Interface, Names[0]),
       Position(Client, Interface, Names[1]),
       Position(Client, Interface, Names[2]));
}

static VOID
TestList(HANDLE Client, const GUID *Interface)
{
    PWLAN_PROFILE_INFO_LIST List = GetList(Client, Interface);
    LPCWSTR *Order;
    LPCWSTR Bad[2];
    DWORD i, Ret, Count, First = MAXDWORD;

    if (List == NULL)
        return;
    Count = List->dwNumberOfItems;
    Order = HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*Order));
    if (Order == NULL)
    {
        WlanFreeMemory(List);
        return;
    }

    /* Every profile in its place, except that the three test profiles (a b c
     * from TestPosition) come in reverse: c b a. */
    for (i = 0; i < Count; i++)
    {
        Order[i] = List->ProfileInfo[i].strProfileName;
        if (wcscmp(Order[i], Names[0]) == 0)
            First = i;
    }
    ok(First != MAXDWORD && First + 2 < Count, "Test profiles missing\n");
    if (First == MAXDWORD || First + 2 >= Count)
        goto done;
    Order[First] = Names[2];
    Order[First + 2] = Names[0];

    Ret = WlanSetProfileList(Client, Interface, 0, Order, NULL);
    ok(Ret == ERROR_INVALID_PARAMETER, "No items: %lu\n", Ret);

    /* A name twice. */
    Bad[0] = Names[1];
    Bad[1] = Names[1];
    Ret = WlanSetProfileList(Client, Interface, 2, Bad, NULL);
    ok(Ret == ERROR_INVALID_PARAMETER, "Repeated name: %lu\n", Ret);

    /* A name that is no profile. */
    Bad[0] = Names[1];
    Bad[1] = L"rostest-edit-none";
    Ret = WlanSetProfileList(Client, Interface, 2, Bad, NULL);
    ok(Ret == ERROR_NOT_FOUND, "Unknown name: %lu\n", Ret);

    /* Nothing of that moved anything. */
    ok(Position(Client, Interface, Names[0]) == (LONG)First &&
       Position(Client, Interface, Names[2]) == (LONG)First + 2,
       "A refused list changed the order\n");

    Ret = WlanSetProfileList(Client, Interface, Count, Order, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanSetProfileList returned %lu\n", Ret);
    ok(Position(Client, Interface, Names[2]) == (LONG)First &&
       Position(Client, Interface, Names[1]) == (LONG)First + 1 &&
       Position(Client, Interface, Names[0]) == (LONG)First + 2,
       "c b a expected, got a %ld b %ld c %ld\n",
       Position(Client, Interface, Names[0]),
       Position(Client, Interface, Names[1]),
       Position(Client, Interface, Names[2]));

    /* The other profiles kept their places. */
    {
        PWLAN_PROFILE_INFO_LIST After = GetList(Client, Interface);

        if (After != NULL)
        {
            ok(After->dwNumberOfItems == Count, "%lu profiles, was %lu\n",
               After->dwNumberOfItems, Count);
            for (i = 0; i < Count && i < After->dwNumberOfItems; i++)
            {
                if (i >= First && i <= First + 2)
                    continue;
                ok(wcscmp(After->ProfileInfo[i].strProfileName, Order[i]) == 0,
                   "Position %lu: '%ls', expected '%ls'\n", i,
                   After->ProfileInfo[i].strProfileName, Order[i]);
            }
            WlanFreeMemory(After);
        }
    }

done:
    HeapFree(GetProcessHeap(), 0, (PVOID)Order);
    WlanFreeMemory(List);
}

static VOID
TestRename(HANDLE Client, const GUID *Interface)
{
    LPWSTR Xml = NULL;
    DWORD Ret, Flags = 0, Granted = 0;
    LONG Before;

    Before = Position(Client, Interface, Names[1]);

    Ret = WlanRenameProfile(Client, Interface, NULL, Renamed, NULL);
    ok(Ret == ERROR_INVALID_PARAMETER, "NULL old name: %lu\n", Ret);
    Ret = WlanRenameProfile(Client, Interface, Names[1], NULL, NULL);
    ok(Ret == ERROR_INVALID_PARAMETER, "NULL new name: %lu\n", Ret);
    Ret = WlanRenameProfile(Client, Interface, L"rostest-edit-none", Renamed, NULL);
    ok(Ret == ERROR_NOT_FOUND, "Unknown profile: %lu\n", Ret);

    Ret = WlanRenameProfile(Client, Interface, Names[1], Renamed, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanRenameProfile returned %lu\n", Ret);

    ok(Position(Client, Interface, Names[1]) == -1, "Old name still listed\n");
    ok(Position(Client, Interface, Renamed) == Before,
       "Renamed profile at %ld, was at %ld\n",
       Position(Client, Interface, Renamed), Before);

    Ret = WlanGetProfile(Client, Interface, Names[1], NULL, &Xml, &Flags, &Granted);
    ok(Ret == ERROR_NOT_FOUND, "WlanGetProfile(old name) returned %lu\n", Ret);
    if (Ret == ERROR_SUCCESS && Xml != NULL)
        WlanFreeMemory(Xml);

    Xml = NULL;
    Ret = WlanGetProfile(Client, Interface, Renamed, NULL, &Xml, &Flags, &Granted);
    ok(Ret == ERROR_SUCCESS, "WlanGetProfile(new name) returned %lu\n", Ret);
    if (Ret == ERROR_SUCCESS && Xml != NULL)
    {
        /* The profile's own name changes, its SSID does not. */
        ok(wcsstr(Xml, L"<name>rostest-edit-renamed</name>") != NULL,
           "Profile XML does not carry the new name\n");
        ok(wcsstr(Xml, L"<name>rostest-edit-b-ssid</name>") != NULL,
           "Profile XML lost its SSID\n");
        WlanFreeMemory(Xml);
    }
}

START_TEST(profileedit)
{
    HANDLE Client;
    DWORD Negotiated, Ret, i;
    PWLAN_INTERFACE_INFO_LIST Interfaces = NULL;
    GUID Interface;

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

    for (i = 0; i < ARRAYSIZE(Names); i++)
        WlanDeleteProfile(Client, &Interface, Names[i], NULL);
    WlanDeleteProfile(Client, &Interface, Renamed, NULL);

    for (i = 0; i < ARRAYSIZE(Names); i++)
    {
        Ret = SetProfile(Client, &Interface, Names[i]);
        ok(Ret == ERROR_SUCCESS, "WlanSetProfile(%ls) returned %lu\n", Names[i], Ret);
    }

    /* c b a -> a b c -> c b a, then b renamed. */
    TestPosition(Client, &Interface);
    TestList(Client, &Interface);
    TestRename(Client, &Interface);

    for (i = 0; i < ARRAYSIZE(Names); i++)
        WlanDeleteProfile(Client, &Interface, Names[i], NULL);
    WlanDeleteProfile(Client, &Interface, Renamed, NULL);
    WlanCloseHandle(Client, NULL);
}

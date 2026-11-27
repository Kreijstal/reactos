/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that auto-configuration walks the profile list on failure
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * Two auto-connect profiles for the same network: the preferred one with a
 * wrong passphrase, the next one with the right passphrase.  After the
 * preferred one fails, auto-configuration has to connect with the next one
 * rather than retry the failing one forever.
 *
 * Needs a wireless interface in range of a WPA2-PSK network, named by the
 * environment: WLANAPI_TEST_SSID and WLANAPI_TEST_KEY.  Skipped otherwise.
 * Takes up to WAIT_SECONDS: the service retries a down interface about once
 * a minute.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <stdio.h>

#define WAIT_SECONDS 240

static const WCHAR BadProfile[] = L"rostest-acm-bad";
static const WCHAR GoodProfile[] = L"rostest-acm-good";

static DWORD
SetPskProfile(HANDLE Client, const GUID *Interface, PCWSTR Name, PCWSTR Ssid,
              PCWSTR Key)
{
    WCHAR Xml[2048];
    DWORD Reason = 0, Ret;

    _snwprintf(Xml, ARRAYSIZE(Xml),
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>auto</connectionMode>"
        L"<MSM><security>"
        L"<authEncryption><authentication>WPA2PSK</authentication>"
        L"<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
        L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
        L"<keyMaterial>%s</keyMaterial></sharedKey>"
        L"</security></MSM>"
        L"</WLANProfile>",
        Name, Ssid, Key);
    Xml[ARRAYSIZE(Xml) - 1] = UNICODE_NULL;

    Ret = WlanSetProfile(Client, Interface, 0, Xml, NULL, TRUE, NULL, &Reason);
    SecureZeroMemory(Xml, sizeof(Xml));
    return Ret;
}

static LONG
ProfilePosition(HANDLE Client, const GUID *Interface, PCWSTR Name)
{
    PWLAN_PROFILE_INFO_LIST List = NULL;
    LONG Position = -1;
    DWORD i;

    if (WlanGetProfileList(Client, Interface, NULL, &List) != ERROR_SUCCESS ||
        List == NULL)
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

/* Is the interface connected, and with which profile? */
static BOOL
ConnectedProfile(HANDLE Client, const GUID *Interface, PWCHAR Name, SIZE_T Count)
{
    PWLAN_CONNECTION_ATTRIBUTES Attributes = NULL;
    DWORD Size = 0;
    BOOL Connected = FALSE;

    if (WlanQueryInterface(Client, Interface,
                           wlan_intf_opcode_current_connection, NULL, &Size,
                           (PVOID *)&Attributes, NULL) != ERROR_SUCCESS ||
        Attributes == NULL)
        return FALSE;

    if (Attributes->isState == wlan_interface_state_connected)
    {
        wcsncpy(Name, Attributes->strProfileName, Count - 1);
        Name[Count - 1] = UNICODE_NULL;
        Connected = TRUE;
    }
    WlanFreeMemory(Attributes);
    return Connected;
}

START_TEST(acmfallback)
{
    WCHAR Ssid[64], Key[80], Current[WLAN_MAX_NAME_LENGTH];
    HANDLE Client;
    DWORD Negotiated, Ret, Second;
    PWLAN_INTERFACE_INFO_LIST Interfaces = NULL;
    GUID Interface;
    WLAN_CONNECTION_PARAMETERS Params;
    BOOL Done = FALSE;

    if (!GetEnvironmentVariableW(L"WLANAPI_TEST_SSID", Ssid, ARRAYSIZE(Ssid)) ||
        !GetEnvironmentVariableW(L"WLANAPI_TEST_KEY", Key, ARRAYSIZE(Key)))
    {
        skip("WLANAPI_TEST_SSID / WLANAPI_TEST_KEY not set\n");
        return;
    }

    Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
    if (Ret != ERROR_SUCCESS)
    {
        skip("WlanSvc is not running (%lu)\n", Ret);
        SecureZeroMemory(Key, sizeof(Key));
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
        SecureZeroMemory(Key, sizeof(Key));
        return;
    }
    Interface = Interfaces->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(Interfaces);

    WlanDeleteProfile(Client, &Interface, BadProfile, NULL);
    WlanDeleteProfile(Client, &Interface, GoodProfile, NULL);

    /* The failing profile has to come first in the list.  Where a new
     * profile lands is the profile order test's business; this one only
     * needs the order, so it adds them in whichever order gives it. */
    ok(SetPskProfile(Client, &Interface, GoodProfile, Ssid, Key) == ERROR_SUCCESS,
       "WlanSetProfile(good) failed\n");
    ok(SetPskProfile(Client, &Interface, BadProfile, Ssid,
                     L"rostest-not-the-passphrase") == ERROR_SUCCESS,
       "WlanSetProfile(bad) failed\n");
    if (ProfilePosition(Client, &Interface, BadProfile) >
        ProfilePosition(Client, &Interface, GoodProfile))
    {
        WlanDeleteProfile(Client, &Interface, GoodProfile, NULL);
        ok(SetPskProfile(Client, &Interface, GoodProfile, Ssid, Key) ==
           ERROR_SUCCESS, "WlanSetProfile(good) failed\n");
    }
    SecureZeroMemory(Key, sizeof(Key));
    ok(ProfilePosition(Client, &Interface, BadProfile) <
       ProfilePosition(Client, &Interface, GoodProfile),
       "Could not put the failing profile first\n");

    /* Start with the failing one, as a user or the ACM itself would. */
    ZeroMemory(&Params, sizeof(Params));
    Params.wlanConnectionMode = wlan_connection_mode_profile;
    Params.strProfile = BadProfile;
    Params.dot11BssType = dot11_BSS_type_infrastructure;
    Ret = WlanConnect(Client, &Interface, &Params, NULL);
    ok(Ret == ERROR_SUCCESS, "WlanConnect(bad) returned %lu\n", Ret);

    for (Second = 0; Second < WAIT_SECONDS && !Done; Second++)
    {
        Sleep(1000);
        if (ConnectedProfile(Client, &Interface, Current, ARRAYSIZE(Current)))
        {
            if (wcscmp(Current, GoodProfile) == 0)
                Done = TRUE;
        }
        if (Second % 10 == 0)
        {
            trace("%lu s: %ls\n", Second,
                  ConnectedProfile(Client, &Interface, Current,
                                   ARRAYSIZE(Current)) ? Current : L"(down)");
        }
    }
    ok(Done, "Not connected with '%ls' after %u s\n", GoodProfile, WAIT_SECONDS);
    if (Done)
        trace("connected with '%ls' after %lu s\n", GoodProfile, Second);

    WlanDeleteProfile(Client, &Interface, BadProfile, NULL);
    WlanDeleteProfile(Client, &Interface, GoodProfile, NULL);
    WlanCloseHandle(Client, NULL);
}

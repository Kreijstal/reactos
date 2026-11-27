/*
 * PROJECT:     ReactOS WLAN Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        base/services/wlansvc/profile.c
 * PURPOSE:     Profile storage + minimal WLAN profile XML parser
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 *
 * Minimal element-driven parser for the Windows WLANProfile XML schema (not a
 * full XML parser), covering open and WPA2-PSK profiles; no EAP/enterprise.
 *
 * All store routines run with WlanSvcLock held by the caller.
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/*
 * Return a pointer just past the first "<tag ...>", or NULL.  Tag matching is
 * case-insensitive and tolerates attributes.
 */
static LPCWSTR
XmlFindOpen(LPCWSTR xml, LPCWSTR tag)
{
    size_t tlen = wcslen(tag);
    LPCWSTR p = xml;

    while ((p = wcschr(p, L'<')) != NULL)
    {
        LPCWSTR q = p + 1;
        if (_wcsnicmp(q, tag, tlen) == 0)
        {
            WCHAR c = q[tlen];
            if (c == L'>' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'/')
            {
                /* advance to end of the opening tag */
                LPCWSTR gt = wcschr(q, L'>');
                if (gt == NULL)
                    return NULL;
                if (gt > q && gt[-1] == L'/')
                    return NULL;   /* self-closed, no text content */
                return gt + 1;
            }
        }
        p++;
    }
    return NULL;
}

/*
 * The character the entity reference at p ("&...;") stands for, in *c, and
 * the length of the reference; 0 if p is no reference this parser knows.
 */
static size_t
XmlDecodeEntity(LPCWSTR p, LPCWSTR end, PULONG c)
{
    static const struct
    {
        LPCWSTR Name;
        WCHAR Char;
    } Named[] =
    {
        { L"&amp;", L'&' }, { L"&lt;", L'<' }, { L"&gt;", L'>' },
        { L"&quot;", L'"' }, { L"&apos;", L'\'' }
    };
    LPCWSTR q;
    ULONG value = 0, digits = 0, base = 10;
    size_t i, len;

    for (i = 0; i < ARRAYSIZE(Named); i++)
    {
        len = wcslen(Named[i].Name);
        if ((size_t)(end - p) >= len && wcsncmp(p, Named[i].Name, len) == 0)
        {
            *c = Named[i].Char;
            return len;
        }
    }

    /* &#ddd; or &#xhhh; */
    if (end - p < 4 || p[1] != L'#')
        return 0;
    q = p + 2;
    if (*q == L'x')
    {
        base = 16;
        q++;
    }
    for (; q < end && *q != L';'; q++, digits++)
    {
        ULONG d;

        if (*q >= L'0' && *q <= L'9')
            d = *q - L'0';
        else if (base == 16 && *q >= L'a' && *q <= L'f')
            d = *q - L'a' + 10;
        else if (base == 16 && *q >= L'A' && *q <= L'F')
            d = *q - L'A' + 10;
        else
            return 0;
        value = value * base + d;
        if (value > 0x10FFFF)
            return 0;
    }
    if (q >= end || digits == 0 || value == 0)
        return 0;
    *c = value;
    return (size_t)(q + 1 - p);
}

/*
 * Extract the text content of the first <tag>...</tag> into out (NUL-terminated,
 * up to outChars-1 chars), with its entity references replaced by the
 * characters they stand for.  Returns TRUE on success.  Leading/trailing ASCII
 * whitespace is trimmed.
 */
static BOOL
XmlGetText(LPCWSTR xml, LPCWSTR tag, LPWSTR out, size_t outChars)
{
    LPCWSTR start = XmlFindOpen(xml, tag);
    LPCWSTR end;
    size_t n;

    if (start == NULL)
        return FALSE;

    while (*start == L' ' || *start == L'\t' || *start == L'\r' || *start == L'\n')
        start++;

    end = wcschr(start, L'<');
    if (end == NULL)
        return FALSE;

    n = (size_t)(end - start);
    while (n > 0 && (start[n - 1] == L' ' || start[n - 1] == L'\t' ||
                     start[n - 1] == L'\r' || start[n - 1] == L'\n'))
        n--;

    end = start + n;
    n = 0;
    while (start < end && n < outChars - 1)
    {
        ULONG c;
        size_t len = (*start == L'&') ? XmlDecodeEntity(start, end, &c) : 0;

        if (len == 0)
        {
            out[n++] = *start++;
            continue;
        }
        start += len;
        if (c > 0xFFFF)
        {
            if (n + 2 > outChars - 1)
                break;
            c -= 0x10000;
            out[n++] = (WCHAR)(0xD800 + (c >> 10));
            out[n++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        }
        else
        {
            out[n++] = (WCHAR)c;
        }
    }
    out[n] = L'\0';
    return TRUE;
}

/* Convert a UTF-16 SSID name into a DOT11_SSID (octet copy of the low byte). */
static BOOL
SsidFromText(LPCWSTR text, PDOT11_SSID ssid)
{
    size_t len = wcslen(text);
    size_t i;

    if (len == 0 || len > DOT11_SSID_MAX_LENGTH)
        return FALSE;

    ssid->uSSIDLength = (ULONG)len;
    for (i = 0; i < len; i++)
        ssid->ucSSID[i] = (UCHAR)text[i];
    return TRUE;
}

VOID
WlanSvcFreeProfile(PWLANSVC_PROFILE Profile)
{
    if (Profile == NULL)
        return;
    if (Profile->Xml != NULL)
        HeapFree(GetProcessHeap(), 0, Profile->Xml);
    HeapFree(GetProcessHeap(), 0, Profile);
}

/*
 * Parse profile XML into a freshly-allocated WLANSVC_PROFILE.  Caller owns the
 * result (free with WlanSvcFreeProfile).  The original XML is duplicated and
 * stored verbatim for WlanGetProfile.
 */
DWORD
WlanSvcParseProfileXml(LPCWSTR Xml, PWLANSVC_PROFILE *ppProfile)
{
    PWLANSVC_PROFILE prof;
    WCHAR buf[512];
    size_t xmlLen;

    *ppProfile = NULL;
    if (Xml == NULL)
        return ERROR_INVALID_PARAMETER;

    prof = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*prof));
    if (prof == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    /* Profile <name> (the top-level one, used as the store key). */
    if (XmlGetText(Xml, L"name", buf, ARRAYSIZE(buf)))
        wcsncpy(prof->Name, buf, WLAN_MAX_NAME_LENGTH - 1);

    /* The SSID's own <name> lives under <SSIDConfig><SSID>; search from there
     * so the profile-level <name> is not matched again. */
    {
        LPCWSTR ssidScope = wcsstr(Xml, L"<SSID>");
        if (ssidScope == NULL)
            ssidScope = wcsstr(Xml, L"<SSIDConfig");
        if (ssidScope != NULL && XmlGetText(ssidScope, L"name", buf, ARRAYSIZE(buf)))
            SsidFromText(buf, &prof->Ssid);
    }

    /* If no SSID element, fall back to the profile name as the SSID. */
    if (prof->Ssid.uSSIDLength == 0 && prof->Name[0] != L'\0')
        SsidFromText(prof->Name, &prof->Ssid);

    if (prof->Name[0] == L'\0' && prof->Ssid.uSSIDLength != 0)
    {
        ULONG i;
        for (i = 0; i < prof->Ssid.uSSIDLength; i++)
            prof->Name[i] = (WCHAR)prof->Ssid.ucSSID[i];
        prof->Name[prof->Ssid.uSSIDLength] = L'\0';
    }

    if (prof->Name[0] == L'\0' || prof->Ssid.uSSIDLength == 0)
    {
        WlanSvcFreeProfile(prof);
        return ERROR_BAD_PROFILE;
    }

    /* connectionType (ESS=infrastructure, IBSS=adhoc). */
    prof->BssType = dot11_BSS_type_infrastructure;
    if (XmlGetText(Xml, L"connectionType", buf, ARRAYSIZE(buf)))
    {
        if (_wcsicmp(buf, L"IBSS") == 0)
            prof->BssType = dot11_BSS_type_independent;
    }

    prof->AutoConnect = TRUE;
    if (XmlGetText(Xml, L"connectionMode", buf, ARRAYSIZE(buf)))
    {
        if (_wcsicmp(buf, L"manual") == 0)
            prof->AutoConnect = FALSE;
    }

    prof->Auth = DOT11_AUTH_ALGO_80211_OPEN;
    prof->Cipher = DOT11_CIPHER_ALGO_NONE;
    prof->SecurityEnabled = FALSE;

    if (XmlGetText(Xml, L"authentication", buf, ARRAYSIZE(buf)))
    {
        if (_wcsicmp(buf, L"WPA2PSK") == 0 || _wcsicmp(buf, L"RSNAPSK") == 0)
            prof->Auth = DOT11_AUTH_ALGO_RSNA_PSK;
        else if (_wcsicmp(buf, L"WPAPSK") == 0)
            prof->Auth = DOT11_AUTH_ALGO_WPA_PSK;
        else if (_wcsicmp(buf, L"WPA3SAE") == 0)
            prof->Auth = DOT11_AUTH_ALGO_WPA3_SAE;
        else if (_wcsicmp(buf, L"WPA2") == 0)
            prof->Auth = DOT11_AUTH_ALGO_RSNA;
        else if (_wcsicmp(buf, L"WPA") == 0)
            prof->Auth = DOT11_AUTH_ALGO_WPA;
        else if (_wcsicmp(buf, L"shared") == 0)
            prof->Auth = DOT11_AUTH_ALGO_80211_SHARED_KEY;
        else /* "open" */
            prof->Auth = DOT11_AUTH_ALGO_80211_OPEN;
    }

    if (XmlGetText(Xml, L"encryption", buf, ARRAYSIZE(buf)))
    {
        if (_wcsicmp(buf, L"AES") == 0)
            prof->Cipher = DOT11_CIPHER_ALGO_CCMP;
        else if (_wcsicmp(buf, L"TKIP") == 0)
            prof->Cipher = DOT11_CIPHER_ALGO_TKIP;
        else if (_wcsicmp(buf, L"WEP") == 0)
            prof->Cipher = DOT11_CIPHER_ALGO_WEP;
        else
            prof->Cipher = DOT11_CIPHER_ALGO_NONE;
    }

    prof->SecurityEnabled = (prof->Auth != DOT11_AUTH_ALGO_80211_OPEN);

    /* keyMaterial (passphrase / PSK), carried opaquely for the supplicant. */
    if (XmlGetText(Xml, L"keyMaterial", buf, ARRAYSIZE(buf)))
    {
        size_t klen = wcslen(buf);
        size_t i;
        if (klen > NWIFI_MAX_PSK_BYTES)
            klen = NWIFI_MAX_PSK_BYTES;
        for (i = 0; i < klen; i++)
            prof->Key[i] = (UCHAR)buf[i];
        prof->KeyLength = (ULONG)klen;
        SecureZeroMemory(buf, sizeof(buf));
    }

    /* Duplicate the source XML for later retrieval. */
    xmlLen = (wcslen(Xml) + 1) * sizeof(WCHAR);
    prof->Xml = HeapAlloc(GetProcessHeap(), 0, xmlLen);
    if (prof->Xml != NULL)
        memcpy(prof->Xml, Xml, xmlLen);

    *ppProfile = prof;
    return ERROR_SUCCESS;
}

/* Lock held by caller.  Profile names are case-sensitive. */
PWLANSVC_PROFILE
WlanSvcFindProfile(PWLANSVC_INTERFACE Iface, LPCWSTR Name)
{
    PLIST_ENTRY entry;

    if (Name == NULL)
        return NULL;

    for (entry = Iface->ProfileListHead.Flink;
         entry != &Iface->ProfileListHead;
         entry = entry->Flink)
    {
        PWLANSVC_PROFILE prof =
            CONTAINING_RECORD(entry, WLANSVC_PROFILE, ListEntry);
        if (wcscmp(prof->Name, Name) == 0)
            return prof;
    }
    return NULL;
}

DWORD
WlanSvcSetProfile(PWLANSVC_INTERFACE Iface,
                  DWORD dwFlags,
                  LPCWSTR Xml,
                  BOOL bOverwrite,
                  PDWORD pdwReasonCode)
{
    PWLANSVC_PROFILE prof, existing;
    DWORD dwResult;

    if (pdwReasonCode)
        *pdwReasonCode = WLAN_REASON_CODE_SUCCESS;

    dwResult = WlanSvcParseProfileXml(Xml, &prof);
    if (dwResult != ERROR_SUCCESS)
    {
        if (pdwReasonCode)
            *pdwReasonCode = dwResult; /* coarse reason code */
        return dwResult;
    }

    prof->Flags = dwFlags;

    existing = WlanSvcFindProfile(Iface, prof->Name);
    if (existing != NULL)
    {
        if (!bOverwrite)
        {
            WlanSvcFreeProfile(prof);
            return ERROR_ALREADY_EXISTS;
        }

        /* An overwritten profile keeps its place in the preference list:
         * the new record takes the old one's slot. */
        WlanSvcAcmForgetAttempt(Iface, existing->Name);
        /* ... and its file. */
        prof->FileGuid = existing->FileGuid;
        InsertTailList(&existing->ListEntry, &prof->ListEntry);
        RemoveEntryList(&existing->ListEntry);
        WlanSvcFreeProfile(existing);
    }
    else
    {
        /* A new profile goes to the top of the list, below the group policy
         * profiles, which always come first. */
        PLIST_ENTRY entry = Iface->ProfileListHead.Flink;

        while (entry != &Iface->ProfileListHead &&
               (CONTAINING_RECORD(entry, WLANSVC_PROFILE, ListEntry)->Flags &
                WLAN_PROFILE_GROUP_POLICY))
        {
            entry = entry->Flink;
        }
        InsertTailList(entry, &prof->ListEntry);
        Iface->ProfileCount++;
    }

    WlanSvcStoreSaveProfile(Iface, prof);
    WlanSvcStoreSaveOrder(Iface);

    WlanSvcIndicateAcm(Iface, wlan_notification_acm_profile_change);
    return ERROR_SUCCESS;
}

/* Blank the <keyMaterial> text in a mutable XML copy (-> <keyMaterial></keyMaterial>). */
static VOID
WlanSvcRedactKeyMaterial(LPWSTR Xml)
{
    LPWSTR start = (LPWSTR)XmlFindOpen(Xml, L"keyMaterial");
    LPWSTR end;
    size_t tail;

    if (start == NULL)
        return;

    end = wcschr(start, L'<');
    if (end == NULL || end <= start)
        return;

    tail = wcslen(end) + 1;
    memmove(start, end, tail * sizeof(WCHAR));
}

DWORD
WlanSvcGetProfile(PWLANSVC_INTERFACE Iface,
                  LPCWSTR Name,
                  BOOL bPlaintextKey,
                  LPWSTR *ppXml,
                  PDWORD pdwFlags)
{
    PWLANSVC_PROFILE prof;
    size_t len;
    LPWSTR copy;

    *ppXml = NULL;

    prof = WlanSvcFindProfile(Iface, Name);
    if (prof == NULL)
        return ERROR_NOT_FOUND;

    if (prof->Xml == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    len = (wcslen(prof->Xml) + 1) * sizeof(WCHAR);
    copy = midl_user_allocate(len);
    if (copy == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(copy, prof->Xml, len);

    /* Never hand the cleartext PSK to an unprivileged caller. */
    if (!bPlaintextKey)
        WlanSvcRedactKeyMaterial(copy);

    *ppXml = copy;
    if (pdwFlags)
        *pdwFlags = prof->Flags;
    return ERROR_SUCCESS;
}

DWORD
WlanSvcDeleteProfile(PWLANSVC_INTERFACE Iface, LPCWSTR Name)
{
    PWLANSVC_PROFILE prof = WlanSvcFindProfile(Iface, Name);

    if (prof == NULL)
        return ERROR_NOT_FOUND;

    WlanSvcAcmForgetAttempt(Iface, prof->Name);
    WlanSvcStoreDeleteProfile(Iface, prof);
    RemoveEntryList(&prof->ListEntry);
    Iface->ProfileCount--;
    WlanSvcFreeProfile(prof);
    WlanSvcStoreSaveOrder(Iface);

    WlanSvcIndicateAcm(Iface, wlan_notification_acm_profile_change);
    return ERROR_SUCCESS;
}

/* The number of group policy profiles: they lead the list, in a fixed order. */
static ULONG
WlanSvcCountPolicyProfiles(PWLANSVC_INTERFACE Iface)
{
    PLIST_ENTRY entry;
    ULONG count = 0;

    for (entry = Iface->ProfileListHead.Flink;
         entry != &Iface->ProfileListHead;
         entry = entry->Flink)
    {
        if (!(CONTAINING_RECORD(entry, WLANSVC_PROFILE, ListEntry)->Flags &
              WLAN_PROFILE_GROUP_POLICY))
            break;
        count++;
    }
    return count;
}

DWORD
WlanSvcSetProfilePosition(PWLANSVC_INTERFACE Iface, LPCWSTR Name,
                          DWORD Position)
{
    PWLANSVC_PROFILE prof = WlanSvcFindProfile(Iface, Name);
    PLIST_ENTRY entry;
    ULONG first, index;

    if (prof == NULL)
        return ERROR_NOT_FOUND;
    /* The position of a group policy profile cannot be changed... */
    if (prof->Flags & WLAN_PROFILE_GROUP_POLICY)
        return ERROR_ACCESS_DENIED;

    /* ... nor can another profile go ahead of them. */
    first = WlanSvcCountPolicyProfiles(Iface);
    if (Position < first)
        Position = first;

    RemoveEntryList(&prof->ListEntry);
    for (entry = Iface->ProfileListHead.Flink, index = 0;
         entry != &Iface->ProfileListHead && index < Position;
         entry = entry->Flink, index++)
    {
        /* Count. */
    }
    /* In front of the profile now at Position, or last. */
    InsertTailList(entry, &prof->ListEntry);

    WlanSvcStoreSaveOrder(Iface);
    WlanSvcIndicateAcm(Iface, wlan_notification_acm_profile_change);
    return ERROR_SUCCESS;
}

DWORD
WlanSvcSetProfileList(PWLANSVC_INTERFACE Iface, DWORD Count, LPCWSTR *Names)
{
    PWLANSVC_PROFILE *order;
    DWORD dwResult = ERROR_SUCCESS;
    DWORD i, j;

    order = HeapAlloc(GetProcessHeap(), 0, (Count ? Count : 1) * sizeof(*order));
    if (order == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    for (i = 0; i < Count && dwResult == ERROR_SUCCESS; i++)
    {
        if (Names[i] == NULL)
        {
            dwResult = ERROR_INVALID_PARAMETER;
            break;
        }
        order[i] = WlanSvcFindProfile(Iface, Names[i]);
        if (order[i] == NULL)
        {
            dwResult = ERROR_NOT_FOUND;
            break;
        }
        /* The same profile twice. */
        for (j = 0; j < i; j++)
        {
            if (order[j] == order[i])
                dwResult = ERROR_INVALID_PARAMETER;
        }
    }

    /* The list names every profile, each once. */
    if (dwResult == ERROR_SUCCESS && Count != Iface->ProfileCount)
        dwResult = ERROR_INVALID_PARAMETER;

    if (dwResult == ERROR_SUCCESS)
    {
        /* Group policy profiles keep their places ahead of the others,
         * whose order the list sets. */
        for (i = 0; i < Count; i++)
        {
            if (!(order[i]->Flags & WLAN_PROFILE_GROUP_POLICY))
            {
                RemoveEntryList(&order[i]->ListEntry);
                InsertTailList(&Iface->ProfileListHead, &order[i]->ListEntry);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, order);
    if (dwResult != ERROR_SUCCESS)
        return dwResult;

    WlanSvcStoreSaveOrder(Iface);
    WlanSvcIndicateAcm(Iface, wlan_notification_acm_profile_change);
    return ERROR_SUCCESS;
}

/* The XML-escaped form of one character, or NULL if it stands as it is. */
static LPCWSTR
WlanSvcXmlEntity(WCHAR c)
{
    switch (c)
    {
        case L'&':  return L"&amp;";
        case L'<':  return L"&lt;";
        case L'>':  return L"&gt;";
        case L'"':  return L"&quot;";
        case L'\'': return L"&apos;";
        default:    return NULL;
    }
}

/* Xml with the text of its profile <name> (the first one) replaced by Name,
 * escaped; heap block, caller frees. */
static LPWSTR
WlanSvcXmlWithName(LPCWSTR Xml, LPCWSTR Name)
{
    LPCWSTR start = XmlFindOpen(Xml, L"name");
    LPCWSTR end, p, entity;
    SIZE_T head, tail, nameLen = 0;
    LPWSTR out, q;

    if (start == NULL)
        return NULL;
    end = wcschr(start, L'<');
    if (end == NULL)
        return NULL;

    for (p = Name; *p != UNICODE_NULL; p++)
    {
        entity = WlanSvcXmlEntity(*p);
        nameLen += (entity != NULL) ? wcslen(entity) : 1;
    }

    head = start - Xml;
    tail = wcslen(end);
    out = HeapAlloc(GetProcessHeap(), 0, (head + nameLen + tail + 1) * sizeof(WCHAR));
    if (out == NULL)
        return NULL;

    memcpy(out, Xml, head * sizeof(WCHAR));
    q = out + head;
    for (p = Name; *p != UNICODE_NULL; p++)
    {
        entity = WlanSvcXmlEntity(*p);
        if (entity != NULL)
        {
            wcscpy(q, entity);
            q += wcslen(entity);
        }
        else
        {
            *q++ = *p;
        }
    }
    memcpy(q, end, (tail + 1) * sizeof(WCHAR));
    return out;
}

DWORD
WlanSvcRenameProfile(PWLANSVC_INTERFACE Iface, LPCWSTR OldName, LPCWSTR NewName)
{
    PWLANSVC_PROFILE prof = WlanSvcFindProfile(Iface, OldName);
    PWLANSVC_PROFILE other;
    LPWSTR xml;
    size_t len;

    if (prof == NULL)
        return ERROR_NOT_FOUND;

    len = wcslen(NewName);
    if (len == 0 || len >= WLAN_MAX_NAME_LENGTH)
        return ERROR_INVALID_PARAMETER;

    other = WlanSvcFindProfile(Iface, NewName);
    if (other != NULL && other != prof)
        return ERROR_ALREADY_EXISTS;

    if (prof->Xml == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    xml = WlanSvcXmlWithName(prof->Xml, NewName);
    if (xml == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    /* The profile keeps its place, its file and its connection. */
    if (wcscmp(Iface->ConnectedProfile, prof->Name) == 0)
        wcscpy(Iface->ConnectedProfile, NewName);
    if (wcscmp(Iface->AttemptProfile, prof->Name) == 0)
        wcscpy(Iface->AttemptProfile, NewName);

    SecureZeroMemory(prof->Xml, wcslen(prof->Xml) * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, prof->Xml);
    prof->Xml = xml;
    wcscpy(prof->Name, NewName);

    WlanSvcStoreSaveProfile(Iface, prof);
    WlanSvcIndicateAcm(Iface, wlan_notification_acm_profile_change);
    return ERROR_SUCCESS;
}

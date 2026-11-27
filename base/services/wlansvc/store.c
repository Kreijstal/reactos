/*
 * PROJECT:     ReactOS WLAN Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        base/services/wlansvc/store.c
 * PURPOSE:     Keep the profiles of each interface on disk
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * Like the Windows WLAN service, every profile is one XML file,
 *     <ProgramData>\Microsoft\Wlansvc\Profiles\Interfaces\{interface}\{profile}.xml
 * written in UTF-8 with its key material protected: <keyMaterial> holds the
 * CryptProtectData blob (machine scope) in hex and <protected> says true.
 * The preference order is kept beside them in ProfileOrder, one profile
 * file GUID per line.  Files, not the registry, so a box whose registry is
 * not written back (a /MININT boot) keeps its networks as well.
 *
 * In memory a profile keeps the plaintext form of its XML (WlanGetProfile
 * hands it out, redacted unless the caller may see the key); only the
 * files carry the protected form.
 *
 * All routines run with WlanSvcLock held by the caller.  A failure to write
 * is reported and otherwise ignored: the profile stays usable for this run.
 */

#include "precomp.h"
#include <winreg.h>
#include <winnls.h>
#include <wincrypt.h>

#define NDEBUG
#include <debug.h>

#define STORE_SUBDIR    L"\\Microsoft\\Wlansvc\\Profiles\\Interfaces\\"
#define STORE_ORDER     L"ProfileOrder"
#define STORE_ENTROPY   "WlanSvc profile key"

static const GUID StoreNoGuid = { 0 };

static VOID
StoreGuidToString(const GUID *Guid, PWCHAR Buffer)
{
    /* 38 characters and the NUL. */
    _snwprintf(Buffer, 39,
             L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             Guid->Data1, Guid->Data2, Guid->Data3,
             Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
             Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

/* "{...}" or "{...}.xml" -> GUID. */
static BOOL
StoreStringToGuid(PCWSTR Text, GUID *Guid)
{
    WCHAR Inner[37];

    if (Text[0] != L'{' || wcslen(Text) < 38 || Text[37] != L'}')
        return FALSE;
    memcpy(Inner, Text + 1, 36 * sizeof(WCHAR));
    Inner[36] = UNICODE_NULL;
    return UuidFromStringW((RPC_WSTR)Inner, Guid) == RPC_S_OK;
}

/* Where ProgramData is: the ProfileList value, as the shell finds it. */
static BOOL
StoreGetProgramData(PWCHAR Path, DWORD Count)
{
    WCHAR Raw[MAX_PATH];
    DWORD Size = sizeof(Raw), Type, Length;
    HKEY Key;
    BOOL Found = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                      0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(Key, L"ProgramData", NULL, &Type, (LPBYTE)Raw,
                             &Size) == ERROR_SUCCESS &&
            (Type == REG_SZ || Type == REG_EXPAND_SZ) &&
            Size >= sizeof(WCHAR))
        {
            Raw[min(Size / sizeof(WCHAR), ARRAYSIZE(Raw) - 1)] = UNICODE_NULL;
            Found = TRUE;
        }
        RegCloseKey(Key);
    }
    if (!Found)
        wcscpy(Raw, L"%ProgramData%");

    Length = ExpandEnvironmentStringsW(Raw, Path, Count);
    return Length != 0 && Length <= Count && wcschr(Path, L'%') == NULL;
}

/* The interface's profile directory, created on the way if Create. */
static BOOL
StoreGetInterfaceDir(PWLANSVC_INTERFACE Iface, PWCHAR Path, SIZE_T Count,
                     BOOL Create)
{
    WCHAR GuidText[39];
    PWCHAR p;

    if (!StoreGetProgramData(Path, (DWORD)Count))
        return FALSE;
    StoreGuidToString(&Iface->InterfaceGuid, GuidText);
    if (wcslen(Path) + wcslen(STORE_SUBDIR) + wcslen(GuidText) + 45 >= Count)
        return FALSE;
    wcscat(Path, STORE_SUBDIR);
    wcscat(Path, GuidText);

    if (Create)
    {
        /* Every component past the drive. */
        for (p = wcschr(Path + 3, L'\\'); p != NULL; p = wcschr(p + 1, L'\\'))
        {
            *p = UNICODE_NULL;
            CreateDirectoryW(Path, NULL);
            *p = L'\\';
        }
        if (!CreateDirectoryW(Path, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            DPRINT1("WLANSVC: cannot create %S: %lu\n", Path, GetLastError());
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL
StoreProfilePath(PWLANSVC_INTERFACE Iface, const GUID *FileGuid, PWCHAR Path,
                 SIZE_T Count, BOOL Create)
{
    WCHAR GuidText[39];

    if (!StoreGetInterfaceDir(Iface, Path, Count, Create))
        return FALSE;
    StoreGuidToString(FileGuid, GuidText);
    wcscat(Path, L"\\");
    wcscat(Path, GuidText);
    wcscat(Path, L".xml");
    return TRUE;
}

/* Write Data to Path through a temporary file, so a crash leaves the old
 * file or the new one, never half of one. */
static BOOL
StoreWriteFile(PCWSTR Path, const VOID *Data, DWORD Size)
{
    WCHAR Temp[MAX_PATH + 8];
    HANDLE File;
    DWORD Written = 0;
    BOOL Ok;

    if (wcslen(Path) + 5 >= ARRAYSIZE(Temp))
        return FALSE;
    wcscpy(Temp, Path);
    wcscat(Temp, L".new");

    File = CreateFileW(Temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Data, Size, &Written, NULL) && Written == Size &&
         FlushFileBuffers(File);
    CloseHandle(File);

    if (Ok)
        Ok = MoveFileExW(Temp, Path, MOVEFILE_REPLACE_EXISTING |
                                     MOVEFILE_WRITE_THROUGH);
    if (!Ok)
    {
        DPRINT1("WLANSVC: cannot write %S: %lu\n", Path, GetLastError());
        DeleteFileW(Temp);
    }
    return Ok;
}

/* The whole file, NUL-terminated, in a heap block the caller frees. */
static PCHAR
StoreReadFile(PCWSTR Path, PDWORD Size)
{
    HANDLE File;
    DWORD Length, Read = 0;
    PCHAR Data;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return NULL;

    Length = GetFileSize(File, NULL);
    if (Length == INVALID_FILE_SIZE || Length > 256 * 1024)
    {
        CloseHandle(File);
        return NULL;
    }

    Data = HeapAlloc(GetProcessHeap(), 0, Length + 1);
    if (Data != NULL &&
        (!ReadFile(File, Data, Length, &Read, NULL) || Read != Length))
    {
        HeapFree(GetProcessHeap(), 0, Data);
        Data = NULL;
    }
    CloseHandle(File);

    if (Data != NULL)
    {
        Data[Length] = '\0';
        *Size = Length;
    }
    return Data;
}

/* The span of an element's text, by plain tag (the profile schema uses no
 * attributes on these). */
static BOOL
StoreFindText(PCWSTR Xml, PCWSTR Open, PCWSTR Close, PCWSTR *Start,
              PCWSTR *End)
{
    PCWSTR s = wcsstr(Xml, Open);
    PCWSTR e;

    if (s == NULL)
        return FALSE;
    s += wcslen(Open);
    e = wcsstr(s, Close);
    if (e == NULL)
        return FALSE;
    *Start = s;
    *End = e;
    return TRUE;
}

/* Xml with [Start, End) replaced by Text; heap block, caller frees. */
static LPWSTR
StoreSplice(PCWSTR Xml, PCWSTR Start, PCWSTR End, PCWSTR Text)
{
    SIZE_T Head = Start - Xml, TextLen = wcslen(Text), Tail = wcslen(End);
    LPWSTR Out;

    Out = HeapAlloc(GetProcessHeap(), 0,
                    (Head + TextLen + Tail + 1) * sizeof(WCHAR));
    if (Out == NULL)
        return NULL;
    memcpy(Out, Xml, Head * sizeof(WCHAR));
    memcpy(Out + Head, Text, TextLen * sizeof(WCHAR));
    memcpy(Out + Head + TextLen, End, (Tail + 1) * sizeof(WCHAR));
    return Out;
}

static VOID
StoreFreeSecret(LPWSTR Xml)
{
    if (Xml != NULL)
    {
        SecureZeroMemory(Xml, wcslen(Xml) * sizeof(WCHAR));
        HeapFree(GetProcessHeap(), 0, Xml);
    }
}

/* Set <protected> to Value, adding the element in front of <keyMaterial>
 * if the profile has none.  Consumes Xml. */
static LPWSTR
StoreSetProtected(LPWSTR Xml, PCWSTR Value)
{
    PCWSTR s, e;
    LPWSTR Out;

    if (StoreFindText(Xml, L"<protected>", L"</protected>", &s, &e))
    {
        Out = StoreSplice(Xml, s, e, Value);
    }
    else
    {
        WCHAR Element[40];

        s = wcsstr(Xml, L"<keyMaterial>");
        if (s == NULL)
            return Xml;
        _snwprintf(Element, ARRAYSIZE(Element), L"<protected>%s</protected>",
                   Value);
        Element[ARRAYSIZE(Element) - 1] = UNICODE_NULL;
        Out = StoreSplice(Xml, s, s, Element);
    }
    StoreFreeSecret(Xml);
    return Out;
}

static BOOL
StoreIsProtected(PCWSTR Xml)
{
    PCWSTR s, e;

    return StoreFindText(Xml, L"<protected>", L"</protected>", &s, &e) &&
           e - s == 4 && _wcsnicmp(s, L"true", 4) == 0;
}

/* The on-disk form of a plaintext profile: key material protected. */
static LPWSTR
StoreProtectXml(PCWSTR Xml)
{
    DATA_BLOB In, Out, Entropy;
    PCWSTR s, e;
    LPWSTR Hex, Result;
    DWORD i;

    if (!StoreFindText(Xml, L"<keyMaterial>", L"</keyMaterial>", &s, &e) ||
        StoreIsProtected(Xml))
    {
        Result = HeapAlloc(GetProcessHeap(), 0, (wcslen(Xml) + 1) * sizeof(WCHAR));
        if (Result != NULL)
            wcscpy(Result, Xml);
        return Result;
    }

    In.pbData = (BYTE *)s;
    In.cbData = (DWORD)((e - s) * sizeof(WCHAR));
    Entropy.pbData = (BYTE *)STORE_ENTROPY;
    Entropy.cbData = sizeof(STORE_ENTROPY) - 1;
    if (!CryptProtectData(&In, L"WLAN profile key", &Entropy, NULL, NULL,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                          &Out))
    {
        DPRINT1("WLANSVC: CryptProtectData failed: %lu\n", GetLastError());
        return NULL;
    }

    Hex = HeapAlloc(GetProcessHeap(), 0, (Out.cbData * 2 + 1) * sizeof(WCHAR));
    if (Hex == NULL)
    {
        LocalFree(Out.pbData);
        return NULL;
    }
    for (i = 0; i < Out.cbData; i++)
    {
        static const WCHAR Digits[] = L"0123456789ABCDEF";
        Hex[i * 2] = Digits[Out.pbData[i] >> 4];
        Hex[i * 2 + 1] = Digits[Out.pbData[i] & 15];
    }
    Hex[Out.cbData * 2] = UNICODE_NULL;
    LocalFree(Out.pbData);

    Result = StoreSplice(Xml, s, e, Hex);
    HeapFree(GetProcessHeap(), 0, Hex);
    if (Result == NULL)
        return NULL;
    return StoreSetProtected(Result, L"true");
}

static INT
StoreHexDigit(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* The in-memory form of an on-disk profile: key material in plaintext. */
static LPWSTR
StoreUnprotectXml(PCWSTR Xml)
{
    DATA_BLOB In, Out, Entropy;
    PCWSTR s, e;
    LPWSTR Plain, Result;
    DWORD i, Length;

    if (!StoreIsProtected(Xml) ||
        !StoreFindText(Xml, L"<keyMaterial>", L"</keyMaterial>", &s, &e))
    {
        Result = HeapAlloc(GetProcessHeap(), 0, (wcslen(Xml) + 1) * sizeof(WCHAR));
        if (Result != NULL)
            wcscpy(Result, Xml);
        return Result;
    }

    Length = (DWORD)(e - s);
    if (Length == 0 || (Length & 1))
        return NULL;
    In.cbData = Length / 2;
    In.pbData = HeapAlloc(GetProcessHeap(), 0, In.cbData);
    if (In.pbData == NULL)
        return NULL;
    for (i = 0; i < In.cbData; i++)
    {
        INT Hi = StoreHexDigit(s[i * 2]), Lo = StoreHexDigit(s[i * 2 + 1]);
        if (Hi < 0 || Lo < 0)
        {
            HeapFree(GetProcessHeap(), 0, In.pbData);
            return NULL;
        }
        In.pbData[i] = (BYTE)(Hi << 4 | Lo);
    }

    Entropy.pbData = (BYTE *)STORE_ENTROPY;
    Entropy.cbData = sizeof(STORE_ENTROPY) - 1;
    if (!CryptUnprotectData(&In, NULL, &Entropy, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &Out))
    {
        DPRINT1("WLANSVC: CryptUnprotectData failed: %lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, In.pbData);
        return NULL;
    }
    HeapFree(GetProcessHeap(), 0, In.pbData);

    Plain = HeapAlloc(GetProcessHeap(), 0, Out.cbData + sizeof(WCHAR));
    if (Plain == NULL)
    {
        SecureZeroMemory(Out.pbData, Out.cbData);
        LocalFree(Out.pbData);
        return NULL;
    }
    memcpy(Plain, Out.pbData, Out.cbData);
    Plain[Out.cbData / sizeof(WCHAR)] = UNICODE_NULL;
    SecureZeroMemory(Out.pbData, Out.cbData);
    LocalFree(Out.pbData);

    Result = StoreSplice(Xml, s, e, Plain);
    StoreFreeSecret(Plain);
    if (Result == NULL)
        return NULL;
    return StoreSetProtected(Result, L"false");
}

VOID
WlanSvcStoreSaveProfile(PWLANSVC_INTERFACE Iface, PWLANSVC_PROFILE Profile)
{
    WCHAR Path[MAX_PATH];
    LPWSTR Disk;
    PCHAR Utf8;
    INT Size;

    if (Profile->Xml == NULL)
        return;
    if (IsEqualGUID(&Profile->FileGuid, &StoreNoGuid) &&
        UuidCreate(&Profile->FileGuid) != RPC_S_OK)
        return;
    if (!StoreProfilePath(Iface, &Profile->FileGuid, Path, ARRAYSIZE(Path), TRUE))
        return;

    Disk = StoreProtectXml(Profile->Xml);
    if (Disk == NULL)
    {
        DPRINT1("WLANSVC: profile '%S' not saved\n", Profile->Name);
        return;
    }

    Size = WideCharToMultiByte(CP_UTF8, 0, Disk, -1, NULL, 0, NULL, NULL);
    Utf8 = (Size > 0) ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
    if (Utf8 != NULL)
    {
        WideCharToMultiByte(CP_UTF8, 0, Disk, -1, Utf8, Size, NULL, NULL);
        StoreWriteFile(Path, Utf8, (DWORD)(Size - 1));
        HeapFree(GetProcessHeap(), 0, Utf8);
    }
    StoreFreeSecret(Disk);
}

VOID
WlanSvcStoreDeleteProfile(PWLANSVC_INTERFACE Iface, PWLANSVC_PROFILE Profile)
{
    WCHAR Path[MAX_PATH];

    if (IsEqualGUID(&Profile->FileGuid, &StoreNoGuid))
        return;
    if (StoreProfilePath(Iface, &Profile->FileGuid, Path, ARRAYSIZE(Path), FALSE) &&
        !DeleteFileW(Path) && GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        DPRINT1("WLANSVC: cannot delete %S: %lu\n", Path, GetLastError());
    }
}

VOID
WlanSvcStoreSaveOrder(PWLANSVC_INTERFACE Iface)
{
    WCHAR Path[MAX_PATH], GuidText[39];
    CHAR *Text, *p;
    PLIST_ENTRY entry;
    ULONG i;

    if (!StoreGetInterfaceDir(Iface, Path, ARRAYSIZE(Path) - 16, TRUE))
        return;
    wcscat(Path, L"\\" STORE_ORDER);

    /* 38 characters and CR LF per profile. */
    Text = HeapAlloc(GetProcessHeap(), 0, (Iface->ProfileCount + 1) * 40);
    if (Text == NULL)
        return;
    p = Text;
    for (entry = Iface->ProfileListHead.Flink;
         entry != &Iface->ProfileListHead;
         entry = entry->Flink)
    {
        PWLANSVC_PROFILE prof = CONTAINING_RECORD(entry, WLANSVC_PROFILE, ListEntry);

        if (IsEqualGUID(&prof->FileGuid, &StoreNoGuid))
            continue;
        StoreGuidToString(&prof->FileGuid, GuidText);
        for (i = 0; i < 38; i++)
            *p++ = (CHAR)GuidText[i];
        *p++ = '\r';
        *p++ = '\n';
    }
    StoreWriteFile(Path, Text, (DWORD)(p - Text));
    HeapFree(GetProcessHeap(), 0, Text);
}

/* Read one profile file and append it to the interface's list. */
static VOID
StoreLoadOne(PWLANSVC_INTERFACE Iface, PCWSTR Dir, const GUID *FileGuid)
{
    WCHAR Path[MAX_PATH], GuidText[39];
    PWLANSVC_PROFILE prof;
    PLIST_ENTRY entry;
    LPWSTR Wide, Plain;
    PCHAR Utf8;
    DWORD Size;
    INT Chars;

    /* Already loaded through the order file. */
    for (entry = Iface->ProfileListHead.Flink;
         entry != &Iface->ProfileListHead;
         entry = entry->Flink)
    {
        if (IsEqualGUID(&CONTAINING_RECORD(entry, WLANSVC_PROFILE,
                                           ListEntry)->FileGuid, FileGuid))
            return;
    }

    StoreGuidToString(FileGuid, GuidText);
    if (wcslen(Dir) + 45 >= ARRAYSIZE(Path))
        return;
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s.xml", Dir, GuidText);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;

    Utf8 = StoreReadFile(Path, &Size);
    if (Utf8 == NULL)
        return;
    Chars = MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, NULL, 0);
    Wide = (Chars > 0) ? HeapAlloc(GetProcessHeap(), 0, Chars * sizeof(WCHAR)) : NULL;
    if (Wide != NULL)
        MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, Wide, Chars);
    HeapFree(GetProcessHeap(), 0, Utf8);
    if (Wide == NULL)
        return;

    Plain = StoreUnprotectXml(Wide);
    HeapFree(GetProcessHeap(), 0, Wide);
    if (Plain == NULL)
    {
        DPRINT1("WLANSVC: cannot read the key of %S\n", Path);
        return;
    }

    if (WlanSvcParseProfileXml(Plain, &prof) == ERROR_SUCCESS)
    {
        if (WlanSvcFindProfile(Iface, prof->Name) != NULL)
        {
            WlanSvcFreeProfile(prof);
        }
        else
        {
            prof->FileGuid = *FileGuid;
            InsertTailList(&Iface->ProfileListHead, &prof->ListEntry);
            Iface->ProfileCount++;
        }
    }
    StoreFreeSecret(Plain);
}

VOID
WlanSvcStoreLoadProfiles(PWLANSVC_INTERFACE Iface)
{
    WCHAR Dir[MAX_PATH], Pattern[MAX_PATH + 8];
    WIN32_FIND_DATAW Find;
    HANDLE Search;
    GUID FileGuid;
    PCHAR Order, Line;
    DWORD Size;

    if (!StoreGetInterfaceDir(Iface, Dir, ARRAYSIZE(Dir) - 16, FALSE))
        return;

    /* The saved order first. */
    _snwprintf(Pattern, ARRAYSIZE(Pattern), L"%s\\%s", Dir, STORE_ORDER);
    Pattern[ARRAYSIZE(Pattern) - 1] = UNICODE_NULL;
    Order = StoreReadFile(Pattern, &Size);
    if (Order != NULL)
    {
        for (Line = Order; *Line != '\0'; )
        {
            WCHAR Text[39];
            ULONG i;

            for (i = 0; i < 38 && Line[i] != '\0' && Line[i] != '\r' &&
                        Line[i] != '\n'; i++)
                Text[i] = (WCHAR)(UCHAR)Line[i];
            Text[i] = UNICODE_NULL;
            if (i == 38 && StoreStringToGuid(Text, &FileGuid))
                StoreLoadOne(Iface, Dir, &FileGuid);

            while (*Line != '\0' && *Line != '\n')
                Line++;
            if (*Line == '\n')
                Line++;
        }
        HeapFree(GetProcessHeap(), 0, Order);
    }

    /* Then any profile the order file does not name, at the end. */
    _snwprintf(Pattern, ARRAYSIZE(Pattern), L"%s\\*.xml", Dir);
    Pattern[ARRAYSIZE(Pattern) - 1] = UNICODE_NULL;
    Search = FindFirstFileW(Pattern, &Find);
    if (Search == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (wcslen(Find.cFileName) == 42 &&
            _wcsicmp(Find.cFileName + 38, L".xml") == 0 &&
            StoreStringToGuid(Find.cFileName, &FileGuid))
        {
            StoreLoadOne(Iface, Dir, &FileGuid);
        }
    } while (FindNextFileW(Search, &Find));
    FindClose(Search);

    DPRINT1("WLANSVC: %lu profiles loaded from %S\n", Iface->ProfileCount, Dir);
}

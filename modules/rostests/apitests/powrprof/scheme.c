/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the power scheme API (PowerGetActiveScheme, PowerEnumerate, PowerRead/Write*ValueIndex)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * The power scheme API works on the store under
 * HKLM\SYSTEM\CurrentControlSet\Control\Power that the kernel reads the
 * power setting values from. The test uses the disk time-out
 * ("Turn off hard disk after") of the active scheme: it exists on Windows
 * and on ReactOS, and the test puts its value back.
 */

#include <apitest.h>

#include <winreg.h>
#include <powrprof.h>
#include <strsafe.h>

static const GUID TestDiskSubgroup =
    { 0x0012ee47, 0x9041, 0x4b5d, { 0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42 } };
static const GUID TestDiskPowerdownTimeout =
    { 0x6738e2c4, 0xe8a5, 0x4a42, { 0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e } };
static const GUID TestNoSuchSetting =
    { 0x0b2ab7f0, 0x4d1c, 0x4f3e, { 0x9a, 0x51, 0x77, 0x13, 0x0c, 0x2d, 0x8e, 0x01 } };

static
VOID
GuidToString(
    _In_ const GUID *Guid,
    _Out_writes_(37) PWSTR Buffer)
{
    StringCchPrintfW(Buffer, 37, L"%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x",
             Guid->Data1, Guid->Data2, Guid->Data3,
             Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
             Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

/* What the kernel reads: the active scheme's own index of the setting */
static
BOOL
ReadStoreIndex(
    _In_ const GUID *Scheme,
    _In_ PCWSTR ValueName,
    _Out_ PDWORD Index)
{
    WCHAR Path[256], SchemeString[37], SubString[37], SettingString[37];
    DWORD Size = sizeof(*Index), Type;
    HKEY Key;
    LONG Error;

    GuidToString(Scheme, SchemeString);
    GuidToString(&TestDiskSubgroup, SubString);
    GuidToString(&TestDiskPowerdownTimeout, SettingString);
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\%s\\%s\\%s",
             SchemeString, SubString, SettingString);
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_QUERY_VALUE, &Key);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    Error = RegQueryValueExW(Key, ValueName, NULL, &Type, (PBYTE)Index, &Size);
    RegCloseKey(Key);
    return Error == ERROR_SUCCESS && Type == REG_DWORD && Size == sizeof(*Index);
}

static
BOOL
Enumerate(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_ POWER_DATA_ACCESSOR Access,
    _In_ const GUID *Wanted,
    _Out_ PULONG Count)
{
    GUID Guid;
    DWORD Size, Error;
    ULONG Index;
    BOOL Found = FALSE;

    for (Index = 0; Index < 1000; Index++)
    {
        /* Without a buffer: the size */
        Size = 0xdead;
        Error = PowerEnumerate(NULL, Scheme, SubGroup, Access, Index, NULL, &Size);
        if (Error == ERROR_NO_MORE_ITEMS)
            break;
        ok(Error == ERROR_MORE_DATA, "Access %d index %lu without buffer: error %lu\n", Access, Index, Error);
        ok(Size == sizeof(GUID), "Access %d index %lu: size %lu\n", Access, Index, Size);

        Size = sizeof(Guid);
        Error = PowerEnumerate(NULL, Scheme, SubGroup, Access, Index, (PUCHAR)&Guid, &Size);
        ok(Error == ERROR_SUCCESS, "Access %d index %lu: error %lu\n", Access, Index, Error);
        if (Error != ERROR_SUCCESS)
            break;
        ok(Size == sizeof(GUID), "Access %d index %lu: size %lu\n", Access, Index, Size);
        if (IsEqualGUID(&Guid, Wanted))
            Found = TRUE;
    }

    *Count = Index;
    return Found;
}

static
VOID
TestScheme(
    _In_ const GUID *Scheme)
{
    DWORD Ac = 0xdead, Dc = 0xdead, Index, Size, Error, Stored;
    ULONG Type, Count;
    BYTE Buffer[16];
    WCHAR Name[256];

    /* Enumeration finds the scheme, the disk subgroup and its time-out */
    ok(Enumerate(NULL, NULL, ACCESS_SCHEME, Scheme, &Count), "Active scheme not enumerated\n");
    ok(Count >= 1, "%lu schemes\n", Count);
    ok(Enumerate(Scheme, NULL, ACCESS_SUBGROUP, &TestDiskSubgroup, &Count),
       "Disk subgroup not enumerated (%lu subgroups)\n", Count);
    ok(Enumerate(Scheme, &TestDiskSubgroup, ACCESS_INDIVIDUAL_SETTING, &TestDiskPowerdownTimeout, &Count),
       "Disk time-out not enumerated (%lu settings)\n", Count);

    /* Reading the indexes */
    Error = PowerReadACValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, &Ac);
    ok(Error == ERROR_SUCCESS, "PowerReadACValueIndex: %lu\n", Error);
    ok(Ac != 0xdead, "No AC index\n");
    Error = PowerReadDCValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, &Dc);
    ok(Error == ERROR_SUCCESS, "PowerReadDCValueIndex: %lu\n", Error);
    ok(Dc != 0xdead, "No DC index\n");
    trace("Disk time-out AC %lu DC %lu\n", Ac, Dc);

    /* The same value as raw data */
    Type = 0xdead;
    Size = 0xdead;
    Error = PowerReadACValue(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, &Type, NULL, &Size);
    ok(Error == ERROR_SUCCESS, "PowerReadACValue without buffer: %lu\n", Error);
    ok(Size == sizeof(DWORD), "Size %lu\n", Size);
    ok(Type == REG_DWORD, "Type %lu\n", Type);
    Size = 2;
    Error = PowerReadACValue(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, NULL, Buffer, &Size);
    ok(Error == ERROR_MORE_DATA, "PowerReadACValue small buffer: %lu\n", Error);
    ok(Size == sizeof(DWORD), "Size %lu\n", Size);
    Size = sizeof(Buffer);
    Error = PowerReadACValue(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, NULL, Buffer, &Size);
    ok(Error == ERROR_SUCCESS, "PowerReadACValue: %lu\n", Error);
    ok(Size == sizeof(DWORD), "Size %lu\n", Size);
    ok(*(PDWORD)Buffer == Ac, "Value %lu, index %lu\n", *(PDWORD)Buffer, Ac);

    /* An unknown setting has no value */
    Error = PowerReadACValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestNoSuchSetting, &Index);
    ok(Error != ERROR_SUCCESS, "Unknown setting read\n");

    /* Writing lands in the scheme's store and reads back */
    Error = PowerWriteACValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, Ac + 60);
    ok(Error == ERROR_SUCCESS, "PowerWriteACValueIndex: %lu\n", Error);
    Error = PowerWriteDCValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, Dc + 120);
    ok(Error == ERROR_SUCCESS, "PowerWriteDCValueIndex: %lu\n", Error);
    Index = 0;
    Error = PowerReadACValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, &Index);
    ok(Error == ERROR_SUCCESS && Index == Ac + 60, "AC index %lu (error %lu), expected %lu\n", Index, Error, Ac + 60);
    Index = 0;
    Error = PowerReadDCValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, &Index);
    ok(Error == ERROR_SUCCESS && Index == Dc + 120, "DC index %lu (error %lu), expected %lu\n", Index, Error, Dc + 120);
    Stored = 0;
    ok(ReadStoreIndex(Scheme, L"ACSettingIndex", &Stored), "No ACSettingIndex in the store\n");
    ok(Stored == Ac + 60, "Stored AC index %lu\n", Stored);
    Stored = 0;
    ok(ReadStoreIndex(Scheme, L"DCSettingIndex", &Stored), "No DCSettingIndex in the store\n");
    ok(Stored == Dc + 120, "Stored DC index %lu\n", Stored);

    /* Put the values back */
    Error = PowerWriteACValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, Ac);
    ok(Error == ERROR_SUCCESS, "PowerWriteACValueIndex: %lu\n", Error);
    Error = PowerWriteDCValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, Dc);
    ok(Error == ERROR_SUCCESS, "PowerWriteDCValueIndex: %lu\n", Error);
    Index = 0;
    Error = PowerReadACValueIndex(NULL, Scheme, &TestDiskSubgroup, &TestDiskPowerdownTimeout, &Index);
    ok(Error == ERROR_SUCCESS && Index == Ac, "AC index %lu (error %lu), expected %lu\n", Index, Error, Ac);

    /* Names */
    Size = sizeof(Name);
    Error = PowerReadFriendlyName(NULL, Scheme, NULL, NULL, (PUCHAR)Name, &Size);
    ok(Error == ERROR_SUCCESS, "PowerReadFriendlyName: %lu\n", Error);
    ok(Error != ERROR_SUCCESS || (Size >= 2 * sizeof(WCHAR) && Name[0] != UNICODE_NULL),
       "Empty scheme name (%lu bytes)\n", Size);
    if (Error == ERROR_SUCCESS)
        trace("Active scheme %S\n", Name);

    /* Making the active scheme active again */
    Error = PowerSetActiveScheme(NULL, Scheme);
    ok(Error == ERROR_SUCCESS, "PowerSetActiveScheme: %lu\n", Error);
}

START_TEST(scheme)
{
    GUID *Active = NULL, *Again = NULL;
    WCHAR Expected[37], Stored[40];
    DWORD Size = sizeof(Stored), Type, Error;
    HKEY Key;

    Error = PowerGetActiveScheme(NULL, &Active);
    ok(Error == ERROR_SUCCESS, "PowerGetActiveScheme: %lu\n", Error);
    ok(Active != NULL, "No active scheme\n");
    if (Error != ERROR_SUCCESS || !Active)
        return;

    /* It is the scheme the store names */
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes",
                          0, KEY_QUERY_VALUE, &Key);
    ok(Error == ERROR_SUCCESS, "No power scheme store: %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        ZeroMemory(Stored, sizeof(Stored));
        Error = RegQueryValueExW(Key, L"ActivePowerScheme", NULL, &Type, (PBYTE)Stored, &Size);
        ok(Error == ERROR_SUCCESS && Type == REG_SZ, "ActivePowerScheme: %lu, type %lu\n", Error, Type);
        RegCloseKey(Key);
        GuidToString(Active, Expected);
        ok(_wcsicmp(Stored, Expected) == 0, "Store names %S, PowerGetActiveScheme %S\n", Stored, Expected);
    }

    TestScheme(Active);

    /* Still the same scheme */
    Error = PowerGetActiveScheme(NULL, &Again);
    ok(Error == ERROR_SUCCESS, "PowerGetActiveScheme: %lu\n", Error);
    if (Error == ERROR_SUCCESS && Again)
    {
        ok(IsEqualGUID(Again, Active), "Active scheme changed\n");
        LocalFree(Again);
    }

    LocalFree(Active);
}

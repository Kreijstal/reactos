/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Kernel Shim Engine device flags (KseQueryDeviceFlags, KseSetDeviceFlags)
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

typedef NTSTATUS (NTAPI *PFN_KSE_QUERY_DEVICE_FLAGS)(PCWSTR, PCWSTR, PULONG64);
typedef NTSTATUS (NTAPI *PFN_KSE_SET_DEVICE_FLAGS)(PCWSTR, PCWSTR, ULONG64);

#define TEST_DEVICE_KEY     L"KMTEST:ROOT\\KSE_DEVICE_FLAGS"
#define TEST_PROVIDER       L"KMTEST"
#define COMPAT_KEY          L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Compatibility\\Device\\"

static PFN_KSE_QUERY_DEVICE_FLAGS pKseQueryDeviceFlags;
static PFN_KSE_SET_DEVICE_FLAGS pKseSetDeviceFlags;

static
PVOID
GetRoutine(
    _In_ PCWSTR Name)
{
    UNICODE_STRING RoutineName;

    RtlInitUnicodeString(&RoutineName, Name);
    return MmGetSystemRoutineAddress(&RoutineName);
}

static
NTSTATUS
OpenTestKey(
    _Out_ PHANDLE Key,
    _In_ ACCESS_MASK Access)
{
    UNICODE_STRING Path = RTL_CONSTANT_STRING(COMPAT_KEY L"KMTEST:ROOT!KSE_DEVICE_FLAGS");
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwOpenKey(Key, Access, &ObjectAttributes);
}

static
VOID
DeleteTestKey(VOID)
{
    HANDLE Key;

    if (NT_SUCCESS(OpenTestKey(&Key, DELETE)))
    {
        ZwDeleteKey(Key);
        ZwClose(Key);
    }
}

/* drvmain.sdb carries the xHCI errata Windows ships for AMD's FCH controller */
static
VOID
TestDatabase(VOID)
{
    ULONG64 Flags;
    NTSTATUS Status;

    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(L"USBXHCI:PCI\\VEN_1022&DEV_7814", L"USBXHCI", &Flags);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulonglong(Flags, 0x0000140800000080ULL);

    /* Device keys and providers are matched without regard to case */
    Flags = 0;
    Status = pKseQueryDeviceFlags(L"usbxhci:pci\\ven_1022&dev_7814", L"usbxhci", &Flags);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulonglong(Flags, 0x0000140800000080ULL);

    /* Only the exact key matches: a more specific one is a different entry */
    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(L"USBXHCI:PCI\\VEN_1022&DEV_7814&REV_09", L"USBXHCI", &Flags);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulonglong(Flags, 0ULL);

    /* The flags belong to a provider */
    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(L"USBXHCI:PCI\\VEN_1022&DEV_7814", L"USBHUB", &Flags);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulonglong(Flags, 0ULL);

    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(L"USBXHCI:PCI\\VEN_FFFF&DEV_FFFF", L"USBXHCI", &Flags);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulonglong(Flags, 0ULL);
}

/* KseSetDeviceFlags writes the registry, which is consulted first */
static
VOID
TestRegistry(VOID)
{
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(TEST_PROVIDER);
    ULONG DwordValue = 0x55;
    ULONG64 Flags;
    HANDLE Key;
    NTSTATUS Status;

    DeleteTestKey();

    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(TEST_DEVICE_KEY, TEST_PROVIDER, &Flags);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulonglong(Flags, 0ULL);

    Status = pKseSetDeviceFlags(TEST_DEVICE_KEY, TEST_PROVIDER, 0x1234567887654321ULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Flags = 0;
    Status = pKseQueryDeviceFlags(TEST_DEVICE_KEY, TEST_PROVIDER, &Flags);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulonglong(Flags, 0x1234567887654321ULL);

    /* The value lives under the device key with '\' turned into '!' */
    Status = OpenTestKey(&Key, KEY_SET_VALUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Another provider of the same device has no flags */
    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(TEST_DEVICE_KEY, L"KMTEST2", &Flags);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulonglong(Flags, 0ULL);

    /* Only a REG_QWORD is a set of flags */
    Status = ZwSetValueKey(Key, &ValueName, 0, REG_DWORD, &DwordValue, sizeof(DwordValue));
    ok_eq_hex(Status, STATUS_SUCCESS);
    Flags = 0xdeadbeef;
    Status = pKseQueryDeviceFlags(TEST_DEVICE_KEY, TEST_PROVIDER, &Flags);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulonglong(Flags, 0ULL);

    ZwDeleteKey(Key);
    ZwClose(Key);
}

START_TEST(KseDeviceFlags)
{
    RTL_OSVERSIONINFOW Version = { sizeof(Version) };

    /* Windows 8 introduced the device flags */
    RtlGetVersion(&Version);
    if (Version.dwMajorVersion < 6 || (Version.dwMajorVersion == 6 && Version.dwMinorVersion < 2))
    {
        skip(FALSE, "No KSE device flags before NT 6.2 (%lu.%lu)\n", Version.dwMajorVersion, Version.dwMinorVersion);
        return;
    }

    pKseQueryDeviceFlags = (PFN_KSE_QUERY_DEVICE_FLAGS)GetRoutine(L"KseQueryDeviceFlags");
    pKseSetDeviceFlags = (PFN_KSE_SET_DEVICE_FLAGS)GetRoutine(L"KseSetDeviceFlags");
    ok(pKseQueryDeviceFlags != NULL, "KseQueryDeviceFlags is not exported\n");
    ok(pKseSetDeviceFlags != NULL, "KseSetDeviceFlags is not exported\n");
    if (!pKseQueryDeviceFlags || !pKseSetDeviceFlags)
        return;

    TestDatabase();
    TestRegistry();
}

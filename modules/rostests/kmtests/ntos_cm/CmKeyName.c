/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite registry key name test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/*
 * The name a key reports does not depend on the case the caller opened it
 * with: the master keys are \REGISTRY\MACHINE and \REGISTRY\USER, which is
 * also what every driver's RegistryPath starts with.
 */
static
VOID
CheckKeyName(
    _In_ PCWSTR OpenName,
    _In_ PCWSTR ExpectedPrefix)
{
    NTSTATUS Status;
    UNICODE_STRING KeyNameString;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE KeyHandle;
    struct
    {
        KEY_NAME_INFORMATION Info;
        WCHAR Buffer[256];
    } NameInfo;
    ULONG ResultLength;
    SIZE_T PrefixLength;

    RtlInitUnicodeString(&KeyNameString, OpenName);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyNameString,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenKey(&KeyHandle, KEY_QUERY_VALUE, &ObjectAttributes);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No key (%ls)\n", OpenName))
        return;

    RtlZeroMemory(&NameInfo, sizeof(NameInfo));
    Status = ZwQueryKey(KeyHandle,
                        KeyNameInformation,
                        &NameInfo,
                        sizeof(NameInfo) - sizeof(WCHAR),
                        &ResultLength);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PrefixLength = wcslen(ExpectedPrefix) * sizeof(WCHAR);
        ok(NameInfo.Info.NameLength >= PrefixLength &&
           RtlCompareMemory(NameInfo.Info.Name, ExpectedPrefix, PrefixLength) == PrefixLength,
           "Opened '%ls', got '%ls', expected it to start with '%ls'\n",
           OpenName,
           NameInfo.Info.Name, /* terminated: zeroed, one WCHAR held back */
           ExpectedPrefix);
    }

    ObCloseHandle(KeyHandle, KernelMode);
}

START_TEST(CmKeyName)
{
    CheckKeyName(L"\\Registry\\Machine\\System", L"\\REGISTRY\\MACHINE\\SYSTEM");
    CheckKeyName(L"\\registry\\machine\\system", L"\\REGISTRY\\MACHINE\\SYSTEM");
    CheckKeyName(L"\\Registry\\Machine\\Software", L"\\REGISTRY\\MACHINE\\SOFTWARE");
    CheckKeyName(L"\\Registry\\Machine", L"\\REGISTRY\\MACHINE");
    CheckKeyName(L"\\registry\\user", L"\\REGISTRY\\USER");
}

/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Validate the apiset lookup in ApiSetResolveToHost
 * COPYRIGHT:   Copyright 2024 Mark Jansen <mark.jansen@reactos.org>
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include <apitest.h>
#include "apisetsp.h"

static void
resolve_single(PCUNICODE_STRING Apiset)
{
    UNICODE_STRING Tmp = {0};
    NTSTATUS Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, Apiset, &Tmp);
    ok_ntstatus(Status, STATUS_SUCCESS);

    BOOLEAN Resolved = FALSE;
    UNICODE_STRING Result = {0};

    Status = ApiSetResolveToHost(~0u, &Tmp, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, TRUE);
    ok(Result.Buffer != NULL, "Got NULL\n");
}

static void
resolve_fail_single(PCUNICODE_STRING Apiset)
{
    UNICODE_STRING Tmp = {0};
    NTSTATUS Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, Apiset, &Tmp);
    ok_ntstatus(Status, STATUS_SUCCESS);

    BOOLEAN Resolved = FALSE;
    UNICODE_STRING Result = {0, 0, (PWSTR)0xbadbeef};

    Status = ApiSetResolveToHost(~0u, &Tmp, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, FALSE);
    ok_ptr(Result.Buffer, (PWSTR)0xbadbeef);
}

static void
test_single(PCUNICODE_STRING Apiset)
{
    winetest_push_context("%S", Apiset->Buffer);
    resolve_single(Apiset);
    winetest_pop_context();
}

static void
fail_single(PCUNICODE_STRING Apiset)
{
    winetest_push_context("%S", Apiset->Buffer);
    resolve_fail_single(Apiset);
    winetest_pop_context();
}

static void
test_version_host(void)
{
    static const UNICODE_STRING Names[] = {
        RTL_CONSTANT_STRING(L"api-ms-win-core-version-l1-1-0.dll"),
        RTL_CONSTANT_STRING(L"api-ms-win-core-version-l1-1-1.dll"),
        RTL_CONSTANT_STRING(L"api-ms-win-core-versionansi-l1-1-0.dll"),
        RTL_CONSTANT_STRING(L"api-ms-win-core-versionansi-l1-1-1.dll"),
    };
    static const char *Exports[] = {
        "GetFileVersionInfoA", "GetFileVersionInfoW",
        "GetFileVersionInfoExA", "GetFileVersionInfoExW",
        "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeW",
        "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW",
        "VerQueryValueA", "VerQueryValueW",
    };
    UNICODE_STRING Version = RTL_CONSTANT_STRING(L"version.dll");

    for (DWORD i = 0; i < RTL_NUMBER_OF(Names); ++i)
    {
        BOOLEAN Resolved = FALSE;
        UNICODE_STRING Host = {0};
        NTSTATUS Status;
        HMODULE Module;

        winetest_push_context("%S", Names[i].Buffer);
        Status = ApiSetResolveToHost(APISET_WIN10, &Names[i], &Resolved, &Host);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_eq_bool(Resolved, TRUE);
        if (!NT_SUCCESS(Status) || !Resolved || !Host.Buffer)
        {
            winetest_pop_context();
            continue;
        }
        ok(RtlEqualUnicodeString(&Host, &Version, TRUE), "Unexpected host %S\n", Host.Buffer);

        /* Check the selected host really provides the version API imports. */
        Module = LoadLibraryW(Host.Buffer);
        ok(Module != NULL, "Failed to load %S: %lu\n", Host.Buffer, GetLastError());
        if (Module)
        {
            for (DWORD j = 0; j < RTL_NUMBER_OF(Exports); ++j)
                ok(GetProcAddress(Module, Exports[j]) != NULL,
                   "%S does not export %s\n", Host.Buffer, Exports[j]);
            FreeLibrary(Module);
        }
        winetest_pop_context();
    }
}


START_TEST(apisets)
{
    test_version_host();

    // Ensure we can find some manually selected ones (both with and without extension):
    UNICODE_STRING Console1 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Console-L1-1-0.dll");
    UNICODE_STRING Console2 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Console-L1-1-0");
    UNICODE_STRING Handle1 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Handle-L1-1-0.dll");
    UNICODE_STRING Handle2 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Handle-L1-1-0");
    UNICODE_STRING Heap1 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Heap-L1-1-0.dll");
    UNICODE_STRING Heap2 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Heap-L1-1-0");

    test_single(&Console1);
    test_single(&Console2);
    test_single(&Handle1);
    test_single(&Handle2);
    test_single(&Heap1);
    test_single(&Heap2);

    // The last version number does not take part in the lookup
    UNICODE_STRING File1 = RTL_CONSTANT_STRING(L"api-ms-win-core-file-l1-1-0.dll");
    UNICODE_STRING Threads1 = RTL_CONSTANT_STRING(L"api-ms-win-core-processthreads-l1-1-0.dll");
    UNICODE_STRING Threads9 = RTL_CONSTANT_STRING(L"api-ms-win-core-processthreads-l1-1-9");
    // PowerShell's SendInput and SendMessageTimeout imports
    UNICODE_STRING Keyboard1 = RTL_CONSTANT_STRING(L"ext-ms-win-ntuser-keyboard-l1-2-1.dll");
    UNICODE_STRING Window0 = RTL_CONSTANT_STRING(L"ext-ms-win-rtcore-ntuser-window-ext-l1-1-0.dll");

    test_single(&File1);
    test_single(&Threads1);
    test_single(&Threads9);
    test_single(&Keyboard1);
    test_single(&Window0);

    // Walk the entire table, to ensure we can find each entry
    for (DWORD n = 0; n < g_ApisetsCount; ++n)
    {
        PCUNICODE_STRING Apiset = &g_Apisets[n].Name;
        winetest_push_context("%ld: %S", n, Apiset->Buffer);
        resolve_single(Apiset);
        winetest_pop_context();
    }

    UNICODE_STRING Fail1 = RTL_CONSTANT_STRING(L"");
    UNICODE_STRING Fail2 = RTL_CONSTANT_STRING(L"api-");
    UNICODE_STRING Fail3 = RTL_CONSTANT_STRING(L"ext");
    UNICODE_STRING Fail4 = RTL_CONSTANT_STRING(L"api-ms-win-core-processthreads-l1-2-0.dll");

    fail_single(&Fail1);
    fail_single(&Fail2);
    fail_single(&Fail3);
    fail_single(&Fail4);
}

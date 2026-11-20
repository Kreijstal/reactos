/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQuerySystemInformation
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 */

#include "precomp.h"

/*
 * Boot-start drivers are loaded by the OS loader, but the kernel must be able
 * to recognize them as already loaded when their service is loaded again, so
 * their full name must be the one the kernel would build for that service.
 */
static
VOID
TestBootDriverModuleNames(VOID)
{
    static const PCSTR BootDrivers[] = { "disk.sys", "partmgr.sys", "mountmgr.sys" };
    PRTL_PROCESS_MODULES Modules;
    PRTL_PROCESS_MODULE_INFORMATION Module;
    CHAR Expected[MAX_PATH];
    ULONG Length = 0, i, j;
    BOOLEAN Found;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemModuleInformation, NULL, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    if (Length == 0)
    {
        skip("No module information length\n");
        return;
    }

    /* Leave room for a driver loaded in the meantime */
    Length += 16 * sizeof(RTL_PROCESS_MODULE_INFORMATION);
    Modules = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (!Modules)
    {
        skip("Out of memory\n");
        return;
    }

    Status = NtQuerySystemInformation(SystemModuleInformation, Modules, Length, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Modules);
        return;
    }

    for (i = 0; i < _countof(BootDrivers); i++)
    {
        Found = FALSE;
        for (j = 0; j < Modules->NumberOfModules; j++)
        {
            Module = &Modules->Modules[j];
            if (Module->OffsetToFileName >= sizeof(Module->FullPathName) ||
                _stricmp(&Module->FullPathName[Module->OffsetToFileName], BootDrivers[i]) != 0)
            {
                continue;
            }

            Found = TRUE;
            StringCbPrintfA(Expected, sizeof(Expected), "\\SystemRoot\\System32\\drivers\\%s", BootDrivers[i]);
            ok(_stricmp(Module->FullPathName, Expected) == 0,
               "Module %s has full name '%s', expected '%s'\n",
               BootDrivers[i], Module->FullPathName, Expected);
            ok(Module->OffsetToFileName == strlen(Expected) - strlen(BootDrivers[i]),
               "Module %s has file name offset %u\n", BootDrivers[i], Module->OffsetToFileName);
        }

        if (!Found)
            skip("%s is not loaded\n", BootDrivers[i]);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Modules);
}

START_TEST(NtQuerySystemInformation)
{
    NTSTATUS Status;

    Status = NtQuerySystemInformation(0, NULL, 0, NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = NtQuerySystemInformation(0x80000000, NULL, 0, NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);

    TestBootDriverModuleNames();
}

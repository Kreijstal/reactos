/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Entry point and helpers
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#if defined(_M_ARM64)
#include <reactos/arm64/early_uart.h>
#endif

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS ********************************************************************/

EFI_HANDLE GlobalImageHandle;
EFI_SYSTEM_TABLE *GlobalSystemTable;
PVOID UefiServiceStack;
PVOID BasicStack;

void _changestack(VOID);

/* FUNCTIONS ******************************************************************/

EFI_STATUS
EfiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    PCSTR CmdLine = ""; // FIXME: Determine a command-line from UEFI boot options

    GlobalImageHandle = ImageHandle;
    GlobalSystemTable = SystemTable;

#if defined(_M_ARM64)
    /* Detect the platform serial port from ACPI SPCR/DBG2 while the UEFI
     * configuration tables are still mapped. Everything downstream
     * (EarlyUartPuts diagnostics, the UART page-table mappings, the
     * loader-block hand-off to the kernel) keys off this detection; without
     * it EarlyUartBaseAddress stays 0 and all of it silently no-ops. */
    EarlyUartInitialize(0);
#endif

    /* Load the default settings from the command-line */
    LoadSettings(CmdLine);

    /* Debugger pre-initialization */
    DebugInit(BootMgrInfo.DebugString);

    MachInit(CmdLine);

    /* UI pre-initialization */
    if (!UiInitialize(FALSE))
    {
        UiMessageBoxCritical("Unable to initialize UI.");
        goto Quit;
    }

    /* Initialize memory manager */
    if (!MmInitializeMemoryManager())
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
        goto Quit;
    }

    /* Initialize I/O subsystem */
    FsInit();

    /* Initialize the module list */
    if (!PeLdrInitializeModuleList())
    {
        UiMessageBoxCritical("Unable to initialize module list.");
        goto Quit;
    }

    if (!MachInitializeBootDevices())
    {
        UiMessageBoxCritical("Error when detecting hardware.");
        goto Quit;
    }

    /* 0x32000 is what UEFI defines, but we can go smaller if we want */
    BasicStack = (PVOID)((ULONG_PTR)0x32000 + (ULONG_PTR)MmAllocateMemoryWithType(0x32000, LoaderOsloaderStack));
    _changestack();

Quit:
    /* If we reach this point, something went wrong before, therefore reboot */
    Reboot();

    UNREACHABLE;
    return 0;
}

DECLSPEC_NORETURN
void
ExecuteLoaderCleanly(PVOID PreviousStack)
{
    TRACE("ExecuteLoaderCleanly Entry\n");
    UefiServiceStack = PreviousStack;

    RunLoader();
    Reboot();
    UNREACHABLE;
}

#ifndef _M_ARM
DECLSPEC_NORETURN
VOID __cdecl Reboot(VOID)
{
    //TODO: Replace with a true firmware reboot eventually
    WARN("Something has gone wrong - halting FreeLoader\n");
    for (;;)
    {
        NOTHING;
    }
}
#endif

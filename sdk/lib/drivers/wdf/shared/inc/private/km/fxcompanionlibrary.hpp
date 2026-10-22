/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device companion library (ReactOS stub)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * Device companions (KMDF 1.23+) need the UMDF companion host, which ReactOS
 * lacks. This library therefore never asks for a companion, so no
 * FxCompanionTarget is ever created.
 */

#pragma once

#define FX_COMPANION_LIBRARY_TAG 'LCxF'

class FxCompanionLibrary {
public:
    static
    NTSTATUS
    _CreateAndInitialize(
        _Out_ FxCompanionLibrary** CompanionLibrary
        )
    {
        *CompanionLibrary = new FxCompanionLibrary();
        return (*CompanionLibrary != NULL) ? STATUS_SUCCESS
                                           : STATUS_INSUFFICIENT_RESOURCES;
    }

    BOOLEAN
    IsCompanionRequiredForDevice(
        _In_ FxDevice* Device,
        _Outptr_result_maybenull_ PCWSTR* CompanionName
        )
    {
        UNREFERENCED_PARAMETER(Device);
        *CompanionName = NULL;
        return FALSE;
    }

    PVOID
    operator new(
        _In_ size_t Size
        )
    {
        return ExAllocatePoolWithTag(NonPagedPool, Size, FX_COMPANION_LIBRARY_TAG);
    }

    VOID
    operator delete(
        _In_ PVOID Memory
        )
    {
        ExFreePoolWithTag(Memory, FX_COMPANION_LIBRARY_TAG);
    }
};

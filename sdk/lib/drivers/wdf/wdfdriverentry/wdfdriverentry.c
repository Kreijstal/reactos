/*
 * PROJECT:     ReactOS KMDF: driver initialization static library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Main file
 * COPYRIGHT:   Copyright 2021 Max Korostil <mrmks04@yandex.ru>
 *              Copyright 2026 ReactOS contributors
 */

/*
 * This is the client side of the KMDF loader handshake, the counterpart of
 * the WDK's wdfdriverentry.lib. Its entry point, FxDriverEntry, runs before
 * the driver's own DriverEntry:
 *
 * 1. Binds the driver to the framework with WdfVersionBind, passing a
 *    WDF_BIND_INFO2 like the KMDF 1.25+ WDK stub does.
 * 2. Binds every class extension the driver uses. Clients describe each
 *    class extension with a WDF_CLASS_BIND_INFO(2) placed in the
 *    ".kmdfclassbind$b" section. This library brackets that section with
 *    markers in "$a" and "$c" and walks the entries in between, so GCC-built
 *    clients bind exactly like WDK-built ones.
 * 3. Calls DriverEntry, and undoes the bindings on failure or unload.
 */

#include <ntddk.h>
#include <windef.h>

#define WDF_STUB
#include <fxldr.h>
#include "wdf.h"

#define WDFENTRY_TAG 'EFDW'

// supplied by the driver this library is linked into
extern
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath);

/* Globals the public KMDF headers expect the stub to provide */
const WDFFUNC *WdfFunctions;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
WDFFUNC WdfDriverMiniportUnloadOverride;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfFunctionCount = WdfFunctionTableNumEntries;
ULONG WdfStructureCount = WDF_STRUCTURE_TABLE_NUM_ENTRIES;
WDF_STRUCT_INFO WdfStructures;

/* Defined by wdffuncenum.h in every client source file */
extern ULONG WdfMinimumVersionRequired;

WDF_BIND_INFO2 WdfBindInfo =
{
    .V1.Size = sizeof(WDF_BIND_INFO2),
    .V1.Component = L"KmdfLibrary",
    .V1.Version.Major = __WDF_MAJOR_VERSION,
    .V1.Version.Minor = __WDF_MINOR_VERSION,
    .V1.Version.Build = __WDF_BUILD_NUMBER,
    .V1.FuncCount = WdfFunctionTableNumEntries,
    .V1.FuncTable = (WDFFUNC *)&WdfFunctions,
    .MinimumVersionRequired = &WdfMinimumVersionRequired,
    .ClientVersionHigherThanFramework = &WdfClientVersionHigherThanFramework,
    .FuncCountPtr = &WdfFunctionCount,
    .StructCountPtr = &WdfStructureCount,
    .StructTable = &WdfStructures,
};

/*
 * Class extension bind table. The layout matches the WDK 1.33 stub: a
 * 16-byte start marker in "$a", the client entries in "$b", an 8-byte end
 * marker in "$c" and the last bound entry in "$d". The linker orders the
 * "$" groups alphabetically and may pad between entries with zeroes.
 */
typedef struct _KMDF_CLASS_BIND_MARKER
{
    PVOID Reserved[2];
} KMDF_CLASS_BIND_MARKER;

#ifdef _MSC_VER
#pragma section(".kmdfclassbind$a", read, write)
#pragma section(".kmdfclassbind$c", read, write)
#pragma section(".kmdfclassbind$d", read, write)
#define KMDF_CLASS_BIND_SECTION(_Section) __declspec(allocate(_Section))
#else
#define KMDF_CLASS_BIND_SECTION(_Section) __attribute__((section(_Section), used))
#endif

KMDF_CLASS_BIND_SECTION(".kmdfclassbind$a") KMDF_CLASS_BIND_MARKER __KMDF_CLASS_BIND_START = { { NULL, NULL } };
KMDF_CLASS_BIND_SECTION(".kmdfclassbind$c") PVOID __KMDF_CLASS_BIND_END = NULL;
KMDF_CLASS_BIND_SECTION(".kmdfclassbind$d") PWDF_CLASS_BIND_INFO __KMDF_CLASS_BIND_LAST_BOUND =
    (PWDF_CLASS_BIND_INFO)&__KMDF_CLASS_BIND_START;

#define KMDF_CLASS_BIND_FIRST ((const BYTE *)(&__KMDF_CLASS_BIND_START + 1))
#define KMDF_CLASS_BIND_END   ((PWDF_CLASS_BIND_INFO)&__KMDF_CLASS_BIND_END)

PDRIVER_UNLOAD pOriginalUnload = NULL;
UNICODE_STRING gRegistryPath;

/*
 * Returns the next class bind entry at or after Current, End when there is
 * none left, or NULL when the table is corrupt. Same rules as
 * FxGetNextClassBindInfo in fxldr.h: zero padding is skipped and each entry
 * must have the size of a WDF_CLASS_BIND_INFO or WDF_CLASS_BIND_INFO2.
 */
static
PWDF_CLASS_BIND_INFO
FxStubGetNextClassBindInfo(
    _In_ const BYTE *Current,
    _In_ PWDF_CLASS_BIND_INFO End)
{
    while ((Current + sizeof(PVOID) <= (const BYTE *)End) && (*(PVOID *)Current == NULL))
    {
        Current += sizeof(PVOID);
    }

    if (Current >= (const BYTE *)End)
    {
        return End;
    }

    if ((Current + sizeof(WDF_CLASS_BIND_INFO) <= (const BYTE *)End) &&
        (((PWDF_CLASS_BIND_INFO)Current)->Size == sizeof(WDF_CLASS_BIND_INFO)))
    {
        return (PWDF_CLASS_BIND_INFO)Current;
    }

    if ((Current + sizeof(WDF_CLASS_BIND_INFO2) <= (const BYTE *)End) &&
        (((PWDF_CLASS_BIND_INFO)Current)->Size == sizeof(WDF_CLASS_BIND_INFO2)))
    {
        return (PWDF_CLASS_BIND_INFO)Current;
    }

    return NULL;
}

static
NTSTATUS
FxStubBindClasses(
    _In_ PWDF_BIND_INFO BindInfo)
{
    PWDF_CLASS_BIND_INFO End = KMDF_CLASS_BIND_END;
    PWDF_CLASS_BIND_INFO Info;
    const BYTE *Current;
    NTSTATUS Status;

    if ((const BYTE *)&__KMDF_CLASS_BIND_START > (const BYTE *)End)
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "FxStubBindClasses: invalid class bind section, start %p end %p\n",
                   &__KMDF_CLASS_BIND_START, End);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    for (Current = KMDF_CLASS_BIND_FIRST; ; Current += Info->Size)
    {
        Info = FxStubGetNextClassBindInfo(Current, End);
        if (Info == NULL)
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "FxStubBindClasses: invalid class bind entry near %p\n", Current);
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        if (Info == End)
        {
            return STATUS_SUCCESS;
        }

        __KMDF_CLASS_BIND_LAST_BOUND = Info;

        if (Info->ClientBindClass != NULL)
        {
            Status = Info->ClientBindClass((PFN_WDF_VERSION_BIND_CLASS)WdfVersionBindClass,
                                           BindInfo,
                                           (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals,
                                           Info);
        }
        else
        {
            Status = WdfVersionBindClass(BindInfo,
                                         (PWDF_COMPONENT_GLOBALS *)WdfDriverGlobals,
                                         Info);
        }

        if (!NT_SUCCESS(Status))
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "FxStubBindClasses: binding class %ws failed 0x%lx\n",
                       Info->ClassName, Status);
            return Status;
        }
    }
}

static
VOID
FxStubUnbindClasses(
    _In_ PWDF_BIND_INFO BindInfo)
{
    PWDF_CLASS_BIND_INFO Last = __KMDF_CLASS_BIND_LAST_BOUND;
    PWDF_CLASS_BIND_INFO End;
    PWDF_CLASS_BIND_INFO Info;
    const BYTE *Current;

    if (Last == (PWDF_CLASS_BIND_INFO)&__KMDF_CLASS_BIND_START)
    {
        /* Nothing was bound */
        return;
    }

    /* Unbind everything up to and including the last entry we tried */
    End = (PWDF_CLASS_BIND_INFO)((PUCHAR)Last + Last->Size);

    for (Current = KMDF_CLASS_BIND_FIRST; ; Current += Info->Size)
    {
        Info = FxStubGetNextClassBindInfo(Current, End);
        if (Info == NULL)
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "FxStubUnbindClasses: invalid class bind entry near %p\n", Current);
            break;
        }

        if (Info == End)
        {
            break;
        }

        if (Info->ClientUnbindClass != NULL)
        {
            Info->ClientUnbindClass(WdfVersionUnbindClass,
                                    BindInfo,
                                    (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals,
                                    Info);
        }
        else
        {
            WdfVersionUnbindClass(BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, Info);
        }
    }

    __KMDF_CLASS_BIND_LAST_BOUND = (PWDF_CLASS_BIND_INFO)&__KMDF_CLASS_BIND_START;
}

static
VOID
FxDriverUnloadCommon(VOID)
{
    FxStubUnbindClasses(&WdfBindInfo.V1);
    WdfVersionUnbind(&gRegistryPath, &WdfBindInfo.V1, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals);
}

VOID
NTAPI
FxDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    if (pOriginalUnload != NULL)
    {
        pOriginalUnload(DriverObject);
    }
    FxDriverUnloadCommon();
}

NTSTATUS
NTAPI
FxDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;

    if (DriverObject == NULL)
    {
        return DriverEntry(DriverObject, RegistryPath);
    }

    // Copy registry path
    gRegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    gRegistryPath.Buffer = ExAllocatePoolWithTag(PagedPool,
                                                 gRegistryPath.MaximumLength,
                                                 WDFENTRY_TAG);

    if (gRegistryPath.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&gRegistryPath, RegistryPath);

    // Bind wdf driver to framework
    status = WdfVersionBind(DriverObject,
                            RegistryPath,
                            &WdfBindInfo.V1,
                            (PWDF_COMPONENT_GLOBALS*)(&WdfDriverGlobals));

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WdfDriverMiniportUnloadOverride = WdfFunctions[WdfDriverMiniportUnloadTableIndex];

    // Bind the class extensions the driver uses
    status = FxStubBindClasses(&WdfBindInfo.V1);
    if (!NT_SUCCESS(status))
    {
        FxDriverUnloadCommon();
        return status;
    }

    // Call original entry point
    status = DriverEntry(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status))
    {
        FxDriverUnloadCommon();
        return status;
    }

    if (WdfDriverGlobals->DisplaceDriverUnload)
    {
        pOriginalUnload = DriverObject->DriverUnload;
        DriverObject->DriverUnload = FxDriverUnload;
    }
    return STATUS_SUCCESS;
}

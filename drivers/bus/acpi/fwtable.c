/*
 * PROJECT:     ReactOS ACPI bus driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     'ACPI' firmware table provider (GetSystemFirmwareTable)
 */

/*
 * acpi.sys registers the 'ACPI' provider of SystemFirmwareTableInformation
 * with SystemRegisterFirmwareTableInformationHandler once the tables are
 * loaded. Enumerate lists the signature of every table that the firmware
 * describes through the RSDT/XSDT, duplicates included; the DSDT, the FACS
 * and the root tables themselves are left out of that list. Get returns the
 * first table that carries the requested signature, the DSDT included.
 * Tables that AML loads at run time are not firmware tables and are skipped.
 */

#include "precomp.h"

#include <accommon.h>
#include <actables.h>

#define NDEBUG
#include <debug.h>

#define ACPI_PROVIDER_SIGNATURE 'ACPI'

/*
 * <ndk/exfuncs.h> needs <ntifs.h>, which this driver does not build with.
 * 75 is SystemRegisterFirmwareTableInformationHandler.
 */
#define SystemRegisterFirmwareTableInformationHandler 75

NTSYSAPI
NTSTATUS
NTAPI
ZwSetSystemInformation(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength);

static
BOOLEAN
AcpipIsFirmwareTable(
    _In_ ACPI_TABLE_DESC *TableDesc)
{
    return (TableDesc->Flags & ACPI_TABLE_ORIGIN_MASK) != ACPI_TABLE_ORIGIN_INTERNAL_VIRTUAL;
}

static
BOOLEAN
AcpipIsEnumeratedTable(
    _In_ ACPI_TABLE_DESC *TableDesc)
{
    if (!AcpipIsFirmwareTable(TableDesc))
        return FALSE;

    return !ACPI_COMPARE_NAMESEG(TableDesc->Signature.Ascii, ACPI_SIG_DSDT) &&
           !ACPI_COMPARE_NAMESEG(TableDesc->Signature.Ascii, ACPI_SIG_FACS) &&
           !ACPI_COMPARE_NAMESEG(TableDesc->Signature.Ascii, ACPI_SIG_RSDT) &&
           !ACPI_COMPARE_NAMESEG(TableDesc->Signature.Ascii, ACPI_SIG_XSDT);
}

static
NTSTATUS
AcpipEnumerateTables(
    _Out_writes_bytes_(*TableBufferLength) PULONG Signatures,
    _Inout_ PULONG TableBufferLength)
{
    ACPI_TABLE_DESC *TableDesc;
    ULONG Count = 0;
    ULONG Index;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    (void)AcpiUtAcquireMutex(ACPI_MTX_TABLES);

    for (i = 0; i < AcpiGbl_RootTableList.CurrentTableCount; i++)
    {
        if (AcpipIsEnumeratedTable(&AcpiGbl_RootTableList.Tables[i]))
            Count++;
    }

    if (Count == 0)
    {
        Status = STATUS_ACPI_INVALID_TABLE;
    }
    else if (*TableBufferLength < Count * sizeof(ULONG))
    {
        *TableBufferLength = Count * sizeof(ULONG);
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        for (i = 0, Index = 0; i < AcpiGbl_RootTableList.CurrentTableCount; i++)
        {
            TableDesc = &AcpiGbl_RootTableList.Tables[i];
            if (AcpipIsEnumeratedTable(TableDesc))
                Signatures[Index++] = TableDesc->Signature.Integer;
        }
        ASSERT(Index == Count);
        *TableBufferLength = Count * sizeof(ULONG);
    }

    (void)AcpiUtReleaseMutex(ACPI_MTX_TABLES);

    return Status;
}

static
NTSTATUS
AcpipGetTable(
    _In_ ULONG Signature,
    _Out_writes_bytes_(*TableBufferLength) PVOID TableBuffer,
    _Inout_ PULONG TableBufferLength)
{
    ACPI_TABLE_DESC *TableDesc;
    ACPI_TABLE_HEADER *Table;
    ACPI_STATUS AcpiStatus;
    ULONG i;
    NTSTATUS Status = STATUS_NOT_FOUND;

    (void)AcpiUtAcquireMutex(ACPI_MTX_TABLES);

    for (i = 0; i < AcpiGbl_RootTableList.CurrentTableCount; i++)
    {
        TableDesc = &AcpiGbl_RootTableList.Tables[i];
        if ((TableDesc->Signature.Integer != Signature) ||
            !AcpipIsFirmwareTable(TableDesc))
        {
            continue;
        }

        AcpiStatus = AcpiTbGetTable(TableDesc, &Table);
        if (ACPI_FAILURE(AcpiStatus))
        {
            DPRINT1("AcpiTbGetTable(%.4s) failed: %s\n",
                    TableDesc->Signature.Ascii, AcpiFormatException(AcpiStatus));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        if (*TableBufferLength < Table->Length)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            RtlCopyMemory(TableBuffer, Table, Table->Length);
            Status = STATUS_SUCCESS;
        }
        *TableBufferLength = Table->Length;

        AcpiTbPutTable(TableDesc);
        break;
    }

    (void)AcpiUtReleaseMutex(ACPI_MTX_TABLES);

    return Status;
}

static
NTSTATUS
__cdecl
AcpipFirmwareTableHandler(
    _Inout_ PSYSTEM_FIRMWARE_TABLE_INFORMATION Info)
{
    PAGED_CODE();

    switch (Info->Action)
    {
        case SystemFirmwareTable_Enumerate:
            return AcpipEnumerateTables((PULONG)Info->TableBuffer,
                                        &Info->TableBufferLength);

        case SystemFirmwareTable_Get:
            return AcpipGetTable(Info->TableID,
                                 Info->TableBuffer,
                                 &Info->TableBufferLength);

        default:
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS
AcpiInitializeKernelTableHandler(
    _In_ BOOLEAN Register,
    _In_ PDRIVER_OBJECT DriverObject)
{
    SYSTEM_FIRMWARE_TABLE_HANDLER Handler;

    PAGED_CODE();

    Handler.ProviderSignature = ACPI_PROVIDER_SIGNATURE;
    Handler.Register = Register;
    Handler.FirmwareTableHandler = AcpipFirmwareTableHandler;
    Handler.DriverObject = DriverObject;

    return ZwSetSystemInformation(SystemRegisterFirmwareTableInformationHandler,
                                  &Handler,
                                  sizeof(Handler));
}

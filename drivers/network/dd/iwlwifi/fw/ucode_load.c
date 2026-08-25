/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Locate, read and parse the device's .ucode container.
 *
 * Linux gets the blob through request_firmware(); we read it straight off
 * the system volume.  Everything here runs at PASSIVE_LEVEL out of
 * MiniportInitializeEx.
 *
 * The container is held in PAGED pool: it is only ever touched at
 * PASSIVE_LEVEL, both here and by the (future) section-push path, which
 * also runs from init.  If that ever changes - a firmware reload from a
 * DPC, say - this allocation has to move to non-paged first.
 */

#include "../iwlwifi.h"

#define NDEBUG
#include <debug.h>

/* Where the CMake DOWNLOAD_IWLWIFI_UCODE option lands the pinned blobs. */
#define IWL_FW_DIRECTORY    L"\\SystemRoot\\System32\\drivers\\iwlwifi\\"

/*
 * A firmware container larger than this is not something we are going to
 * push to a wireless NIC; refuse rather than trusting a size field we read
 * off disk.  The largest blob shipping today is ~2.5 MB.
 */
#define IWL_FW_MAX_SIZE     (16u * 1024u * 1024u)

/*
 * Read one candidate file whole.  Returns NDIS_STATUS_FILE_NOT_FOUND when
 * the file simply is not there, which is the normal case while walking
 * down the API range and must stay distinguishable from a real failure.
 */
static NDIS_STATUS
IwlReadFirmwareFile(
    _In_ PCSTR FileName,
    _Outptr_result_maybenull_ PVOID *Buffer,
    _Out_ PULONG Length)
{
    WCHAR PathBuffer[128];
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    FILE_STANDARD_INFORMATION StandardInfo;
    HANDLE FileHandle = NULL;
    NTSTATUS Status;
    PVOID Data = NULL;
    ULONG Size;

    *Buffer = NULL;
    *Length = 0;

    Status = RtlStringCbPrintfW(PathBuffer,
                                sizeof(PathBuffer),
                                IWL_FW_DIRECTORY L"%hs",
                                FileName);
    if (!NT_SUCCESS(Status))
        return NDIS_STATUS_FAILURE;

    RtlInitUnicodeString(&Path, PathBuffer);
    InitializeObjectAttributes(&ObjectAttributes,
                               &Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&FileHandle,
                        GENERIC_READ | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatus,
                        FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
    {
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND ||
            Status == STATUS_OBJECT_PATH_NOT_FOUND)
        {
            return NDIS_STATUS_FILE_NOT_FOUND;
        }

        DPRINT1("iwlwifi: ZwOpenFile(%wZ) failed 0x%08x\n", &Path, Status);
        return NDIS_STATUS_FAILURE;
    }

    Status = ZwQueryInformationFile(FileHandle,
                                    &IoStatus,
                                    &StandardInfo,
                                    sizeof(StandardInfo),
                                    FileStandardInformation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iwlwifi: cannot size %wZ: 0x%08x\n", &Path, Status);
        ZwClose(FileHandle);
        return NDIS_STATUS_FAILURE;
    }

    if (StandardInfo.EndOfFile.HighPart != 0 ||
        StandardInfo.EndOfFile.LowPart == 0 ||
        StandardInfo.EndOfFile.LowPart > IWL_FW_MAX_SIZE)
    {
        DPRINT1("iwlwifi: %wZ has an implausible size 0x%I64x\n",
                &Path, StandardInfo.EndOfFile.QuadPart);
        ZwClose(FileHandle);
        return NDIS_STATUS_FAILURE;
    }

    Size = StandardInfo.EndOfFile.LowPart;

    Data = ExAllocatePoolWithTag(PagedPool, Size, IWL_TAG);
    if (Data == NULL)
    {
        DPRINT1("iwlwifi: out of pool for a %u byte firmware image\n", Size);
        ZwClose(FileHandle);
        return NDIS_STATUS_RESOURCES;
    }

    Status = ZwReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Data,
                        Size,
                        NULL,
                        NULL);
    ZwClose(FileHandle);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iwlwifi: read of %wZ failed 0x%08x\n", &Path, Status);
        ExFreePoolWithTag(Data, IWL_TAG);
        return NDIS_STATUS_FAILURE;
    }

    /* A short read means the file changed under us or the volume is
     * unhappy; either way the container is not what we sized. */
    if (IoStatus.Information != Size)
    {
        DPRINT1("iwlwifi: short read of %wZ: %Iu of %u bytes\n",
                &Path, IoStatus.Information, Size);
        ExFreePoolWithTag(Data, IWL_TAG);
        return NDIS_STATUS_FAILURE;
    }

    *Buffer = Data;
    *Length = Size;
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
IwlLoadFirmware(_In_ PIWL_ADAPTER Adapter)
{
    ULONG Api;
    BOOLEAN SawMalformed = FALSE;

    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    NT_ASSERT(Adapter->Cfg != NULL);

    Adapter->FwParsed = ExAllocatePoolWithTag(PagedPool,
                                              sizeof(IWL_FW_PARSED),
                                              IWL_TAG);
    if (Adapter->FwParsed == NULL)
        return NDIS_STATUS_RESOURCES;

    /*
     * Walk the API range from newest to oldest, exactly as
     * iwl_request_firmware() does.  The first one that both exists and
     * parses wins.
     */
    /* Counting down to an inclusive bound: test-then-decrement at the
     * bottom of the body rather than in the for-header, so a UcodeApiMin
     * of 0 terminates instead of wrapping the counter. */
    for (Api = Adapter->Cfg->UcodeApiMax; ; Api--)
    {
        CHAR Name[IWL_MAX_FW_NAME];
        PVOID Image;
        ULONG Length;
        NDIS_STATUS Status;
        IWL_FW_PARSE_STATUS ParseStatus;

        if (!IwlBuildFirmwareName(Adapter->Cfg, (UCHAR)Api, Name, sizeof(Name)))
            goto Next;

        Status = IwlReadFirmwareFile(Name, &Image, &Length);
        if (Status == NDIS_STATUS_FILE_NOT_FOUND)
            goto Next;
        if (Status != NDIS_STATUS_SUCCESS)
            goto Fail;

        ParseStatus = IwlParseUcodeFile(Image, Length, Adapter->FwParsed);
        if (ParseStatus != IwlFwParseOk)
        {
            /* The file is present but unusable.  Say so by name - a
             * truncated download and a genuinely absent file are very
             * different problems and must not read the same in the log -
             * then keep walking down the API range. */
            DPRINT1("iwlwifi: %s is present but will not parse: %s\n",
                    Name, IwlFwParseStatusName(ParseStatus));
            ExFreePoolWithTag(Image, IWL_TAG);
            SawMalformed = TRUE;
            goto Next;
        }

        Adapter->FwImage       = Image;
        Adapter->FwImageLength = Length;
        RtlStringCbCopyA(Adapter->FwName, sizeof(Adapter->FwName), Name);

        DPRINT1("iwlwifi: loaded %s (%u bytes): \"%s\" build 0x%08x, "
                "%u TLVs (%u unrecognised), %u CPU(s), %u scan channels\n",
                Name,
                Length,
                Adapter->FwParsed->HumanReadable,
                Adapter->FwParsed->Build,
                Adapter->FwParsed->TlvCount,
                Adapter->FwParsed->UnknownTlvCount,
                Adapter->FwParsed->NumOfCpus,
                Adapter->FwParsed->NScanChannels);
        DPRINT1("iwlwifi:   sections: regular=%u init=%u wowlan=%u\n",
                Adapter->FwParsed->Image[IWL_UCODE_REGULAR].SectionCount,
                Adapter->FwParsed->Image[IWL_UCODE_INIT].SectionCount,
                Adapter->FwParsed->Image[IWL_UCODE_WOWLAN].SectionCount);

        InterlockedOr(&Adapter->Flags, IWL_FLAG_FW_LOADED);
        return NDIS_STATUS_SUCCESS;

Next:
        if (Api == Adapter->Cfg->UcodeApiMin)
            break;
    }

    if (SawMalformed)
    {
        DPRINT1("iwlwifi: every candidate firmware for %s was malformed\n",
                Adapter->Cfg->Name);
    }
    else
    {
        CHAR Newest[IWL_MAX_FW_NAME];

        IwlBuildFirmwareName(Adapter->Cfg, Adapter->Cfg->UcodeApiMax,
                             Newest, sizeof(Newest));
        DPRINT1("iwlwifi: no firmware for %s in %S - looked for %s down to "
                "API %u.  Build with -DDOWNLOAD_IWLWIFI_UCODE=1, or drop the "
                "blob there by hand.\n",
                Adapter->Cfg->Name,
                IWL_FW_DIRECTORY,
                Newest,
                Adapter->Cfg->UcodeApiMin);
    }

Fail:
    ExFreePoolWithTag(Adapter->FwParsed, IWL_TAG);
    Adapter->FwParsed = NULL;
    return NDIS_STATUS_FAILURE;
}

VOID
IwlFreeFirmware(_In_ PIWL_ADAPTER Adapter)
{
    /* FwParsed's sections point into FwImage, so the two are released
     * together and FwParsed goes first. */
    if (Adapter->FwParsed != NULL)
    {
        ExFreePoolWithTag(Adapter->FwParsed, IWL_TAG);
        Adapter->FwParsed = NULL;
    }

    if (Adapter->FwImage != NULL)
    {
        ExFreePoolWithTag(Adapter->FwImage, IWL_TAG);
        Adapter->FwImage = NULL;
        Adapter->FwImageLength = 0;
    }

    InterlockedAnd(&Adapter->Flags, ~IWL_FLAG_FW_LOADED);
}

/* ------------------------------------------------------------------ */
/* Platform NVM                                                        */
/* ------------------------------------------------------------------ */

NDIS_STATUS
IwlLoadPnvm(_In_ PIWL_ADAPTER Adapter)
{
    PVOID Image;
    ULONG Length;
    NDIS_STATUS Status;
    IWL_FW_PARSE_STATUS ParseStatus;
    ULONG i;

    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    NT_ASSERT(Adapter->Cfg != NULL);
    /* Callers must not ask for a PNVM on a part that has none - the file
     * would not exist and the failure would read as a missing install. */
    NT_ASSERT(Adapter->Cfg->Flags & IWL_CFG_NEEDS_PNVM);

    if (!IwlBuildPnvmName(Adapter->Cfg, Adapter->PnvmName, sizeof(Adapter->PnvmName)))
        return NDIS_STATUS_FAILURE;

    Status = IwlReadFirmwareFile(Adapter->PnvmName, &Image, &Length);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        if (Status == NDIS_STATUS_FILE_NOT_FOUND)
        {
            DPRINT1("iwlwifi: %s is missing from %S.  AX210-class parts "
                    "cannot run without it.\n",
                    Adapter->PnvmName, IWL_FW_DIRECTORY);
        }
        return NDIS_STATUS_FAILURE;
    }

    Adapter->PnvmParsed = ExAllocatePoolWithTag(PagedPool,
                                                sizeof(IWL_PNVM_PARSED),
                                                IWL_TAG);
    if (Adapter->PnvmParsed == NULL)
    {
        ExFreePoolWithTag(Image, IWL_TAG);
        return NDIS_STATUS_RESOURCES;
    }

    ParseStatus = IwlParsePnvmFile(Image, Length, Adapter->PnvmParsed);
    if (ParseStatus != IwlFwParseOk)
    {
        DPRINT1("iwlwifi: %s will not parse: %s\n",
                Adapter->PnvmName, IwlFwParseStatusName(ParseStatus));
        ExFreePoolWithTag(Adapter->PnvmParsed, IWL_TAG);
        Adapter->PnvmParsed = NULL;
        ExFreePoolWithTag(Image, IWL_TAG);
        return NDIS_STATUS_FAILURE;
    }

    Adapter->PnvmImage       = Image;
    Adapter->PnvmImageLength = Length;

    DPRINT1("iwlwifi: loaded %s (%u bytes): %u SKU block(s)\n",
            Adapter->PnvmName, Length, Adapter->PnvmParsed->BlockCount);
    for (i = 0; i < Adapter->PnvmParsed->BlockCount; i++)
    {
        const IWL_PNVM_BLOCK *Block = &Adapter->PnvmParsed->Block[i];

        DPRINT1("iwlwifi:   sku %08x-%08x-%08x mac 0x%04x rf 0x%04x "
                "ver 0x%08x %u section(s) %u bytes\n",
                Block->SkuId[0], Block->SkuId[1], Block->SkuId[2],
                Block->MacType, Block->RfId, Block->Version,
                Block->SectionCount, Block->TotalDataLength);
    }

    /* Not a defensive check - a truncated block set means the block this
     * board needs may simply not be in the table, and the eventual
     * IwlPnvmSelectBlock() NULL would otherwise look like a bad blob. */
    if (Adapter->PnvmParsed->TruncatedBlockCount != 0)
    {
        DPRINT1("iwlwifi:   WARNING: %u further SKU block(s) were dropped; "
                "raise IWL_PNVM_MAX_BLOCKS\n",
                Adapter->PnvmParsed->TruncatedBlockCount);
    }

    InterlockedOr(&Adapter->Flags, IWL_FLAG_PNVM_LOADED);
    return NDIS_STATUS_SUCCESS;
}

VOID
IwlFreePnvm(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->PnvmParsed != NULL)
    {
        ExFreePoolWithTag(Adapter->PnvmParsed, IWL_TAG);
        Adapter->PnvmParsed = NULL;
    }

    if (Adapter->PnvmImage != NULL)
    {
        ExFreePoolWithTag(Adapter->PnvmImage, IWL_TAG);
        Adapter->PnvmImage = NULL;
        Adapter->PnvmImageLength = 0;
    }

    InterlockedAnd(&Adapter->Flags, ~IWL_FLAG_PNVM_LOADED);
}

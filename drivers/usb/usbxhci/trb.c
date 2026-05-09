/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         xHCI transfer TRB helpers
 */

#include "usbxhcip.h"

#define NDEBUG
#include <debug.h>

MPSTATUS
NTAPI
XHCI_BuildBulkNormalTrbs(IN PUSBPORT_SCATTER_GATHER_LIST SgList,
                         IN ULONG TransferLength,
                         OUT PXHCI_TRB Trbs,
                         IN ULONG MaxTrbs,
                         OUT PULONG TrbCount)
{
    ULONG SgIdx;
    ULONG TrbIdx;
    SIZE_T SubmittedLength;
    ULONG NonZeroCount;
    PHYSICAL_ADDRESS BufferPA;

    if (TrbCount == NULL)
        return MP_STATUS_FAILURE;

    *TrbCount = 0;

    if (Trbs == NULL || MaxTrbs == 0)
        return MP_STATUS_FAILURE;

    if (TransferLength == 0)
    {
        RtlZeroMemory(&Trbs[0], sizeof(Trbs[0]));
        Trbs[0].GenericTRB.Word2 = 0;
        Trbs[0].GenericTRB.Word3 = (NORMAL_TRB << 10) |
                                   (1 << 5) | /* IOC */
                                   1;         /* cycle bit is fixed by enqueue */
        *TrbCount = 1;
        return MP_STATUS_SUCCESS;
    }

    if (SgList == NULL || SgList->SgElementCount == 0)
        return MP_STATUS_FAILURE;

    SubmittedLength = 0;
    NonZeroCount = 0;

    for (SgIdx = 0; SgIdx < SgList->SgElementCount; SgIdx++)
    {
        ULONG ElementLength = SgList->SgElement[SgIdx].SgTransferLength;

        if (ElementLength == 0)
            continue;

        NonZeroCount++;
        SubmittedLength += ElementLength;
    }

    if (NonZeroCount == 0 ||
        NonZeroCount > MaxTrbs ||
        SubmittedLength != TransferLength)
    {
        return MP_STATUS_FAILURE;
    }

    TrbIdx = 0;

    for (SgIdx = 0; SgIdx < SgList->SgElementCount; SgIdx++)
    {
        ULONG ElementLength = SgList->SgElement[SgIdx].SgTransferLength;

        if (ElementLength == 0)
            continue;

        ASSERT(TrbIdx < NonZeroCount);
        ASSERT(TrbIdx < MaxTrbs);

        BufferPA = SgList->SgElement[SgIdx].SgPhysicalAddress;

        RtlZeroMemory(&Trbs[TrbIdx], sizeof(Trbs[TrbIdx]));
        Trbs[TrbIdx].GenericTRB.Word0 = (ULONG)(BufferPA.QuadPart & 0xFFFFFFFF);
        Trbs[TrbIdx].GenericTRB.Word1 = (ULONG)(BufferPA.QuadPart >> 32);
        Trbs[TrbIdx].GenericTRB.Word2 = ElementLength;
        Trbs[TrbIdx].GenericTRB.Word3 = (NORMAL_TRB << 10) |
                                        ((TrbIdx + 1 < NonZeroCount) ? (1 << 4) : 0) | /* CH */
                                        ((TrbIdx + 1 == NonZeroCount) ? (1 << 5) : 0) | /* IOC */
                                        1; /* cycle bit is fixed by enqueue */

        TrbIdx++;
    }

    ASSERT(TrbIdx == NonZeroCount);
    ASSERT(SubmittedLength == TransferLength);

    *TrbCount = TrbIdx;
    return MP_STATUS_SUCCESS;
}

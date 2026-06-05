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
    PHYSICAL_ADDRESS BufferPA;
    ULONG ElementLength;
    ULONG ChunkLength;
    ULONG BoundaryLength;
    ULONG LastLength;
    PHYSICAL_ADDRESS LastEndPA;

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

    RtlZeroMemory(Trbs, sizeof(Trbs[0]) * MaxTrbs);

    SubmittedLength = 0;
    TrbIdx = 0;

    for (SgIdx = 0; SgIdx < SgList->SgElementCount; SgIdx++)
    {
        BufferPA = SgList->SgElement[SgIdx].SgPhysicalAddress;
        ElementLength = SgList->SgElement[SgIdx].SgTransferLength;

        while (ElementLength != 0)
        {
            BoundaryLength = XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH -
                             (BufferPA.LowPart & (XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH - 1));
            ChunkLength = min(ElementLength, BoundaryLength);
            ChunkLength = min(ChunkLength, XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH);

            if (TrbIdx != 0)
            {
                LastLength = Trbs[TrbIdx - 1].GenericTRB.Word2;
                LastEndPA.LowPart = Trbs[TrbIdx - 1].GenericTRB.Word0;
                LastEndPA.HighPart = Trbs[TrbIdx - 1].GenericTRB.Word1;
                LastEndPA.QuadPart += LastLength;

                if (LastLength < XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH &&
                    LastEndPA.QuadPart == BufferPA.QuadPart &&
                    ((Trbs[TrbIdx - 1].GenericTRB.Word0 &
                      ~(XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH - 1)) ==
                     (BufferPA.LowPart &
                      ~(XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH - 1))))
                {
                    ULONG AppendLength;

                    AppendLength = min(ChunkLength,
                                       XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH - LastLength);
                    Trbs[TrbIdx - 1].GenericTRB.Word2 += AppendLength;
                    BufferPA.QuadPart += AppendLength;
                    ElementLength -= AppendLength;
                    SubmittedLength += AppendLength;
                    continue;
                }
            }

            if (TrbIdx >= MaxTrbs)
                return MP_STATUS_FAILURE;

            RtlZeroMemory(&Trbs[TrbIdx], sizeof(Trbs[TrbIdx]));
            Trbs[TrbIdx].GenericTRB.Word0 = (ULONG)(BufferPA.QuadPart & 0xFFFFFFFF);
            Trbs[TrbIdx].GenericTRB.Word1 = (ULONG)(BufferPA.QuadPart >> 32);
            Trbs[TrbIdx].GenericTRB.Word2 = ChunkLength;
            Trbs[TrbIdx].GenericTRB.Word3 = (NORMAL_TRB << 10) | 1;

            TrbIdx++;
            BufferPA.QuadPart += ChunkLength;
            ElementLength -= ChunkLength;
            SubmittedLength += ChunkLength;
        }
    }

    if (TrbIdx == 0 || SubmittedLength != TransferLength)
        return MP_STATUS_FAILURE;

    for (SgIdx = 0; SgIdx < TrbIdx; SgIdx++)
    {
        Trbs[SgIdx].GenericTRB.Word3 = (NORMAL_TRB << 10) |
                                        ((SgIdx + 1 < TrbIdx) ? (1 << 4) : 0) | /* CH */
                                        ((SgIdx + 1 == TrbIdx) ? (1 << 5) : 0) | /* IOC */
                                        1; /* cycle bit is fixed by enqueue */
    }

    ASSERT(SubmittedLength == TransferLength);

    *TrbCount = TrbIdx;
    return MP_STATUS_SUCCESS;
}

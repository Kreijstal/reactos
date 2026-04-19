/*
 * smb2rdr upcall bridge to the usermode smb2d daemon.
 *
 * Three moving parts:
 *   - SmbRdrIssueUpcall(): kernel producer.  Allocates an entry, enqueues
 *     it on gUpcallQueue, kicks gUpcallEvent, blocks on the per-entry
 *     Completed KEVENT.  The caller owns the entry's InBuffer lifetime
 *     for the duration of the call.
 *   - SmbRdrUpcallIoctl(): daemon IOCTL_SMB2RDR_READ handler.  Dequeues
 *     an entry from gUpcallQueue (blocking on gUpcallEvent if empty),
 *     reparks it on gInFlightList, and serialises the header + input
 *     payload into the daemon's output buffer.
 *   - SmbRdrDowncallIoctl(): daemon IOCTL_SMB2RDR_WRITE handler.
 *     Parses the { xid, status, outlen, <outlen bytes> } reply, finds
 *     the matching in-flight entry, copies back into its OutBuffer,
 *     and signals Completed so the producer wakes up.
 *
 * We intentionally never free the entry here: the producer waits on
 * Completed and then frees when the wait returns, because the entry is
 * stack-allocated on the producer's side.  That way there's exactly one
 * owner of the entry and no reference counting is needed.
 */

#define MINIRDR__NAME "smb2rdr"
#include <rx.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

#include "smb2rdr.h"
#include "upcall.h"

#define SMB2RDR_POOLTAG_UP  'pucS'    /* 'Scup' */
#define SMB2RDR_POOLTAG_IN  'nicS'    /* 'Scin' */
#define SMB2RDR_POOLTAG_OUT 'tucS'    /* 'Scot' */

static KSPIN_LOCK   gUpcallLock;
static LIST_ENTRY   gUpcallQueue;   /* WAIT_DEQUEUE entries */
static LIST_ENTRY   gInFlightList;  /* WAIT_REPLY entries */
static KEVENT       gUpcallEvent;   /* signalled when gUpcallQueue non-empty */
static volatile LONG64 gXidNext = 0;
static BOOLEAN      gUpcallInit = FALSE;

VOID
SmbRdrInitUpcall(VOID)
{
    if (gUpcallInit)
        return;

    KeInitializeSpinLock(&gUpcallLock);
    InitializeListHead(&gUpcallQueue);
    InitializeListHead(&gInFlightList);
    KeInitializeEvent(&gUpcallEvent, NotificationEvent, FALSE);
    gXidNext = 0;
    gUpcallInit = TRUE;

    DbgPrint("SMB2RDR: upcall bridge initialised\n");
}

VOID
SmbRdrShutdownUpcall(VOID)
{
    KIRQL irql;
    PLIST_ENTRY entry;

    if (!gUpcallInit)
        return;

    /* Wake every pending entry so producers can unwind and free. */
    KeAcquireSpinLock(&gUpcallLock, &irql);
    while (!IsListEmpty(&gUpcallQueue)) {
        PSMB2RDR_UPCALL_ENTRY up;
        entry = RemoveHeadList(&gUpcallQueue);
        up = CONTAINING_RECORD(entry, SMB2RDR_UPCALL_ENTRY, ListEntry);
        up->Status = STATUS_CANCELLED;
        up->State = SMB2RDR_UPCALL_ABANDONED;
        KeSetEvent(&up->Completed, 0, FALSE);
    }
    while (!IsListEmpty(&gInFlightList)) {
        PSMB2RDR_UPCALL_ENTRY up;
        entry = RemoveHeadList(&gInFlightList);
        up = CONTAINING_RECORD(entry, SMB2RDR_UPCALL_ENTRY, ListEntry);
        up->Status = STATUS_CANCELLED;
        up->State = SMB2RDR_UPCALL_ABANDONED;
        KeSetEvent(&up->Completed, 0, FALSE);
    }
    KeReleaseSpinLock(&gUpcallLock, irql);

    gUpcallInit = FALSE;
}

NTSTATUS
SmbRdrIssueUpcall(
    _In_ ULONG Opcode,
    _In_reads_bytes_opt_(InLength) PVOID InBuffer,
    _In_ ULONG InLength,
    _Out_writes_bytes_to_opt_(OutBufferLen, *OutActualLength) PVOID OutBuffer,
    _In_ ULONG OutBufferLen,
    _Out_ PULONG OutActualLength,
    _Out_ PNTSTATUS OutStatus,
    _In_ ULONG TimeoutSec)
{
    NTSTATUS status;
    SMB2RDR_UPCALL_ENTRY entry;
    LARGE_INTEGER timeout;
    KIRQL irql;

    if (OutActualLength != NULL)
        *OutActualLength = 0;
    if (OutStatus != NULL)
        *OutStatus = STATUS_UNSUCCESSFUL;

    if (!gUpcallInit)
        return STATUS_DEVICE_NOT_READY;

    RtlZeroMemory(&entry, sizeof(entry));
    InitializeListHead(&entry.ListEntry);
    entry.Xid       = InterlockedIncrement64(&gXidNext);
    entry.Opcode    = Opcode;
    entry.Status    = STATUS_PENDING;
    entry.State     = SMB2RDR_UPCALL_WAIT_DEQUEUE;
    entry.InBuffer  = InBuffer;
    entry.InLength  = InLength;
    entry.OutBuffer = OutBuffer;
    entry.OutLength = OutBufferLen;
    entry.OutActualLength = 0;
    KeInitializeEvent(&entry.Completed, NotificationEvent, FALSE);

    DbgPrint("SMB2RDR: enqueue upcall xid=%lld opcode=%lu inlen=%lu\n",
             entry.Xid, entry.Opcode, entry.InLength);

    KeAcquireSpinLock(&gUpcallLock, &irql);
    InsertTailList(&gUpcallQueue, &entry.ListEntry);
    KeSetEvent(&gUpcallEvent, 0, FALSE);
    KeReleaseSpinLock(&gUpcallLock, irql);

    /* Block waiting for the daemon. */
    timeout.QuadPart = -((LONGLONG)TimeoutSec * 10000000LL);
    status = KeWaitForSingleObject(&entry.Completed, Executive, KernelMode,
                                   FALSE, &timeout);

    if (status == STATUS_TIMEOUT) {
        /* Best-effort yank from whichever queue it still sits on.  If the
         * daemon raced us and already pulled it, the downcall path will
         * signal Completed; but since we've timed out, we have to treat
         * this as STATUS_IO_TIMEOUT and abandon the entry.  The daemon
         * discovering a zombie xid is harmless. */
        KeAcquireSpinLock(&gUpcallLock, &irql);
        if (entry.State == SMB2RDR_UPCALL_WAIT_DEQUEUE ||
            entry.State == SMB2RDR_UPCALL_WAIT_REPLY)
        {
            RemoveEntryList(&entry.ListEntry);
            entry.State = SMB2RDR_UPCALL_ABANDONED;
        }
        KeReleaseSpinLock(&gUpcallLock, irql);
        DbgPrint("SMB2RDR: upcall xid=%lld TIMED OUT after %lu s\n",
                 entry.Xid, TimeoutSec);
        return STATUS_IO_TIMEOUT;
    }

    DbgPrint("SMB2RDR: upcall xid=%lld woke with status=0x%08lx outlen=%lu\n",
             entry.Xid, entry.Status, entry.OutActualLength);

    if (OutStatus != NULL)
        *OutStatus = entry.Status;
    if (OutActualLength != NULL)
        *OutActualLength = entry.OutActualLength;

    return STATUS_SUCCESS;
}

NTSTATUS
SmbRdrUpcallIoctl(
    _In_ PRX_CONTEXT RxContext,
    _Out_ PULONG Information)
{
    NTSTATUS status;
    PLOWIO_CONTEXT LowIo = &RxContext->LowIoContext;
    ULONG outBufLen = LowIo->ParamsFor.IoCtl.OutputBufferLength;
    PUCHAR outBuf = LowIo->ParamsFor.IoCtl.pOutputBuffer;
    KIRQL irql;
    PLIST_ENTRY head;
    PSMB2RDR_UPCALL_ENTRY up;
    ULONG needed;

    *Information = 0;

    if (!gUpcallInit)
        return STATUS_DEVICE_NOT_READY;

    for (;;) {
        /* Look for a pending upcall; if none, wait for the event. */
        KeAcquireSpinLock(&gUpcallLock, &irql);
        if (IsListEmpty(&gUpcallQueue)) {
            /* Queue drained; reset the event so we actually block. */
            KeClearEvent(&gUpcallEvent);
            KeReleaseSpinLock(&gUpcallLock, irql);

            status = KeWaitForSingleObject(&gUpcallEvent, Executive,
                                           UserMode, TRUE, NULL);
            if (status == STATUS_USER_APC || status == STATUS_ALERTED) {
                /* Daemon got interrupted; let it retry. */
                return status;
            }
            continue;
        }

        head = RemoveHeadList(&gUpcallQueue);
        up = CONTAINING_RECORD(head, SMB2RDR_UPCALL_ENTRY, ListEntry);

        needed = (ULONG)sizeof(SMB2RDR_UPCALL_HEADER) + up->InLength;
        if (outBufLen < needed) {
            /* Re-queue the entry at the head so the daemon can retry
             * with a larger buffer; we intentionally don't touch state. */
            InsertHeadList(&gUpcallQueue, &up->ListEntry);
            KeSetEvent(&gUpcallEvent, 0, FALSE);
            KeReleaseSpinLock(&gUpcallLock, irql);
            DbgPrint("SMB2RDR: IOCTL_SMB2RDR_READ buffer too small: "
                     "have=%lu need=%lu\n", outBufLen, needed);
            return STATUS_BUFFER_TOO_SMALL;
        }

        /* Park the entry on the in-flight list so downcall can find it. */
        up->State = SMB2RDR_UPCALL_WAIT_REPLY;
        InsertTailList(&gInFlightList, &up->ListEntry);
        KeReleaseSpinLock(&gUpcallLock, irql);

        /* Serialise: header + payload. */
        {
            BOOLEAN copyFaulted = FALSE;
            _SEH2_TRY {
                SMB2RDR_UPCALL_HEADER hdr;
                hdr.Xid      = up->Xid;
                hdr.Opcode   = up->Opcode;
                hdr.InLength = up->InLength;
                RtlCopyMemory(outBuf, &hdr, sizeof(hdr));
                if (up->InLength && up->InBuffer)
                    RtlCopyMemory(outBuf + sizeof(hdr), up->InBuffer,
                                  up->InLength);
            } _SEH2_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
                copyFaulted = TRUE;
            } _SEH2_END;
            if (copyFaulted) {
                /* Daemon went away mid-copy.  Re-park on head of upcall
                 * queue and return failure so caller can retry next poll. */
                KeAcquireSpinLock(&gUpcallLock, &irql);
                RemoveEntryList(&up->ListEntry);
                up->State = SMB2RDR_UPCALL_WAIT_DEQUEUE;
                InsertHeadList(&gUpcallQueue, &up->ListEntry);
                KeSetEvent(&gUpcallEvent, 0, FALSE);
                KeReleaseSpinLock(&gUpcallLock, irql);
                return STATUS_INVALID_USER_BUFFER;
            }
        }

        *Information = needed;
        DbgPrint("SMB2RDR: IOCTL_SMB2RDR_READ served xid=%lld opcode=%lu "
                 "bytes=%lu\n", up->Xid, up->Opcode, needed);
        return STATUS_SUCCESS;
    }
}

NTSTATUS
SmbRdrDowncallIoctl(
    _In_ PRX_CONTEXT RxContext)
{
    PLOWIO_CONTEXT LowIo = &RxContext->LowIoContext;
    ULONG inBufLen = LowIo->ParamsFor.IoCtl.InputBufferLength;
    PUCHAR inBuf = LowIo->ParamsFor.IoCtl.pInputBuffer;
    SMB2RDR_DOWNCALL_HEADER hdr;
    KIRQL irql;
    PLIST_ENTRY le;
    PSMB2RDR_UPCALL_ENTRY up = NULL;
    BOOLEAN found = FALSE;
    ULONG toCopy;

    if (!gUpcallInit)
        return STATUS_DEVICE_NOT_READY;
    if (inBufLen < sizeof(hdr) || inBuf == NULL)
        return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(&hdr, inBuf, sizeof(hdr));
    if ((ULONG)(inBufLen - sizeof(hdr)) < hdr.OutLength)
        return STATUS_INVALID_PARAMETER;

    DbgPrint("SMB2RDR: IOCTL_SMB2RDR_WRITE received xid=%lld status=0x%08lx "
             "outlen=%lu\n", hdr.Xid, hdr.Status, hdr.OutLength);

    KeAcquireSpinLock(&gUpcallLock, &irql);
    for (le = gInFlightList.Flink; le != &gInFlightList; le = le->Flink) {
        PSMB2RDR_UPCALL_ENTRY cur =
            CONTAINING_RECORD(le, SMB2RDR_UPCALL_ENTRY, ListEntry);
        if (cur->Xid == hdr.Xid) {
            up = cur;
            found = TRUE;
            break;
        }
    }
    if (!found) {
        KeReleaseSpinLock(&gUpcallLock, irql);
        DbgPrint("SMB2RDR: IOCTL_SMB2RDR_WRITE no in-flight xid=%lld\n",
                 hdr.Xid);
        return STATUS_NOT_FOUND;
    }

    RemoveEntryList(&up->ListEntry);
    up->Status = hdr.Status;
    toCopy = hdr.OutLength;
    if (toCopy > up->OutLength)
        toCopy = up->OutLength;
    up->OutActualLength = toCopy;
    up->State = SMB2RDR_UPCALL_COMPLETED;
    KeReleaseSpinLock(&gUpcallLock, irql);

    if (toCopy && up->OutBuffer) {
        _SEH2_TRY {
            RtlCopyMemory(up->OutBuffer, inBuf + sizeof(hdr), toCopy);
        } _SEH2_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
            /* Treat a faulted daemon reply as a zero-length reply. */
            up->OutActualLength = 0;
        } _SEH2_END;
    }

    KeSetEvent(&up->Completed, 0, FALSE);
    return STATUS_SUCCESS;
}

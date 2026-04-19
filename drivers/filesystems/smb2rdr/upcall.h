/*
 * smb2rdr <-> smb2d upcall bridge.
 *
 * Kernel side enqueues an SMB2RDR_UPCALL_ENTRY, blocks on the per-entry
 * completion event, and a usermode daemon polls via IOCTL_SMB2RDR_READ /
 * replies via IOCTL_SMB2RDR_WRITE.  The wire format is deliberately simple
 * and fixed-layout so the kernel side doesn't drag in any marshalling
 * library: every upcall is { xid (LONGLONG), opcode (ULONG), inlen (ULONG),
 * <inlen bytes of Opcode-specific input> } and every downcall is
 * { xid, status, outlen, <outlen bytes of output> }.
 */

#ifndef _SMB2RDR_UPCALL_H_
#define _SMB2RDR_UPCALL_H_

/* Both kernel and usermode pull this header.  Kernel side includes the
 * entry struct + helpers; usermode only needs the on-the-wire layout. */
#if !defined(_KERNEL_MODE) && !defined(_NTDDK_) && !defined(_NTIFS_)
# include <windows.h>
#endif

/*
 * Opcode catalogue.  We allocate the whole P0 op set up-front so the wire
 * format doesn't have to change every time a new MRx callback is wired in.
 * Today only SMB2D_OP_CONNECT_SHARE actually fires; the rest are reserved.
 */
typedef enum _SMB2D_OPCODE {
    SMB2D_OP_INVALID         = 0,
    SMB2D_OP_CONNECT_SHARE   = 1,
    SMB2D_OP_CREATE          = 2,
    SMB2D_OP_READDIR         = 3,
    SMB2D_OP_QUERY_INFO      = 4,
    SMB2D_OP_READ            = 5,
    SMB2D_OP_CLOSE           = 6,
    SMB2D_OP_DISCONNECT      = 7,
    SMB2D_OP_MAX
} SMB2D_OPCODE;

#pragma pack(push, 8)

/* On-the-wire header for both the upcall buffer (kernel -> user) and the
 * downcall buffer (user -> kernel).  Both headers share the xid slot. */
typedef struct _SMB2RDR_UPCALL_HEADER {
    LONGLONG Xid;
    ULONG    Opcode;
    ULONG    InLength;      /* bytes of payload following this header */
} SMB2RDR_UPCALL_HEADER, *PSMB2RDR_UPCALL_HEADER;

typedef struct _SMB2RDR_DOWNCALL_HEADER {
    LONGLONG Xid;
    NTSTATUS Status;
    ULONG    OutLength;     /* bytes of payload following this header */
} SMB2RDR_DOWNCALL_HEADER, *PSMB2RDR_DOWNCALL_HEADER;

#pragma pack(pop)

#if defined(_NTDDK_) || defined(_NTIFS_) || defined(_KERNEL_MODE)

typedef enum _SMB2RDR_UPCALL_STATE {
    SMB2RDR_UPCALL_WAIT_DEQUEUE = 1,
    SMB2RDR_UPCALL_WAIT_REPLY   = 2,
    SMB2RDR_UPCALL_COMPLETED    = 3,
    SMB2RDR_UPCALL_ABANDONED    = 4,
} SMB2RDR_UPCALL_STATE;

typedef struct _SMB2RDR_UPCALL_ENTRY {
    LIST_ENTRY           ListEntry;
    LONGLONG             Xid;
    ULONG                Opcode;
    NTSTATUS             Status;
    SMB2RDR_UPCALL_STATE State;
    KEVENT               Completed;

    /* Input to the daemon (kernel-owned, copied into the READ output). */
    PVOID   InBuffer;
    ULONG   InLength;

    /* Output from the daemon (kernel-owned, filled by WRITE). */
    PVOID   OutBuffer;
    ULONG   OutLength;          /* capacity */
    ULONG   OutActualLength;    /* bytes actually written by daemon */
} SMB2RDR_UPCALL_ENTRY, *PSMB2RDR_UPCALL_ENTRY;

/* Lifecycle. */
VOID     SmbRdrInitUpcall(VOID);
VOID     SmbRdrShutdownUpcall(VOID);

/*
 * Synchronous upcall: pack InBuffer, enqueue, wait up to TimeoutSec,
 * fill *OutStatus and write up to OutBufferLen bytes of daemon-produced
 * reply into OutBuffer, returning the bytes actually written via
 * *OutActualLength.  Returns STATUS_SUCCESS if the round-trip completed
 * (regardless of the daemon-reported status), STATUS_IO_TIMEOUT on no
 * reply, STATUS_INSUFFICIENT_RESOURCES on OOM.
 */
NTSTATUS SmbRdrIssueUpcall(
    _In_ ULONG Opcode,
    _In_reads_bytes_opt_(InLength) PVOID InBuffer,
    _In_ ULONG InLength,
    _Out_writes_bytes_to_opt_(OutBufferLen, *OutActualLength) PVOID OutBuffer,
    _In_ ULONG OutBufferLen,
    _Out_ PULONG OutActualLength,
    _Out_ PNTSTATUS OutStatus,
    _In_ ULONG TimeoutSec);

/*
 * Daemon-facing IOCTL handlers.  Implemented in upcall.c; invoked from
 * the minirdr's MRxDevFcbXXXControlFile path in smb2rdr.c.  The RxContext
 * contains a LowIoContext.ParamsFor.IoCtl with input/output buffer info.
 */
struct _RX_CONTEXT;
NTSTATUS SmbRdrUpcallIoctl(
    _In_ struct _RX_CONTEXT *RxContext,
    _Out_ PULONG Information);
NTSTATUS SmbRdrDowncallIoctl(
    _In_ struct _RX_CONTEXT *RxContext);

#endif /* kernel mode */

#endif /* _SMB2RDR_UPCALL_H_ */

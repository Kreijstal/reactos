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
    SMB2D_OP_WRITE           = 8,
    SMB2D_OP_FSYNC           = 9,
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

/*
 * Per-opcode payload layouts for CREATE and CLOSE.  Paths are carried as
 * length-prefixed UTF-16LE with no NUL terminator; the daemon converts to
 * UTF-8 and rewrites backslashes into forward slashes for libsmb2.
 */
typedef struct _OP_CREATE_IN {
    ULONGLONG    VNetHandle;      /* ULONG64 from V_NET_ROOT_EXTENSION->DaemonHandle */
    ULONG        CreateOptions;   /* NT CreateOptions (FILE_DIRECTORY_FILE, ...) */
    ULONG        Disposition;     /* NT CreateDisposition (FILE_OPEN, ...) */
    ULONG        DesiredAccess;   /* NT ACCESS_MASK */
    ULONG        FileAttributes;
    ULONG        ShareAccess;
    USHORT       PathLen;         /* bytes, UTF-16LE, no NUL */
    USHORT       _Pad;
    /* PathLen bytes of UTF-16LE path immediately follow */
} OP_CREATE_IN, *POP_CREATE_IN;

typedef struct _OP_CREATE_OUT {
    ULONGLONG     FileHandle;     /* opaque daemon-assigned, non-zero on success */
    ULONG         Information;    /* NT FILE_OPENED / FILE_CREATED / ... */
    ULONG         IsDirectory;    /* 0 = file, 1 = directory */
    LARGE_INTEGER EndOfFile;      /* file size; zero for directories */
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes; /* server-reported attribute bits */
    ULONG         _Pad;
} OP_CREATE_OUT, *POP_CREATE_OUT;

typedef struct _OP_CLOSE_IN {
    ULONGLONG FileHandle;
    ULONG     IsDirectory;
    ULONG     _Pad;
} OP_CLOSE_IN, *POP_CLOSE_IN;
/* OP_CLOSE has no output payload (status-only downcall). */

/*
 * READDIR wire format.  Input carries the opaque directory handle (assigned
 * by OP_CREATE for a directory open), the NT file-information class the
 * caller wants entries encoded as, the maximum bytes the kernel can ingest,
 * scan-control flags, and an optional UTF-16LE filename filter.  Output is
 * a compact header followed by a flat packed run of NT dir-info records —
 * the daemon has already formatted them in the NT layout so the kernel side
 * can memcpy straight into the caller's buffer.
 */
typedef struct _OP_READDIR_IN {
    ULONGLONG FileHandle;
    ULONG     InfoClass;          /* FILE_INFORMATION_CLASS value */
    ULONG     MaxOutputLen;       /* bytes available after the OUT header */
    ULONG     RestartScan;        /* 1 = smb2_rewinddir() first */
    ULONG     ReturnSingleEntry;  /* 1 = stop after first entry */
    ULONG     FileIndex;          /* resume index, 0 typical */
    USHORT    FileSpecLen;        /* bytes of UTF-16LE filter; 0 = no filter */
    USHORT    _Pad;
    /* FileSpecLen bytes of UTF-16LE filter immediately follow */
} OP_READDIR_IN, *POP_READDIR_IN;

/* OUT header.  BytesWritten bytes of flat NT-format records follow.  Flags:
 *   0x1 = more entries remain (kernel side should report STATUS_SUCCESS on
 *         a partially-filled buffer, and the next call will continue); zero
 *         when the daemon walked the whole directory.  EntryCount == 0 with
 *         Flags == 0 means the iterator is exhausted.
 */
#define OP_READDIR_FLAG_MORE_ENTRIES 0x00000001u

typedef struct _OP_READDIR_OUT {
    ULONG BytesWritten;
    ULONG EntryCount;
    ULONG Flags;
    ULONG _Pad;
    /* BytesWritten bytes of packed NT dir-info records follow */
} OP_READDIR_OUT, *POP_READDIR_OUT;

/*
 * READ wire format.  Input carries the opaque daemon file handle (assigned
 * by OP_CREATE), the 64-bit byte offset to start reading at, and the byte
 * count requested in this chunk.  Output is a compact header whose
 * BytesRead says how many bytes of file data immediately follow; a short
 * read (BytesRead < Length) signals end-of-file.  The kernel side splits
 * large reads into 1 MiB chunks so the upcall staging buffer stays bounded.
 */
typedef struct _OP_READ_IN {
    ULONGLONG FileHandle;
    ULONGLONG Offset;
    ULONG     Length;
    ULONG     _Pad;
} OP_READ_IN, *POP_READ_IN;

typedef struct _OP_READ_OUT {
    ULONG BytesRead;        /* may be < Length on EOF */
    ULONG _Pad;
    /* BytesRead bytes of file data follow */
} OP_READ_OUT, *POP_READ_OUT;

/*
 * WRITE wire format.  Input carries the opaque daemon file handle, the
 * 64-bit byte offset to start writing at, and the byte count for this
 * chunk; Length bytes of caller-supplied payload follow the header inline.
 * Output is a compact header carrying the number of bytes the server
 * actually accepted, which may be < Length on a short write (in which
 * case the kernel side stops the chunk loop and surfaces a partial
 * success to the caller).  Kernel splits large writes into 1 MiB chunks
 * so the upcall staging buffers on both sides stay bounded.
 */
typedef struct _OP_WRITE_IN {
    ULONGLONG FileHandle;
    ULONGLONG Offset;
    ULONG     Length;
    ULONG     _Pad;
    /* Length bytes of payload follow immediately */
} OP_WRITE_IN, *POP_WRITE_IN;

typedef struct _OP_WRITE_OUT {
    ULONG BytesWritten;
    ULONG _Pad;
} OP_WRITE_OUT, *POP_WRITE_OUT;

/*
 * FSYNC wire format.  Input carries the opaque daemon file handle.  There
 * is no output payload — the downcall Status is the whole answer.  Fires
 * on MRxFlush (FlushFileBuffers / cache-manager flush paths).
 */
typedef struct _OP_FSYNC_IN {
    ULONGLONG FileHandle;
} OP_FSYNC_IN, *POP_FSYNC_IN;
/* OP_FSYNC has no output payload (status-only downcall). */

/*
 * QUERY_FILE_INFO wire format.  A single opcode delivers everything the
 * kernel needs to synthesize all NT file-info classes that cmd.exe copy
 * exercises; the kernel translates the daemon-reported stat into each
 * concrete FILE_* layout.  SMB2RDR_STAT mirrors libsmb2's smb2_stat_64
 * field-by-field so the daemon can do a flat copy, without forcing
 * <smb2/libsmb2.h> into the kernel include set.  Attributes are the
 * NT-style bitmap derived server-side from smb2_type (directory flag)
 * plus whatever FILE_ATTRIBUTE_* bits smb2d cares to stamp down today.
 */
typedef struct _SMB2RDR_STAT {
    ULONG     Type;            /* SMB2_TYPE_* (0=file, 1=dir, 2=link) */
    ULONG     NLink;
    ULONGLONG Ino;
    ULONGLONG Size;
    ULONGLONG Atime;           /* seconds since the POSIX epoch */
    ULONGLONG Atime_nsec;
    ULONGLONG Mtime;
    ULONGLONG Mtime_nsec;
    ULONGLONG Ctime;
    ULONGLONG Ctime_nsec;
    ULONGLONG Btime;
    ULONGLONG Btime_nsec;
    ULONG     Attributes;      /* NT FILE_ATTRIBUTE_* bits */
    ULONG     _Pad;
} SMB2RDR_STAT, *PSMB2RDR_STAT;

typedef struct _OP_QUERY_FILE_INFO_IN {
    ULONGLONG FileHandle;
} OP_QUERY_FILE_INFO_IN, *POP_QUERY_FILE_INFO_IN;

typedef struct _OP_QUERY_FILE_INFO_OUT {
    SMB2RDR_STAT Stat;
} OP_QUERY_FILE_INFO_OUT, *POP_QUERY_FILE_INFO_OUT;

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

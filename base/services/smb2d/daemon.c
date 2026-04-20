/*
 * smb2d daemon loop.  Opens \\.\smb2rdr, blocks on IOCTL_SMB2RDR_READ,
 * decodes the opcode, and calls into libsmb2 to actually bring up / tear
 * down SMB2 share connections.  The upcall wire format is simple and
 * fixed-layout (header = {xid, opcode, inlen}, body is opcode-specific),
 * so we deliberately avoid pulling any DDK types down here.
 *
 * Today we implement:
 *   SMB2D_OP_CONNECT_SHARE  - smb2_init_context + smb2_connect_share, on
 *                             success return a ULONG64 handle that the
 *                             kernel stashes on the MRX_V_NET_ROOT_EXTENSION.
 *   SMB2D_OP_DISCONNECT     - lookup handle -> smb2_disconnect_share +
 *                             smb2_destroy_context, drop table entry.
 *
 * The file/directory ops (SMB2D_OP_CREATE / READDIR / QUERY_INFO / READ /
 * CLOSE) are left as STATUS_NOT_IMPLEMENTED stubs for a follow-up slice.
 */

#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "smb2d.h"

/* libsmb2 uses POSIX open() flag values; if the CRT headers don't provide
 * them (e.g. ancient cross-compile toolchains) fall back to the Linux
 * numeric values libsmb2 itself was compiled against. */
#ifndef O_RDONLY
# define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
# define O_WRONLY 0x0001
#endif
#ifndef O_RDWR
# define O_RDWR   0x0002
#endif
#ifndef O_CREAT
# define O_CREAT  0x0040
#endif
#ifndef O_EXCL
# define O_EXCL   0x0080
#endif
#ifndef O_TRUNC
# define O_TRUNC  0x0200
#endif
#ifndef O_APPEND
# define O_APPEND 0x0400
#endif

/*
 * smb2d runs as a service where stdout/stderr have nowhere to go, so we
 * route all log lines through OutputDebugStringA — KD/DbgPrint is the
 * only sink COM1 observers can actually read in that context.
 */
static void
smb2d_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    OutputDebugStringA(buf);
}

/* We pull in only the IOCTLs we need from the driver header without
 * dragging in any DDK types.  Keep this in sync with drivers/filesystems/
 * smb2rdr/smb2rdr.h and upcall.h.  winioctl.h already defines
 * FILE_DEVICE_NETWORK_REDIRECTOR. */
#define SMB2RDR_USER_DEVICE_NAME_A     "\\\\.\\smb2rdr"
#define _RDR_CTL_CODE(code, method) \
    CTL_CODE(FILE_DEVICE_NETWORK_REDIRECTOR, 0x800 | (code), method, FILE_ANY_ACCESS)
#define IOCTL_SMB2RDR_READ  _RDR_CTL_CODE(6, METHOD_BUFFERED)
#define IOCTL_SMB2RDR_WRITE _RDR_CTL_CODE(7, METHOD_BUFFERED)

#pragma pack(push, 8)
typedef struct {
    LONGLONG Xid;
    ULONG    Opcode;
    ULONG    InLength;
} SMB2D_UPCALL_HEADER;

typedef struct {
    LONGLONG Xid;
    LONG     Status;        /* NTSTATUS */
    ULONG    OutLength;
} SMB2D_DOWNCALL_HEADER;

/* Must stay byte-identical to the kernel definitions in
 * drivers/filesystems/smb2rdr/upcall.h.  Kept inline rather than shared
 * because this daemon is deliberately DDK-free. */
typedef struct {
    ULONGLONG VNetHandle;
    ULONG     CreateOptions;
    ULONG     Disposition;
    ULONG     DesiredAccess;
    ULONG     FileAttributes;
    ULONG     ShareAccess;
    USHORT    PathLen;
    USHORT    _Pad;
} SMB2D_OP_CREATE_IN;

typedef struct {
    ULONGLONG     FileHandle;
    ULONG         Information;
    ULONG         IsDirectory;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         _Pad;
} SMB2D_OP_CREATE_OUT;

typedef struct {
    ULONGLONG FileHandle;
    ULONG     IsDirectory;
    ULONG     _Pad;
} SMB2D_OP_CLOSE_IN;

typedef struct {
    ULONGLONG FileHandle;
    ULONG     InfoClass;
    ULONG     MaxOutputLen;
    ULONG     RestartScan;
    ULONG     ReturnSingleEntry;
    ULONG     FileIndex;
    USHORT    FileSpecLen;
    USHORT    _Pad;
} SMB2D_OP_READDIR_IN;

typedef struct {
    ULONG BytesWritten;
    ULONG EntryCount;
    ULONG Flags;
    ULONG _Pad;
} SMB2D_OP_READDIR_OUT;

#define SMB2D_READDIR_FLAG_MORE 0x00000001u

/* Kernel→daemon READ wire layout, mirror of OP_READ_IN / OP_READ_OUT in
 * drivers/filesystems/smb2rdr/upcall.h.  Kept byte-identical; any layout
 * change on either side must be mirrored here. */
typedef struct {
    ULONGLONG FileHandle;
    ULONGLONG Offset;
    ULONG     Length;
    ULONG     _Pad;
} SMB2D_OP_READ_IN;

typedef struct {
    ULONG BytesRead;
    ULONG _Pad;
} SMB2D_OP_READ_OUT;

/* Kernel->daemon WRITE wire layout, mirror of OP_WRITE_IN / OP_WRITE_OUT
 * in drivers/filesystems/smb2rdr/upcall.h.  Length bytes of payload follow
 * the header inline in the upcall buffer. */
typedef struct {
    ULONGLONG FileHandle;
    ULONGLONG Offset;
    ULONG     Length;
    ULONG     _Pad;
} SMB2D_OP_WRITE_IN;

typedef struct {
    ULONG BytesWritten;
    ULONG _Pad;
} SMB2D_OP_WRITE_OUT;

/* Kernel->daemon FSYNC wire layout, mirror of OP_FSYNC_IN in
 * drivers/filesystems/smb2rdr/upcall.h.  No reply payload — the downcall
 * Status is the whole answer. */
typedef struct {
    ULONGLONG FileHandle;
} SMB2D_OP_FSYNC_IN;

/* NT dir-info record types (flat layout, matches FILE_*_DIR_INFORMATION in
 * ntifs.h / ndk/iotypes.h; we keep a local copy here so the daemon stays
 * free of DDK/NTIFS headers).  See sdk/include/ndk/iotypes.h. */
typedef struct {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    CCHAR         ShortNameLength;
    WCHAR         ShortName[12];
    WCHAR         FileName[1];
} SMB2D_FILE_BOTH_DIR_INFORMATION;

typedef struct {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    WCHAR         FileName[1];
} SMB2D_FILE_FULL_DIR_INFORMATION;

typedef struct {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    WCHAR         FileName[1];
} SMB2D_FILE_DIRECTORY_INFORMATION;

typedef struct {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    ULONG FileNameLength;
    WCHAR FileName[1];
} SMB2D_FILE_NAMES_INFORMATION;
#pragma pack(pop)

/* Matches SMB2D_OP_* in drivers/filesystems/smb2rdr/upcall.h. */
enum {
    SMB2D_OP_INVALID       = 0,
    SMB2D_OP_CONNECT_SHARE = 1,
    SMB2D_OP_CREATE        = 2,
    SMB2D_OP_READDIR       = 3,
    SMB2D_OP_QUERY_INFO    = 4,
    SMB2D_OP_READ          = 5,
    SMB2D_OP_CLOSE         = 6,
    SMB2D_OP_DISCONNECT    = 7,
    SMB2D_OP_WRITE         = 8,
    SMB2D_OP_FSYNC         = 9,
};

/* A small grab-bag of NTSTATUS values we need.  Most come in via winnt.h
 * when windows.h is included; we redefine the ones that aren't there as
 * LONG-typed constants to keep the daemon source self-contained without
 * dragging in NTSTATUS.h. */
#ifndef STATUS_NOT_IMPLEMENTED
# define STATUS_NOT_IMPLEMENTED        ((LONG)0xC0000002L)
#endif
#ifndef STATUS_BAD_NETWORK_NAME
# define STATUS_BAD_NETWORK_NAME       ((LONG)0xC00000CCL)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
# define STATUS_OBJECT_NAME_NOT_FOUND  ((LONG)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
# define STATUS_OBJECT_NAME_COLLISION  ((LONG)0xC0000035L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
# define STATUS_OBJECT_PATH_NOT_FOUND  ((LONG)0xC000003AL)
#endif
#ifndef STATUS_INVALID_HANDLE
# define STATUS_INVALID_HANDLE         ((LONG)0xC0000008L)
#endif
#ifndef STATUS_NOT_A_DIRECTORY
# define STATUS_NOT_A_DIRECTORY        ((LONG)0xC0000103L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
# define STATUS_FILE_IS_A_DIRECTORY    ((LONG)0xC00000BAL)
#endif
#ifndef STATUS_ACCESS_DENIED
# define STATUS_ACCESS_DENIED          ((LONG)0xC0000022L)
#endif
#ifndef STATUS_UNSUCCESSFUL
# define STATUS_UNSUCCESSFUL           ((LONG)0xC0000001L)
#endif
#ifndef STATUS_NO_MORE_FILES
# define STATUS_NO_MORE_FILES          ((LONG)0x80000006L)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
# define STATUS_BUFFER_OVERFLOW        ((LONG)0x80000005L)
#endif
#ifndef STATUS_INVALID_INFO_CLASS
# define STATUS_INVALID_INFO_CLASS     ((LONG)0xC0000003L)
#endif
#define SMB2D_STATUS_SUCCESS           ((LONG)0x00000000L)

/* FILE_INFORMATION_CLASS values we recognise. */
#define SMB2D_FileDirectoryInformation      1
#define SMB2D_FileFullDirectoryInformation  2
#define SMB2D_FileBothDirectoryInformation  3
#define SMB2D_FileNamesInformation          12

/* NT create-disposition / createoptions / file-attribute constants we need
 * on the daemon side.  winnt.h supplies all of these when built against the
 * SDK; we defensively redefine them in case the service build environment
 * omits anything. */
#ifndef FILE_SUPERSEDE
# define FILE_SUPERSEDE                0x00000000
#endif
#ifndef FILE_OPEN
# define FILE_OPEN                     0x00000001
#endif
#ifndef FILE_CREATE
# define FILE_CREATE                   0x00000002
#endif
#ifndef FILE_OPEN_IF
# define FILE_OPEN_IF                  0x00000003
#endif
#ifndef FILE_OVERWRITE
# define FILE_OVERWRITE                0x00000004
#endif
#ifndef FILE_OVERWRITE_IF
# define FILE_OVERWRITE_IF             0x00000005
#endif

#ifndef FILE_SUPERSEDED
# define FILE_SUPERSEDED               0x00000000
#endif
#ifndef FILE_OPENED
# define FILE_OPENED                   0x00000001
#endif
#ifndef FILE_CREATED
# define FILE_CREATED                  0x00000002
#endif
#ifndef FILE_OVERWRITTEN
# define FILE_OVERWRITTEN              0x00000003
#endif
#ifndef FILE_EXISTS
# define FILE_EXISTS                   0x00000004
#endif
#ifndef FILE_DOES_NOT_EXIST
# define FILE_DOES_NOT_EXIST           0x00000005
#endif

#ifndef FILE_DIRECTORY_FILE
# define FILE_DIRECTORY_FILE           0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
# define FILE_NON_DIRECTORY_FILE       0x00000040
#endif

#ifndef FILE_ATTRIBUTE_DIRECTORY
# define FILE_ATTRIBUTE_DIRECTORY      0x00000010
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
# define FILE_ATTRIBUTE_NORMAL         0x00000080
#endif
#ifndef FILE_ATTRIBUTE_ARCHIVE
# define FILE_ATTRIBUTE_ARCHIVE        0x00000020
#endif

#ifndef FILE_WRITE_DATA
# define FILE_WRITE_DATA               0x00000002
#endif
#ifndef FILE_APPEND_DATA
# define FILE_APPEND_DATA              0x00000004
#endif
#ifndef GENERIC_WRITE_A
# define GENERIC_WRITE_A               0x40000000
#endif

/* ---------- handle table: ULONG64 handle -> smb2_context * ---------- */

#define SMB2D_MAX_HANDLES 64

typedef struct {
    ULONG64              handle;      /* 0 = free slot */
    struct smb2_context *ctx;
} SMB2D_CONN;

static SMB2D_CONN       g_conns[SMB2D_MAX_HANDLES];
static CRITICAL_SECTION g_conn_lock;
static BOOL             g_conn_lock_init;
static ULONG64          g_next_handle = 1;
static BOOL             g_wsa_started;

static void
EnsureWsaStartup(void)
{
    WSADATA wd;
    if (!g_wsa_started) {
        if (WSAStartup(MAKEWORD(2, 2), &wd) == 0) {
            g_wsa_started = TRUE;
        } else {
            smb2d_log("smb2d: WSAStartup failed: %d\n",
                      WSAGetLastError());
        }
    }
}

static void
EnsureConnLock(void)
{
    if (!g_conn_lock_init) {
        InitializeCriticalSection(&g_conn_lock);
        g_conn_lock_init = TRUE;
    }
}

static ULONG64
InsertConn(struct smb2_context *ctx)
{
    int i;
    ULONG64 h = 0;

    EnterCriticalSection(&g_conn_lock);
    for (i = 0; i < SMB2D_MAX_HANDLES; i++) {
        if (g_conns[i].handle == 0) {
            h = g_next_handle++;
            if (g_next_handle == 0) g_next_handle = 1;
            g_conns[i].handle = h;
            g_conns[i].ctx    = ctx;
            break;
        }
    }
    LeaveCriticalSection(&g_conn_lock);
    return h;
}

static struct smb2_context *
RemoveConn(ULONG64 h)
{
    int i;
    struct smb2_context *ctx = NULL;

    if (h == 0) return NULL;
    EnterCriticalSection(&g_conn_lock);
    for (i = 0; i < SMB2D_MAX_HANDLES; i++) {
        if (g_conns[i].handle == h) {
            ctx = g_conns[i].ctx;
            g_conns[i].handle = 0;
            g_conns[i].ctx    = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_conn_lock);
    return ctx;
}

/* Non-owning lookup: returns the smb2_context * associated with the vnet
 * handle without removing it from the table.  Used by per-file ops that
 * leave the share connection intact across the upcall. */
static struct smb2_context *
LookupConn(ULONG64 h)
{
    int i;
    struct smb2_context *ctx = NULL;

    if (h == 0) return NULL;
    EnterCriticalSection(&g_conn_lock);
    for (i = 0; i < SMB2D_MAX_HANDLES; i++) {
        if (g_conns[i].handle == h) {
            ctx = g_conns[i].ctx;
            break;
        }
    }
    LeaveCriticalSection(&g_conn_lock);
    return ctx;
}

/* ---------- file/dir handle table: ULONG64 file handle -> (ctx, fh|dir, is_dir) ----- */

#define SMB2D_MAX_FILE_HANDLES 256

typedef struct {
    ULONG64              handle;       /* 0 = free slot */
    struct smb2_context *ctx;          /* borrowed reference from the conn table */
    void                *opaque;       /* smb2fh* if !is_dir, smb2dir* otherwise */
    int                  is_dir;
} SMB2D_FILE;

static SMB2D_FILE       g_files[SMB2D_MAX_FILE_HANDLES];
static CRITICAL_SECTION g_file_lock;
static BOOL             g_file_lock_init;
/* Start file handles at 0x100 to keep them visually distinct from the
 * per-share handles (which start at 1).  Both namespaces live in the same
 * ULONG64 but never collide because they go through separate tables. */
static ULONG64          g_next_file_handle = 0x100;

static void
EnsureFileLock(void)
{
    if (!g_file_lock_init) {
        InitializeCriticalSection(&g_file_lock);
        g_file_lock_init = TRUE;
    }
}

static ULONG64
InsertFile(struct smb2_context *ctx, void *opaque, int is_dir)
{
    int i;
    ULONG64 h = 0;

    EnsureFileLock();
    EnterCriticalSection(&g_file_lock);
    for (i = 0; i < SMB2D_MAX_FILE_HANDLES; i++) {
        if (g_files[i].handle == 0) {
            h = g_next_file_handle++;
            if (g_next_file_handle == 0) g_next_file_handle = 0x100;
            g_files[i].handle = h;
            g_files[i].ctx    = ctx;
            g_files[i].opaque = opaque;
            g_files[i].is_dir = is_dir;
            break;
        }
    }
    LeaveCriticalSection(&g_file_lock);
    return h;
}

/* Non-owning lookup: returns TRUE and fills in *ctx/opaque/is_dir when the
 * handle is known.  The kernel side holds a reference on the handle for the
 * lifetime of the FCB, so the returned pointers are safe to use for the
 * duration of the upcall. */
static BOOL
LookupFile(ULONG64 h, struct smb2_context **ctx_out,
           void **opaque_out, int *is_dir_out)
{
    int i;
    BOOL found = FALSE;

    EnsureFileLock();
    EnterCriticalSection(&g_file_lock);
    for (i = 0; i < SMB2D_MAX_FILE_HANDLES; i++) {
        if (g_files[i].handle == h) {
            if (ctx_out)    *ctx_out    = g_files[i].ctx;
            if (opaque_out) *opaque_out = g_files[i].opaque;
            if (is_dir_out) *is_dir_out = g_files[i].is_dir;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_file_lock);
    return found;
}

/* Remove-and-return a file-handle table entry.  Returns FALSE if the handle
 * wasn't known.  The caller is responsible for invoking smb2_close() /
 * smb2_closedir() on the returned opaque. */
static BOOL
RemoveFile(ULONG64 h, struct smb2_context **ctx_out,
           void **opaque_out, int *is_dir_out)
{
    int i;
    BOOL found = FALSE;

    EnsureFileLock();
    EnterCriticalSection(&g_file_lock);
    for (i = 0; i < SMB2D_MAX_FILE_HANDLES; i++) {
        if (g_files[i].handle == h) {
            *ctx_out    = g_files[i].ctx;
            *opaque_out = g_files[i].opaque;
            *is_dir_out = g_files[i].is_dir;
            g_files[i].handle = 0;
            g_files[i].ctx    = NULL;
            g_files[i].opaque = NULL;
            g_files[i].is_dir = 0;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_file_lock);
    return found;
}

/* ---------- small UTF-16 helpers ---------- */

/*
 * Convert a length-prefixed UTF-16LE block (as produced by the kernel side
 * of MRxCreateVNetRoot's payload packing) into a heap-allocated UTF-8 C
 * string.  wchars is the number of WCHARs, not bytes.  Caller HeapFrees.
 */
static char *
Utf16ToUtf8(const WCHAR *w, int wchars)
{
    int n;
    char *s;

    if (wchars <= 0) {
        s = (char *)HeapAlloc(GetProcessHeap(), 0, 1);
        if (s) s[0] = '\0';
        return s;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, wchars, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n + 1);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, wchars, s, n, NULL, NULL);
    s[n] = '\0';
    return s;
}

static void
Utf8Free(char *s)
{
    if (s) HeapFree(GetProcessHeap(), 0, s);
}

static const char *
OpcodeName(ULONG op)
{
    switch (op) {
    case SMB2D_OP_CONNECT_SHARE: return "CONNECT_SHARE";
    case SMB2D_OP_CREATE:        return "CREATE";
    case SMB2D_OP_READDIR:       return "READDIR";
    case SMB2D_OP_QUERY_INFO:    return "QUERY_INFO";
    case SMB2D_OP_READ:          return "READ";
    case SMB2D_OP_CLOSE:         return "CLOSE";
    case SMB2D_OP_DISCONNECT:    return "DISCONNECT";
    case SMB2D_OP_WRITE:         return "WRITE";
    case SMB2D_OP_FSYNC:         return "FSYNC";
    default:                     return "INVALID";
    }
}

static HANDLE
OpenSmbRdr(void)
{
    return CreateFileA(SMB2RDR_USER_DEVICE_NAME_A,
                       GENERIC_READ | GENERIC_WRITE,
                       0, NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL
SendDowncall(HANDLE h, LONGLONG xid, LONG status, const void *out, ULONG outLen)
{
    BYTE stack[4096];
    BYTE *buf = stack;
    DWORD br = 0;
    SMB2D_DOWNCALL_HEADER hdr;
    BOOL ok;
    ULONG total;
    BOOL heap = FALSE;

    total = (ULONG)sizeof(hdr) + outLen;
    if (total > sizeof(stack)) {
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, total);
        if (buf == NULL) {
            smb2d_log("smb2d: downcall alloc failed xid=%lld size=%lu\n",
                      xid, total);
            /* Degrade to a status-only reply so the kernel waiter unblocks. */
            hdr.Xid = xid;
            hdr.Status = (LONG)0xC0000017L; /* STATUS_NO_MEMORY */
            hdr.OutLength = 0;
            ok = DeviceIoControl(h, IOCTL_SMB2RDR_WRITE,
                                 &hdr, sizeof(hdr), NULL, 0, &br, NULL);
            return ok;
        }
        heap = TRUE;
    }

    hdr.Xid = xid;
    hdr.Status = status;
    hdr.OutLength = outLen;

    memcpy(buf, &hdr, sizeof(hdr));
    if (outLen && out)
        memcpy(buf + sizeof(hdr), out, outLen);

    ok = DeviceIoControl(h, IOCTL_SMB2RDR_WRITE,
                         buf, total,
                         NULL, 0, &br, NULL);
    if (!ok)
        smb2d_log("smb2d: downcall DeviceIoControl failed xid=%lld err=%lu\n",
                  xid, GetLastError());
    if (heap)
        HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

/* ---------- opcode handlers ---------- */

/*
 * Payload layout produced by smb2rdr_PackConnectShare:
 *   USHORT serverBytes; WCHAR server[serverBytes/2];
 *   USHORT shareBytes;  WCHAR share[shareBytes/2];
 */
static LONG
HandleConnectShare(const BYTE *in, ULONG inLen,
                   ULONG64 *outHandle)
{
    const BYTE *cursor = in;
    const BYTE *end = in + inLen;
    USHORT serverBytes, shareBytes;
    const WCHAR *serverW, *shareW;
    char *server = NULL, *share = NULL;
    struct smb2_context *ctx = NULL;
    ULONG64 handle;
    int rc;
    LONG status = STATUS_BAD_NETWORK_NAME;

    *outHandle = 0;

    if (inLen < 2 * sizeof(USHORT)) return STATUS_INVALID_PARAMETER;

    serverBytes = *(const USHORT *)cursor;
    cursor += sizeof(USHORT);
    if (cursor + serverBytes > end) return STATUS_INVALID_PARAMETER;
    serverW = (const WCHAR *)cursor;
    cursor += serverBytes;

    if (cursor + sizeof(USHORT) > end) return STATUS_INVALID_PARAMETER;
    shareBytes = *(const USHORT *)cursor;
    cursor += sizeof(USHORT);
    if (cursor + shareBytes > end) return STATUS_INVALID_PARAMETER;
    shareW = (const WCHAR *)cursor;

    server = Utf16ToUtf8(serverW, serverBytes / (int)sizeof(WCHAR));
    share  = Utf16ToUtf8(shareW,  shareBytes  / (int)sizeof(WCHAR));
    if (!server || !share) {
        status = (LONG)0xC0000017L; /* STATUS_NO_MEMORY */
        goto out;
    }

    smb2d_log("smb2d: OP_CONNECT_SHARE server=%s share=%s\n",
              server, share);

    EnsureWsaStartup();

    ctx = smb2_init_context();
    if (!ctx) {
        smb2d_log("smb2d: smb2_init_context returned NULL\n");
        status = (LONG)0xC0000017L; /* STATUS_NO_MEMORY */
        goto out;
    }

    /* Anonymous/guest connect.  Matches the native smb2np test path. */
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    rc = smb2_connect_share(ctx, server, share, NULL);
    smb2d_log("smb2d: smb2_connect_share(%s, %s) -> %d\n",
              server, share, rc);
    if (rc != 0) {
        smb2d_log("smb2d: smb2_connect_share failed: %s\n",
                  smb2_get_error(ctx));
        smb2_destroy_context(ctx);
        ctx = NULL;
        status = STATUS_BAD_NETWORK_NAME;
        goto out;
    }

    handle = InsertConn(ctx);
    if (handle == 0) {
        smb2d_log("smb2d: handle table full, disconnecting\n");
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        ctx = NULL;
        status = (LONG)0xC0000017L; /* STATUS_NO_MEMORY */
        goto out;
    }

    smb2d_log("smb2d: connect_share(%s, %s) -> ok handle=0x%llx\n",
              server, share, (unsigned long long)handle);

    *outHandle = handle;
    status = SMB2D_STATUS_SUCCESS;

out:
    Utf8Free(server);
    Utf8Free(share);
    return status;
}

/*
 * Converts an UTF-16LE WCHAR path (as shipped by smb2rdr) into a libsmb2-
 * friendly UTF-8 string.  Handles three path-munging quirks in one place:
 *   - an empty input or "\" becomes "" (libsmb2 treats the share root as
 *     the empty path),
 *   - every '\\' is rewritten to '/' for libsmb2's POSIX path syntax,
 *   - a leading slash (from a leading '\\' on the kernel side) is stripped
 *     because smb2_open() expects "dir/file", not "/dir/file".
 * The returned string is HeapFreed by the caller.  Returns NULL on OOM.
 */
static char *
MakeSmb2Path(const WCHAR *w, int wbytes)
{
    char *s;
    size_t i;
    size_t len;

    if (wbytes <= 0) {
        s = (char *)HeapAlloc(GetProcessHeap(), 0, 1);
        if (s) s[0] = '\0';
        return s;
    }

    s = Utf16ToUtf8(w, wbytes / (int)sizeof(WCHAR));
    if (!s) return NULL;

    for (i = 0; s[i]; i++) {
        if (s[i] == '\\') s[i] = '/';
    }
    len = strlen(s);
    if (len > 0 && s[0] == '/') {
        memmove(s, s + 1, len); /* len includes the NUL */
    }
    /* Drop any trailing slash; libsmb2 rejects "dir/" on opendir. */
    len = strlen(s);
    while (len > 0 && s[len - 1] == '/') {
        s[--len] = '\0';
    }
    return s;
}

/*
 * Map NT {CreateOptions, Disposition, DesiredAccess} to POSIX open() flags
 * for libsmb2's smb2_open().  Directories go through smb2_opendir() and
 * don't need POSIX flags; this helper is only used for file opens.
 */
static int
NtOpenToPosixFlags(ULONG createOptions, ULONG disposition, ULONG desiredAccess)
{
    int flags;
    BOOL needsWrite;

    UNREFERENCED_PARAMETER(createOptions);

    needsWrite = (desiredAccess &
                  (FILE_WRITE_DATA | FILE_APPEND_DATA |
                   GENERIC_WRITE)) != 0;

    switch (disposition) {
    case FILE_OPEN:
        flags = needsWrite ? O_RDWR : O_RDONLY;
        break;
    case FILE_CREATE:
        flags = O_CREAT | O_EXCL | O_RDWR;
        break;
    case FILE_OPEN_IF:
        flags = O_CREAT | O_RDWR;
        break;
    case FILE_OVERWRITE:
        flags = O_TRUNC | O_RDWR;
        break;
    case FILE_OVERWRITE_IF:
    case FILE_SUPERSEDE:
        flags = O_CREAT | O_TRUNC | O_RDWR;
        break;
    default:
        flags = needsWrite ? O_RDWR : O_RDONLY;
        break;
    }
    return flags;
}

static LONG
HandleOpCreate(const BYTE *in, ULONG inLen,
               SMB2D_OP_CREATE_OUT *out)
{
    SMB2D_OP_CREATE_IN hdr;
    const WCHAR *pathW;
    char *path = NULL;
    struct smb2_context *ctx;
    struct smb2_stat_64 st;
    BOOLEAN wantDir, wantFile;
    int is_dir;
    int rc;
    struct smb2fh *fh = NULL;
    struct smb2dir *dir = NULL;
    ULONG64 handle;
    LONG status = STATUS_UNSUCCESSFUL;

    memset(out, 0, sizeof(*out));

    if (inLen < sizeof(hdr)) return STATUS_INVALID_PARAMETER;
    memcpy(&hdr, in, sizeof(hdr));

    if ((ULONG)sizeof(hdr) + hdr.PathLen > inLen)
        return STATUS_INVALID_PARAMETER;
    pathW = (const WCHAR *)(in + sizeof(hdr));

    ctx = LookupConn(hdr.VNetHandle);
    if (ctx == NULL) {
        smb2d_log("smb2d: OP_CREATE unknown vnet handle=0x%llx\n",
                  (unsigned long long)hdr.VNetHandle);
        return STATUS_INVALID_HANDLE;
    }

    path = MakeSmb2Path(pathW, (int)hdr.PathLen);
    if (!path) return (LONG)0xC0000017L; /* STATUS_NO_MEMORY */

    smb2d_log("smb2d: OP_CREATE vnet=0x%llx path=\"%s\" options=0x%lx "
              "disp=%lu access=0x%lx\n",
              (unsigned long long)hdr.VNetHandle, path,
              hdr.CreateOptions, hdr.Disposition, hdr.DesiredAccess);

    wantDir  = (hdr.CreateOptions & FILE_DIRECTORY_FILE)     != 0;
    wantFile = (hdr.CreateOptions & FILE_NON_DIRECTORY_FILE) != 0;

    /* Decide whether the target is a file or directory.  The empty path
     * is always the share root (directory).  Otherwise we honour the
     * NT hints, falling back to smb2_stat() to disambiguate when neither
     * FILE_DIRECTORY_FILE nor FILE_NON_DIRECTORY_FILE is set. */
    if (path[0] == '\0') {
        is_dir = 1;
    } else if (wantDir && !wantFile) {
        is_dir = 1;
    } else if (wantFile && !wantDir) {
        is_dir = 0;
    } else {
        memset(&st, 0, sizeof(st));
        rc = smb2_stat(ctx, path, &st);
        if (rc == 0) {
            is_dir = (st.smb2_type == SMB2_TYPE_DIRECTORY) ? 1 : 0;
        } else {
            /* stat failed; fall through to treat as file if caller wanted
             * to create/supersede, otherwise report not-found. */
            smb2d_log("smb2d: smb2_stat(\"%s\") failed: %s\n",
                      path, smb2_get_error(ctx));
            if (hdr.Disposition == FILE_CREATE ||
                hdr.Disposition == FILE_OPEN_IF ||
                hdr.Disposition == FILE_OVERWRITE_IF ||
                hdr.Disposition == FILE_SUPERSEDE) {
                is_dir = 0;
            } else {
                status = STATUS_OBJECT_NAME_NOT_FOUND;
                goto out;
            }
        }
    }

    if (is_dir) {
        dir = smb2_opendir(ctx, path);
        if (dir == NULL) {
            smb2d_log("smb2d: smb2_opendir(\"%s\") failed: %s\n",
                      path, smb2_get_error(ctx));
            /* Path-not-found is the most useful kernel-side hint. */
            status = (path[0] == '\0') ? STATUS_BAD_NETWORK_NAME
                                       : STATUS_OBJECT_NAME_NOT_FOUND;
            goto out;
        }
        handle = InsertFile(ctx, dir, 1);
        if (handle == 0) {
            smb2_closedir(ctx, dir);
            status = (LONG)0xC0000017L; /* STATUS_NO_MEMORY */
            goto out;
        }
        smb2d_log("smb2d: smb2_opendir(\"%s\") -> dir=%p\n",
                  path, (void *)dir);
        out->FileHandle  = handle;
        out->Information = FILE_OPENED;
        out->IsDirectory = 1;
        out->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        smb2d_log("smb2d: create_file ok handle=0x%llx\n",
                  (unsigned long long)handle);
    } else {
        int posixFlags;

        posixFlags = NtOpenToPosixFlags(hdr.CreateOptions,
                                         hdr.Disposition,
                                         hdr.DesiredAccess);
        fh = smb2_open(ctx, path, posixFlags);
        if (fh == NULL) {
            smb2d_log("smb2d: smb2_open(\"%s\", 0x%x) failed: %s\n",
                      path, posixFlags, smb2_get_error(ctx));
            status = STATUS_OBJECT_NAME_NOT_FOUND;
            goto out;
        }
        handle = InsertFile(ctx, fh, 0);
        if (handle == 0) {
            smb2_close(ctx, fh);
            status = (LONG)0xC0000017L; /* STATUS_NO_MEMORY */
            goto out;
        }
        smb2d_log("smb2d: smb2_open(\"%s\", 0x%x) -> fh=%p\n",
                  path, posixFlags, (void *)fh);

        /* Fill in whatever file metadata we can cheaply get from fstat.
         * QueryFileInfo (next slice) can refine; for CreateFile() to
         * return a usable handle we only need Information to be
         * non-zero. */
        memset(&st, 0, sizeof(st));
        if (smb2_fstat(ctx, fh, &st) == 0) {
            out->EndOfFile.QuadPart      = (LONGLONG)st.smb2_size;
            out->AllocationSize.QuadPart = (LONGLONG)st.smb2_size;
        }
        out->FileHandle   = handle;
        out->IsDirectory  = 0;
        out->FileAttributes = FILE_ATTRIBUTE_NORMAL;

        switch (hdr.Disposition) {
        case FILE_CREATE:           out->Information = FILE_CREATED;      break;
        case FILE_SUPERSEDE:        out->Information = FILE_SUPERSEDED;   break;
        case FILE_OVERWRITE:
        case FILE_OVERWRITE_IF:     out->Information = FILE_OVERWRITTEN;  break;
        case FILE_OPEN:
        case FILE_OPEN_IF:
        default:                    out->Information = FILE_OPENED;       break;
        }
        smb2d_log("smb2d: create_file ok handle=0x%llx\n",
                  (unsigned long long)handle);
    }

    status = SMB2D_STATUS_SUCCESS;

out:
    Utf8Free(path);
    return status;
}

static LONG
HandleOpClose(const BYTE *in, ULONG inLen)
{
    SMB2D_OP_CLOSE_IN hdr;
    struct smb2_context *ctx = NULL;
    void *opaque = NULL;
    int is_dir = 0;

    if (inLen < sizeof(hdr)) return STATUS_INVALID_PARAMETER;
    memcpy(&hdr, in, sizeof(hdr));

    smb2d_log("smb2d: OP_CLOSE handle=0x%llx\n",
              (unsigned long long)hdr.FileHandle);

    if (!RemoveFile(hdr.FileHandle, &ctx, &opaque, &is_dir)) {
        smb2d_log("smb2d: OP_CLOSE unknown handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_INVALID_HANDLE;
    }

    if (is_dir) {
        smb2_closedir(ctx, (struct smb2dir *)opaque);
        smb2d_log("smb2d: smb2_closedir ok\n");
    } else {
        smb2_close(ctx, (struct smb2fh *)opaque);
        smb2d_log("smb2d: smb2_close ok\n");
    }
    return SMB2D_STATUS_SUCCESS;
}

/*
 * Windows FILETIME is 100-ns ticks since 1601-01-01; libsmb2 delivers
 * seconds + nanoseconds since the POSIX epoch.  Bump by the 369-year gap
 * and scale to 100-ns units.  A zero POSIX timestamp maps to the 1601
 * epoch origin here, which is exactly what NT reports for "never set".
 */
static ULONG64
Smb2TimeToFileTime(uint64_t sec, uint64_t nsec)
{
    return (sec + 11644473600ULL) * 10000000ULL + nsec / 100ULL;
}

/*
 * Encode one libsmb2 dir entry into an NT FILE_*_DIR_INFORMATION record at
 * *cursor (pointing inside a flat buffer of total capacity capacity bytes).
 * Returns the on-the-wire size written on success, 0 when the record
 * wouldn't fit.  nameUtf16/nameWchars must already hold the encoded name.
 *
 * The caller is responsible for computing the 8-byte-aligned next offset;
 * this helper writes NextEntryOffset = 0 and lets the loop patch it up
 * before emitting the following entry.
 */
static ULONG
EncodeDirEntry(ULONG infoClass,
               BYTE *base, ULONG offset, ULONG capacity,
               const struct smb2dirent *e,
               const WCHAR *nameUtf16, int nameWchars)
{
    ULONG nameBytes = (ULONG)nameWchars * (ULONG)sizeof(WCHAR);
    ULONG recBase;
    ULONG recTotal;
    ULONG attrs;
    ULONG64 cre, acc, wri, chg;
    LONGLONG size;
    BYTE *rec;

    attrs = (e->st.smb2_type == SMB2_TYPE_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY
                                                     : FILE_ATTRIBUTE_NORMAL;
    size = (LONGLONG)e->st.smb2_size;
    cre  = Smb2TimeToFileTime(e->st.smb2_btime, e->st.smb2_btime_nsec);
    acc  = Smb2TimeToFileTime(e->st.smb2_atime, e->st.smb2_atime_nsec);
    wri  = Smb2TimeToFileTime(e->st.smb2_mtime, e->st.smb2_mtime_nsec);
    chg  = Smb2TimeToFileTime(e->st.smb2_ctime, e->st.smb2_ctime_nsec);

    switch (infoClass) {
    case SMB2D_FileBothDirectoryInformation:
        recBase = (ULONG)(FIELD_OFFSET(SMB2D_FILE_BOTH_DIR_INFORMATION, FileName));
        break;
    case SMB2D_FileFullDirectoryInformation:
        recBase = (ULONG)(FIELD_OFFSET(SMB2D_FILE_FULL_DIR_INFORMATION, FileName));
        break;
    case SMB2D_FileDirectoryInformation:
        recBase = (ULONG)(FIELD_OFFSET(SMB2D_FILE_DIRECTORY_INFORMATION, FileName));
        break;
    case SMB2D_FileNamesInformation:
        recBase = (ULONG)(FIELD_OFFSET(SMB2D_FILE_NAMES_INFORMATION, FileName));
        break;
    default:
        return 0;
    }

    recTotal = recBase + nameBytes;
    /* 8-byte alignment for NextEntryOffset chaining. */
    recTotal = (recTotal + 7u) & ~7u;

    if ((ULONG64)offset + recTotal > capacity)
        return 0;

    rec = base + offset;
    memset(rec, 0, recTotal);

    switch (infoClass) {
    case SMB2D_FileBothDirectoryInformation: {
        SMB2D_FILE_BOTH_DIR_INFORMATION *r = (SMB2D_FILE_BOTH_DIR_INFORMATION *)rec;
        r->NextEntryOffset      = 0;
        r->FileIndex            = 0;
        r->CreationTime.QuadPart   = (LONGLONG)cre;
        r->LastAccessTime.QuadPart = (LONGLONG)acc;
        r->LastWriteTime.QuadPart  = (LONGLONG)wri;
        r->ChangeTime.QuadPart     = (LONGLONG)chg;
        r->EndOfFile.QuadPart      = size;
        r->AllocationSize.QuadPart = size;
        r->FileAttributes       = attrs;
        r->FileNameLength       = nameBytes;
        r->EaSize               = 0;
        r->ShortNameLength      = 0;
        /* ShortName left zeroed. */
        if (nameBytes)
            memcpy(r->FileName, nameUtf16, nameBytes);
        break;
    }
    case SMB2D_FileFullDirectoryInformation: {
        SMB2D_FILE_FULL_DIR_INFORMATION *r = (SMB2D_FILE_FULL_DIR_INFORMATION *)rec;
        r->NextEntryOffset      = 0;
        r->FileIndex            = 0;
        r->CreationTime.QuadPart   = (LONGLONG)cre;
        r->LastAccessTime.QuadPart = (LONGLONG)acc;
        r->LastWriteTime.QuadPart  = (LONGLONG)wri;
        r->ChangeTime.QuadPart     = (LONGLONG)chg;
        r->EndOfFile.QuadPart      = size;
        r->AllocationSize.QuadPart = size;
        r->FileAttributes       = attrs;
        r->FileNameLength       = nameBytes;
        r->EaSize               = 0;
        if (nameBytes)
            memcpy(r->FileName, nameUtf16, nameBytes);
        break;
    }
    case SMB2D_FileDirectoryInformation: {
        SMB2D_FILE_DIRECTORY_INFORMATION *r = (SMB2D_FILE_DIRECTORY_INFORMATION *)rec;
        r->NextEntryOffset      = 0;
        r->FileIndex            = 0;
        r->CreationTime.QuadPart   = (LONGLONG)cre;
        r->LastAccessTime.QuadPart = (LONGLONG)acc;
        r->LastWriteTime.QuadPart  = (LONGLONG)wri;
        r->ChangeTime.QuadPart     = (LONGLONG)chg;
        r->EndOfFile.QuadPart      = size;
        r->AllocationSize.QuadPart = size;
        r->FileAttributes       = attrs;
        r->FileNameLength       = nameBytes;
        if (nameBytes)
            memcpy(r->FileName, nameUtf16, nameBytes);
        break;
    }
    case SMB2D_FileNamesInformation: {
        SMB2D_FILE_NAMES_INFORMATION *r = (SMB2D_FILE_NAMES_INFORMATION *)rec;
        r->NextEntryOffset      = 0;
        r->FileIndex            = 0;
        r->FileNameLength       = nameBytes;
        if (nameBytes)
            memcpy(r->FileName, nameUtf16, nameBytes);
        break;
    }
    }

    return recTotal;
}

/*
 * Patch the NextEntryOffset of the record that ends at offset+0 to reach
 * the record at nextOffset.  All four info classes keep NextEntryOffset
 * as the first ULONG in the record, so we can do this header-less.
 */
static void
PatchNextOffset(BYTE *base, ULONG recOffset, ULONG delta)
{
    ULONG *p = (ULONG *)(base + recOffset);
    *p = delta;
}

/*
 * OP_READDIR: walk smb2_readdir() and lay down NT dir-info records into a
 * staging buffer.  The kernel side hands us MaxOutputLen bytes of capacity
 * and we keep emitting until we either run out of room, the iterator is
 * drained, or ReturnSingleEntry says "stop after one".
 *
 * Paging semantics: the smb2dir iterator position persists inside libsmb2
 * across calls on the same FCB, so a follow-up OP_READDIR with RestartScan=0
 * continues from where we left off.  If a record doesn't fit into the
 * buffer, we re-queue nothing — libsmb2 has already advanced past that
 * entry, so it'll come back on the next call.  Rather than trying to
 * peek/rewind one entry, we simply defer: the NT semantics of dir queries
 * is that each call returns as many whole records as fit.
 *
 * Caller owns outBuf (capacity outCap, which must include the
 * SMB2D_OP_READDIR_OUT header) and on success *outWritten is the total
 * number of bytes to ship to the kernel (header + payload).
 */
static LONG
HandleOpReaddir(const BYTE *in, ULONG inLen,
                BYTE *outBuf, ULONG outCap, ULONG *outWritten)
{
    SMB2D_OP_READDIR_IN hdr;
    struct smb2_context *ctx = NULL;
    void *opaque = NULL;
    int is_dir = 0;
    struct smb2dir *dir;
    SMB2D_OP_READDIR_OUT *oh;
    BYTE *payload;
    ULONG payloadCap;
    ULONG bytesWritten = 0;
    ULONG entryCount = 0;
    ULONG lastRecOffset = 0;
    ULONG lastRecTotal = 0;
    BOOL haveLast = FALSE;
    BOOL more = FALSE;
    ULONG totalReply;

    *outWritten = 0;

    if (inLen < sizeof(hdr)) return STATUS_INVALID_PARAMETER;
    if (outCap < sizeof(SMB2D_OP_READDIR_OUT)) return STATUS_INVALID_PARAMETER;
    memcpy(&hdr, in, sizeof(hdr));

    smb2d_log("smb2d: OP_READDIR fh=0x%llx class=%lu maxout=%lu restart=%lu "
              "single=%lu\n",
              (unsigned long long)hdr.FileHandle, hdr.InfoClass,
              hdr.MaxOutputLen, hdr.RestartScan, hdr.ReturnSingleEntry);

    if (hdr.InfoClass != SMB2D_FileBothDirectoryInformation &&
        hdr.InfoClass != SMB2D_FileFullDirectoryInformation &&
        hdr.InfoClass != SMB2D_FileDirectoryInformation &&
        hdr.InfoClass != SMB2D_FileNamesInformation)
    {
        smb2d_log("smb2d: OP_READDIR unsupported info class %lu\n",
                  hdr.InfoClass);
        return STATUS_INVALID_INFO_CLASS;
    }

    if (!LookupFile(hdr.FileHandle, &ctx, &opaque, &is_dir) ||
        ctx == NULL || opaque == NULL)
    {
        smb2d_log("smb2d: OP_READDIR unknown handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_INVALID_HANDLE;
    }
    if (!is_dir) {
        smb2d_log("smb2d: OP_READDIR not a directory handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_NOT_A_DIRECTORY;
    }
    dir = (struct smb2dir *)opaque;

    if (hdr.RestartScan) {
        smb2_rewinddir(ctx, dir);
    }

    /* Clamp the caller's declared max against the actual staging capacity —
     * the kernel side has MaxOutputLen == userbuffer_size and sized outBuf
     * to accommodate it, but defend against a bogus header. */
    payloadCap = outCap - (ULONG)sizeof(SMB2D_OP_READDIR_OUT);
    if (hdr.MaxOutputLen < payloadCap)
        payloadCap = hdr.MaxOutputLen;

    oh      = (SMB2D_OP_READDIR_OUT *)outBuf;
    payload = outBuf + sizeof(SMB2D_OP_READDIR_OUT);
    memset(oh, 0, sizeof(*oh));

    for (;;) {
        struct smb2dirent *e;
        int wlen;
        WCHAR *wname;
        ULONG wnameBytes;
        ULONG recTotal;

        e = smb2_readdir(ctx, dir);
        if (e == NULL) {
            /* End of directory. */
            more = FALSE;
            break;
        }

        if (e->name == NULL || e->name[0] == '\0')
            continue;

        /* Convert the UTF-8 name to UTF-16LE in a stack-sized scratch
         * buffer; SMB paths are capped at 255 WCHARs on the wire so 512
         * WCHARs is a generous upper bound that never overflows. */
        {
            static WCHAR scratch[520];
            wlen = MultiByteToWideChar(CP_UTF8, 0, e->name, -1,
                                        scratch, (int)(sizeof(scratch)/sizeof(WCHAR)));
            if (wlen <= 1) {
                /* Conversion failed or returned only the NUL — skip the
                 * entry rather than emit a zero-length name. */
                smb2d_log("smb2d: readdir name conversion failed for \"%s\"\n",
                          e->name ? e->name : "(null)");
                continue;
            }
            /* MultiByteToWideChar with -1 includes the NUL in the return;
             * the NT record carries an explicit length so strip it. */
            wlen -= 1;
            wname = scratch;
            wnameBytes = (ULONG)wlen * (ULONG)sizeof(WCHAR);
            (void)wnameBytes;
        }

        recTotal = EncodeDirEntry(hdr.InfoClass, payload, bytesWritten,
                                  payloadCap, e, wname, wlen);
        if (recTotal == 0) {
            /* Out of buffer space.  libsmb2 has already advanced past this
             * entry, so we lose it for this iteration; the NT contract only
             * promises partial fills via STATUS_BUFFER_OVERFLOW/SUCCESS on
             * the partial page.  Mark more=TRUE and stop. */
            more = TRUE;
            break;
        }

        /* Patch the previously-emitted record so NT callers can walk the
         * chain forwards.  Each NextEntryOffset is the distance from that
         * record's start to the next one. */
        if (haveLast) {
            PatchNextOffset(payload, lastRecOffset, lastRecTotal);
        }

        lastRecOffset = bytesWritten;
        lastRecTotal  = recTotal;
        haveLast      = TRUE;
        bytesWritten += recTotal;
        entryCount++;

        smb2d_log("smb2d: readdir entry \"%s\" attr=0x%lx size=%llu\n",
                  e->name,
                  (e->st.smb2_type == SMB2_TYPE_DIRECTORY)
                      ? (ULONG)FILE_ATTRIBUTE_DIRECTORY
                      : (ULONG)FILE_ATTRIBUTE_NORMAL,
                  (unsigned long long)e->st.smb2_size);

        if (hdr.ReturnSingleEntry) {
            /* Single-entry contract: emit exactly one record and stop, but
             * leave `more` unset — the iterator position decides whether
             * the next call returns anything. */
            more = FALSE;
            break;
        }
    }

    /* Final record terminates the chain with NextEntryOffset == 0 (already
     * zeroed when we wrote the record, so nothing to patch). */

    oh->BytesWritten = bytesWritten;
    oh->EntryCount   = entryCount;
    oh->Flags        = more ? SMB2D_READDIR_FLAG_MORE : 0u;
    oh->_Pad         = 0;

    totalReply = (ULONG)sizeof(SMB2D_OP_READDIR_OUT) + bytesWritten;
    *outWritten = totalReply;

    smb2d_log("smb2d: OP_READDIR wrote %lu entries, bytes=%lu, more=%u\n",
              entryCount, bytesWritten, (unsigned)more);

    if (entryCount == 0) {
        /* No entries produced and end-of-dir reached — surface the NT
         * terminating status so rdbss completes the IRP cleanly. */
        return STATUS_NO_MORE_FILES;
    }

    return SMB2D_STATUS_SUCCESS;
}

/*
 * OP_READ: translate a kernel-side OP_READ upcall into smb2_pread() on the
 * target libsmb2 file handle.  The kernel chunks at 1 MiB so each call
 * fits comfortably inside the reply staging buffer; we simply clamp
 * defensively against outCap (sizeof(header) + whatever the kernel
 * allocated).  smb2_pread returns the number of bytes read on success
 * (may be < Length on EOF) or a negative errno on error.
 */
static LONG
HandleOpRead(const BYTE *in, ULONG inLen,
             BYTE *outBuf, ULONG outCap, ULONG *outWritten)
{
    SMB2D_OP_READ_IN hdr;
    struct smb2_context *ctx = NULL;
    void *opaque = NULL;
    int is_dir = 0;
    struct smb2fh *fh;
    SMB2D_OP_READ_OUT *oh;
    BYTE *payload;
    ULONG capacity;
    ULONG want;
    int rc;

    *outWritten = 0;

    if (inLen < sizeof(hdr)) return STATUS_INVALID_PARAMETER;
    if (outCap < sizeof(SMB2D_OP_READ_OUT)) return STATUS_INVALID_PARAMETER;
    memcpy(&hdr, in, sizeof(hdr));

    smb2d_log("smb2d: OP_READ fh=0x%llx offset=%llu len=%lu\n",
              (unsigned long long)hdr.FileHandle,
              (unsigned long long)hdr.Offset, hdr.Length);

    if (!LookupFile(hdr.FileHandle, &ctx, &opaque, &is_dir) ||
        ctx == NULL || opaque == NULL)
    {
        smb2d_log("smb2d: OP_READ unknown handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_INVALID_HANDLE;
    }
    if (is_dir) {
        smb2d_log("smb2d: OP_READ on directory handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    fh = (struct smb2fh *)opaque;

    oh      = (SMB2D_OP_READ_OUT *)outBuf;
    payload = outBuf + sizeof(SMB2D_OP_READ_OUT);
    memset(oh, 0, sizeof(*oh));

    capacity = outCap - (ULONG)sizeof(SMB2D_OP_READ_OUT);
    want = hdr.Length;
    if (want > capacity)
        want = capacity;

    if (want == 0) {
        oh->BytesRead = 0;
        *outWritten = (ULONG)sizeof(SMB2D_OP_READ_OUT);
        return SMB2D_STATUS_SUCCESS;
    }

    rc = smb2_pread(ctx, fh, payload, want, hdr.Offset);
    smb2d_log("smb2d: smb2_pread -> %d\n", rc);
    if (rc < 0) {
        smb2d_log("smb2d: smb2_pread error: %s\n", smb2_get_error(ctx));
        return (LONG)0xC000020CL; /* STATUS_UNEXPECTED_NETWORK_ERROR */
    }

    oh->BytesRead = (ULONG)rc;
    *outWritten = (ULONG)sizeof(SMB2D_OP_READ_OUT) + (ULONG)rc;
    return SMB2D_STATUS_SUCCESS;
}

/*
 * OP_WRITE: translate a kernel-side OP_WRITE upcall into smb2_pwrite() on
 * the target libsmb2 file handle.  The kernel chunks at 1 MiB so each
 * call fits inside a bounded receive buffer; we still clamp Length
 * defensively against the trailing payload length.  smb2_pwrite returns
 * the number of bytes written on success (may be < Length on a short
 * write) or a negative errno on error.
 */
static LONG
HandleOpWrite(const BYTE *in, ULONG inLen,
              SMB2D_OP_WRITE_OUT *out)
{
    SMB2D_OP_WRITE_IN hdr;
    struct smb2_context *ctx = NULL;
    void *opaque = NULL;
    int is_dir = 0;
    struct smb2fh *fh;
    ULONG want;
    int rc;

    memset(out, 0, sizeof(*out));

    if (inLen < sizeof(hdr)) return STATUS_INVALID_PARAMETER;
    memcpy(&hdr, in, sizeof(hdr));
    if ((ULONG)sizeof(hdr) + hdr.Length > inLen)
        return STATUS_INVALID_PARAMETER;

    smb2d_log("smb2d: OP_WRITE fh=0x%llx offset=%llu len=%lu\n",
              (unsigned long long)hdr.FileHandle,
              (unsigned long long)hdr.Offset, hdr.Length);

    if (!LookupFile(hdr.FileHandle, &ctx, &opaque, &is_dir) ||
        ctx == NULL || opaque == NULL)
    {
        smb2d_log("smb2d: OP_WRITE unknown handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_INVALID_HANDLE;
    }
    if (is_dir) {
        smb2d_log("smb2d: OP_WRITE on directory handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    fh = (struct smb2fh *)opaque;

    want = hdr.Length;
    if (want == 0) {
        out->BytesWritten = 0;
        return SMB2D_STATUS_SUCCESS;
    }

    /* smb2_pwrite's buf is const in the public prototype, so we can pass
     * the in-buffer pointer straight through. */
    rc = smb2_pwrite(ctx, fh, (const uint8_t *)(in + sizeof(hdr)),
                     want, hdr.Offset);
    smb2d_log("smb2d: smb2_pwrite -> %d\n", rc);
    if (rc < 0) {
        smb2d_log("smb2d: smb2_pwrite error: %s\n", smb2_get_error(ctx));
        return (LONG)0xC000020CL; /* STATUS_UNEXPECTED_NETWORK_ERROR */
    }

    out->BytesWritten = (ULONG)rc;
    return SMB2D_STATUS_SUCCESS;
}

/*
 * OP_FSYNC: fire libsmb2's smb2_fsync() on the target file handle so the
 * server forces a disk flush of the open stream.  Directory handles are
 * a no-op — SMB2 has no directory-flush op, and pretending one failed
 * would break FlushFileBuffers() on directory handles that applications
 * happen to open.  A negative rc from libsmb2 is surfaced as a network
 * error so FlushFileBuffers propagates failure to user code.
 */
static LONG
HandleOpFsync(const BYTE *in, ULONG inLen)
{
    SMB2D_OP_FSYNC_IN hdr;
    struct smb2_context *ctx = NULL;
    void *opaque = NULL;
    int is_dir = 0;
    struct smb2fh *fh;
    int rc;

    if (inLen < sizeof(hdr)) return STATUS_INVALID_PARAMETER;
    memcpy(&hdr, in, sizeof(hdr));

    smb2d_log("smb2d: OP_FSYNC fh=0x%llx\n",
              (unsigned long long)hdr.FileHandle);

    if (!LookupFile(hdr.FileHandle, &ctx, &opaque, &is_dir) ||
        ctx == NULL || opaque == NULL)
    {
        smb2d_log("smb2d: OP_FSYNC unknown handle=0x%llx\n",
                  (unsigned long long)hdr.FileHandle);
        return STATUS_INVALID_HANDLE;
    }
    if (is_dir) {
        /* SMB2 has no directory-flush; report success so the caller's
         * FlushFileBuffers on a directory handle still returns ok. */
        return SMB2D_STATUS_SUCCESS;
    }
    fh = (struct smb2fh *)opaque;

    rc = smb2_fsync(ctx, fh);
    smb2d_log("smb2d: smb2_fsync -> %d\n", rc);
    if (rc < 0) {
        smb2d_log("smb2d: smb2_fsync error: %s\n", smb2_get_error(ctx));
        return (LONG)0xC000020CL; /* STATUS_UNEXPECTED_NETWORK_ERROR */
    }
    return SMB2D_STATUS_SUCCESS;
}

static LONG
HandleDisconnect(const BYTE *in, ULONG inLen)
{
    ULONG64 handle;
    struct smb2_context *ctx;

    if (inLen < sizeof(handle)) return STATUS_INVALID_PARAMETER;
    memcpy(&handle, in, sizeof(handle));

    ctx = RemoveConn(handle);
    if (ctx == NULL) {
        smb2d_log("smb2d: OP_DISCONNECT handle=0x%llx not found\n",
                  (unsigned long long)handle);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    smb2d_log("smb2d: OP_DISCONNECT handle=0x%llx -> ok\n",
              (unsigned long long)handle);

    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    return SMB2D_STATUS_SUCCESS;
}

/* ---------- main loop ---------- */

/* Initial upcall receive capacity: needs to hold at least the SMB2D_UPCALL
 * header + 1 MiB (the kernel's per-chunk WRITE/READ bound) + a little
 * slack.  Keeping this in a heap allocation rather than on the stack
 * avoids the 1 MB default thread-stack growing a full 1 MiB on every
 * loop iteration. */
#define SMB2D_UPCALL_INBUF_INITIAL (1u << 20)  /* 1 MiB */
#define SMB2D_UPCALL_INBUF_MAX     (4u << 20)  /* 4 MiB absolute cap */

DWORD
Smb2dDaemonLoop(HANDLE hStopEvent)
{
    HANDLE hDev = INVALID_HANDLE_VALUE;
    BYTE  *inBuf = NULL;
    ULONG  inBufCap = 0;

    EnsureConnLock();

    smb2d_log("smb2d: daemon loop starting\n");

    inBufCap = SMB2D_UPCALL_INBUF_INITIAL +
               (ULONG)sizeof(SMB2D_UPCALL_HEADER) + 4096u;
    inBuf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, inBufCap);
    if (inBuf == NULL) {
        smb2d_log("smb2d: failed to allocate upcall inbuf\n");
        return 1;
    }

    for (;;) {
        DWORD br = 0;
        SMB2D_UPCALL_HEADER hdr;
        const BYTE *payload;
        ULONG payloadLen;
        LONG status;
        ULONG64 connHandle;
        BOOL ok;

        if (hStopEvent &&
            WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0)
            break;

        if (hDev == INVALID_HANDLE_VALUE) {
            hDev = OpenSmbRdr();
            if (hDev == INVALID_HANDLE_VALUE) {
                /* Driver not loaded yet, or MUP hasn't registered us.
                 * Back off and retry. */
                Sleep(500);
                continue;
            }
            smb2d_log("smb2d: opened %s\n", SMB2RDR_USER_DEVICE_NAME_A);
        }

        /* Block in the kernel until there's an upcall or the call is
         * aborted by an APC (service stop).  METHOD_BUFFERED copies
         * inBuf into the IRP; on return br is the bytes produced. */
        ok = DeviceIoControl(hDev, IOCTL_SMB2RDR_READ,
                             NULL, 0,
                             inBuf, inBufCap,
                             &br, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) {
                /* APC broke us out of the wait, typical on stop. */
                continue;
            }
            if (err == ERROR_INSUFFICIENT_BUFFER ||
                err == ERROR_MORE_DATA)
            {
                /* Kernel re-queued the upcall because our buffer is too
                 * small.  Double the receive buffer (capped) and retry. */
                ULONG newCap = inBufCap * 2u;
                BYTE *nbuf;
                if (newCap > SMB2D_UPCALL_INBUF_MAX)
                    newCap = SMB2D_UPCALL_INBUF_MAX;
                if (newCap <= inBufCap) {
                    smb2d_log("smb2d: upcall inbuf already at cap %lu, "
                              "giving up\n", inBufCap);
                    Sleep(200);
                    continue;
                }
                nbuf = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, inBuf, newCap);
                if (nbuf == NULL) {
                    smb2d_log("smb2d: upcall inbuf HeapReAlloc(%lu) failed\n",
                              newCap);
                    Sleep(200);
                    continue;
                }
                inBuf    = nbuf;
                inBufCap = newCap;
                smb2d_log("smb2d: upcall inbuf grown to %lu bytes\n",
                          inBufCap);
                continue;
            }
            smb2d_log("smb2d: upcall DeviceIoControl failed err=%lu\n", err);
            /* Reopen on the next iteration. */
            CloseHandle(hDev);
            hDev = INVALID_HANDLE_VALUE;
            Sleep(200);
            continue;
        }

        if (br < sizeof(hdr)) {
            smb2d_log("smb2d: short upcall, %lu bytes\n", br);
            continue;
        }
        memcpy(&hdr, inBuf, sizeof(hdr));
        payload = inBuf + sizeof(hdr);
        payloadLen = (br >= sizeof(hdr)) ? (ULONG)(br - sizeof(hdr)) : 0;
        if (hdr.InLength < payloadLen) payloadLen = hdr.InLength;

        smb2d_log("smb2d: opcode=%lu (%s) xid=%lld inlen=%lu\n",
                  hdr.Opcode, OpcodeName(hdr.Opcode), hdr.Xid, hdr.InLength);

        switch (hdr.Opcode) {
        case SMB2D_OP_CONNECT_SHARE:
            connHandle = 0;
            status = HandleConnectShare(payload, payloadLen, &connHandle);
            if (status == SMB2D_STATUS_SUCCESS) {
                SendDowncall(hDev, hdr.Xid, status,
                             &connHandle, (ULONG)sizeof(connHandle));
            } else {
                SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            }
            break;

        case SMB2D_OP_DISCONNECT:
            status = HandleDisconnect(payload, payloadLen);
            SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            break;

        case SMB2D_OP_CREATE: {
            SMB2D_OP_CREATE_OUT cout;
            status = HandleOpCreate(payload, payloadLen, &cout);
            if (status == SMB2D_STATUS_SUCCESS) {
                SendDowncall(hDev, hdr.Xid, status, &cout, (ULONG)sizeof(cout));
            } else {
                SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            }
            break;
        }

        case SMB2D_OP_CLOSE:
            status = HandleOpClose(payload, payloadLen);
            SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            break;

        case SMB2D_OP_READDIR: {
            /* Pre-read the caller's declared MaxOutputLen to size a
             * correctly-bounded reply staging buffer; clamp defensively. */
            SMB2D_OP_READDIR_IN rdhdr;
            BYTE *rdbuf;
            ULONG rdcap;
            ULONG rdWritten = 0;
            if (payloadLen < sizeof(rdhdr)) {
                SendDowncall(hDev, hdr.Xid, STATUS_INVALID_PARAMETER, NULL, 0);
                break;
            }
            memcpy(&rdhdr, payload, sizeof(rdhdr));
            /* Cap at 1 MB — dir listings on NT are usually 4K–64K, but
             * don't let a bogus header wedge the daemon. */
            if (rdhdr.MaxOutputLen > (1u << 20))
                rdhdr.MaxOutputLen = (1u << 20);
            rdcap = (ULONG)sizeof(SMB2D_OP_READDIR_OUT) + rdhdr.MaxOutputLen;
            rdbuf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rdcap);
            if (rdbuf == NULL) {
                SendDowncall(hDev, hdr.Xid, (LONG)0xC0000017L, NULL, 0);
                break;
            }
            status = HandleOpReaddir(payload, payloadLen, rdbuf, rdcap, &rdWritten);
            if (status == SMB2D_STATUS_SUCCESS && rdWritten >= sizeof(SMB2D_OP_READDIR_OUT)) {
                SendDowncall(hDev, hdr.Xid, status, rdbuf, rdWritten);
            } else {
                SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            }
            HeapFree(GetProcessHeap(), 0, rdbuf);
            break;
        }

        case SMB2D_OP_READ: {
            SMB2D_OP_READ_IN rhdr;
            BYTE *rbuf;
            ULONG rcap;
            ULONG rWritten = 0;
            if (payloadLen < sizeof(rhdr)) {
                SendDowncall(hDev, hdr.Xid, STATUS_INVALID_PARAMETER, NULL, 0);
                break;
            }
            memcpy(&rhdr, payload, sizeof(rhdr));
            /* Cap at 1 MiB — matches the kernel-side chunk bound and keeps
             * the daemon's heap footprint predictable. */
            if (rhdr.Length > (1u << 20))
                rhdr.Length = (1u << 20);
            rcap = (ULONG)sizeof(SMB2D_OP_READ_OUT) + rhdr.Length;
            rbuf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, rcap);
            if (rbuf == NULL) {
                SendDowncall(hDev, hdr.Xid, (LONG)0xC0000017L, NULL, 0);
                break;
            }
            status = HandleOpRead(payload, payloadLen, rbuf, rcap, &rWritten);
            if (status == SMB2D_STATUS_SUCCESS &&
                rWritten >= sizeof(SMB2D_OP_READ_OUT))
            {
                SendDowncall(hDev, hdr.Xid, status, rbuf, rWritten);
            } else {
                SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            }
            HeapFree(GetProcessHeap(), 0, rbuf);
            break;
        }

        case SMB2D_OP_WRITE: {
            SMB2D_OP_WRITE_OUT wout;
            status = HandleOpWrite(payload, payloadLen, &wout);
            if (status == SMB2D_STATUS_SUCCESS) {
                SendDowncall(hDev, hdr.Xid, status,
                             &wout, (ULONG)sizeof(wout));
            } else {
                SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            }
            break;
        }

        case SMB2D_OP_FSYNC:
            status = HandleOpFsync(payload, payloadLen);
            SendDowncall(hDev, hdr.Xid, status, NULL, 0);
            break;

        case SMB2D_OP_QUERY_INFO:
        default:
            smb2d_log("smb2d: opcode %lu (%s) not implemented\n",
                      hdr.Opcode, OpcodeName(hdr.Opcode));
            SendDowncall(hDev, hdr.Xid, STATUS_NOT_IMPLEMENTED, NULL, 0);
            break;
        }
    }

    if (hDev != INVALID_HANDLE_VALUE)
        CloseHandle(hDev);
    if (inBuf)
        HeapFree(GetProcessHeap(), 0, inBuf);

    smb2d_log("smb2d: daemon loop exiting\n");
    return 0;
}

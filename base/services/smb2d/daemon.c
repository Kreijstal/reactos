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
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "smb2d.h"

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
#define SMB2D_STATUS_SUCCESS           ((LONG)0x00000000L)

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
    BYTE buf[4096];
    DWORD br = 0;
    SMB2D_DOWNCALL_HEADER hdr;
    BOOL ok;

    if (outLen > sizeof(buf) - sizeof(hdr))
        outLen = sizeof(buf) - sizeof(hdr);

    hdr.Xid = xid;
    hdr.Status = status;
    hdr.OutLength = outLen;

    memcpy(buf, &hdr, sizeof(hdr));
    if (outLen && out)
        memcpy(buf + sizeof(hdr), out, outLen);

    ok = DeviceIoControl(h, IOCTL_SMB2RDR_WRITE,
                         buf, sizeof(hdr) + outLen,
                         NULL, 0, &br, NULL);
    if (!ok)
        smb2d_log("smb2d: downcall DeviceIoControl failed xid=%lld err=%lu\n",
                  xid, GetLastError());
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

DWORD
Smb2dDaemonLoop(HANDLE hStopEvent)
{
    HANDLE hDev = INVALID_HANDLE_VALUE;

    EnsureConnLock();

    smb2d_log("smb2d: daemon loop starting\n");

    for (;;) {
        BYTE inBuf[8192];
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
                             inBuf, sizeof(inBuf),
                             &br, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) {
                /* APC broke us out of the wait, typical on stop. */
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

        case SMB2D_OP_CREATE:
        case SMB2D_OP_READDIR:
        case SMB2D_OP_QUERY_INFO:
        case SMB2D_OP_READ:
        case SMB2D_OP_CLOSE:
        default:
            smb2d_log("smb2d: opcode %lu (%s) not implemented\n",
                      hdr.Opcode, OpcodeName(hdr.Opcode));
            SendDowncall(hDev, hdr.Xid, STATUS_NOT_IMPLEMENTED, NULL, 0);
            break;
        }
    }

    if (hDev != INVALID_HANDLE_VALUE)
        CloseHandle(hDev);

    smb2d_log("smb2d: daemon loop exiting\n");
    return 0;
}

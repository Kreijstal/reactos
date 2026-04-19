/*
 * smb2d daemon loop.  Skeletal proof-of-plumbing: open \\.\smb2rdr,
 * block on IOCTL_SMB2RDR_READ, decode the header, print the inline
 * payload, reply with a hard-coded STATUS_NOT_IMPLEMENTED / no output
 * so the kernel side can unblock and unwind.  A later pass replaces
 * the SMB2D_OP_CONNECT_SHARE case with a real libsmb2 smb2_connect_share
 * call; the rest of the op set stays stubbed until individual MRx
 * callbacks are wired up.
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

#include "smb2d.h"

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

#define STATUS_NOT_IMPLEMENTED ((LONG)0xC0000002L)

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
        fprintf(stderr,
                "smb2d: downcall DeviceIoControl failed xid=%lld err=%lu\n",
                xid, GetLastError());
    return ok;
}

DWORD
Smb2dDaemonLoop(HANDLE hStopEvent)
{
    HANDLE hDev = INVALID_HANDLE_VALUE;

    fprintf(stdout, "smb2d: daemon loop starting\n");
    fflush(stdout);

    for (;;) {
        BYTE inBuf[8192];
        DWORD br = 0;
        SMB2D_UPCALL_HEADER hdr;
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
            fprintf(stdout, "smb2d: opened %s\n", SMB2RDR_USER_DEVICE_NAME_A);
            fflush(stdout);
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
            fprintf(stderr,
                    "smb2d: upcall DeviceIoControl failed err=%lu\n", err);
            /* Reopen on the next iteration. */
            CloseHandle(hDev);
            hDev = INVALID_HANDLE_VALUE;
            Sleep(200);
            continue;
        }

        if (br < sizeof(hdr)) {
            fprintf(stderr, "smb2d: short upcall, %lu bytes\n", br);
            continue;
        }
        memcpy(&hdr, inBuf, sizeof(hdr));

        fprintf(stdout,
                "smb2d: opcode=%lu (%s) xid=%lld inlen=%lu data=%.*ls\n",
                hdr.Opcode, OpcodeName(hdr.Opcode), hdr.Xid, hdr.InLength,
                (int)(hdr.InLength / sizeof(WCHAR)),
                (WCHAR *)(inBuf + sizeof(hdr)));
        fflush(stdout);

        /* TODO(next agent): replace this with an op dispatcher so
         * SMB2D_OP_CONNECT_SHARE actually calls smb2_connect_share via
         * libsmb2.  The payload is the server name in UTF-16LE (no NUL)
         * at inBuf + sizeof(hdr), hdr.InLength bytes long.  A successful
         * connect returns a context handle that the driver can stash in
         * the MRX_SRV_CALL extension; for now we stub it out and force
         * rdbss to unwind with STATUS_NOT_IMPLEMENTED. */
        SendDowncall(hDev, hdr.Xid, STATUS_NOT_IMPLEMENTED, NULL, 0);
    }

    if (hDev != INVALID_HANDLE_VALUE)
        CloseHandle(hDev);

    fprintf(stdout, "smb2d: daemon loop exiting\n");
    fflush(stdout);
    return 0;
}

/*
 * smb2rdr - SMB2/3 mini-redirector for ReactOS.
 * Pairs with a usermode daemon linked against libsmb2 for the wire-protocol
 * work; the driver just handles RDBSS integration and IRP dispatch.
 */

#ifndef _SMB2RDR_H_
#define _SMB2RDR_H_

#define SMB2RDR_DEVICE_NAME         L"\\Device\\smb2rdr"
#define SMB2RDR_SHADOW_DEVICE_NAME  L"\\??\\smb2rdr"
#define SMB2RDR_USER_DEVICE_NAME    L"\\\\.\\smb2rdr"
#define SMB2RDR_USER_DEVICE_NAME_A  "\\\\.\\smb2rdr"
#define SMB2RDR_PROVIDER_NAME_A     "SMB2 Network"
#define SMB2RDR_PROVIDER_NAME_U     L"SMB2 Network"

#define _RDR_CTL_CODE(code, method) \
    CTL_CODE(FILE_DEVICE_NETWORK_REDIRECTOR, 0x800 | (code), method, FILE_ANY_ACCESS)

#define IOCTL_SMB2RDR_START     _RDR_CTL_CODE(0, METHOD_BUFFERED)
#define IOCTL_SMB2RDR_STOP      _RDR_CTL_CODE(1, METHOD_NEITHER)
#define IOCTL_SMB2RDR_GETSTATE  _RDR_CTL_CODE(3, METHOD_NEITHER)
#define IOCTL_SMB2RDR_ADDCONN   _RDR_CTL_CODE(4, METHOD_BUFFERED)
#define IOCTL_SMB2RDR_DELCONN   _RDR_CTL_CODE(5, METHOD_BUFFERED)
/* Daemon poll: kernel -> usermode (serialized upcall entry). */
#define IOCTL_SMB2RDR_READ      _RDR_CTL_CODE(6, METHOD_BUFFERED)
/* Daemon reply: usermode -> kernel (downcall for a pending xid). */
#define IOCTL_SMB2RDR_WRITE     _RDR_CTL_CODE(7, METHOD_BUFFERED)

/* Legacy names kept for the early scaffolding; prefer READ/WRITE. */
#define IOCTL_SMB2RDR_UPCALL    IOCTL_SMB2RDR_READ
#define IOCTL_SMB2RDR_DOWNCALL  IOCTL_SMB2RDR_WRITE

#endif /* _SMB2RDR_H_ */

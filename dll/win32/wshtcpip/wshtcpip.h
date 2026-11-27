/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS WinSock Helper DLL for TCP/IP
 * FILE:        include/wshtcpip.h
 * PURPOSE:     WinSock Helper DLL for TCP/IP header
 */
#ifndef __WSHTCPIP_H
#define __WSHTCPIP_H

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <wsahelp.h>
#include <tdiinfo.h>
#include <tcpioctl.h>
#include <tdilib.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <rtlfuncs.h>
#include <mstcpip.h>

#define EXPORT WINAPI

#define DD_TCP_DEVICE_NAME      L"\\Device\\Tcp"
#define DD_UDP_DEVICE_NAME      L"\\Device\\Udp"
#define DD_RAW_IP_DEVICE_NAME   L"\\Device\\RawIp"

typedef enum _SOCKET_STATE {
    SocketStateCreated,
    SocketStateBound,
    SocketStateListening,
    SocketStateConnected
} SOCKET_STATE, *PSOCKET_STATE;

/* A request kept until the socket has the TDI object it is for */
typedef struct _QUEUED_REQUEST {
    PTCP_REQUEST_SET_INFORMATION_EX Info;
    struct _QUEUED_REQUEST *Next;
} QUEUED_REQUEST, *PQUEUED_REQUEST;

/* The options the helper sets on TCP/IP, which a socket a listening one
 * accepts has as the listening one does */
typedef enum _SOCKET_OPTION_SLOT {
    OptionKeepAlive,        /* SO_KEEPALIVE */
    OptionKeepAliveValues,  /* SIO_KEEPALIVE_VALS */
    OptionTtl,              /* IP_TTL */
    OptionDontFragment,     /* IP_DONTFRAGMENT */
    OptionHeaderInclude,    /* IP_HDRINCL */
    OptionUnicastIf,        /* IP_UNICAST_IF */
    OptionNoDelay,          /* TCP_NODELAY */
    OptionSlotCount
} SOCKET_OPTION_SLOT;

/* The value an option was last set to, as it went to TCP/IP */
typedef struct _SOCKET_OPTION {
    ULONG Length;           /* 0 while the option has not been set */
    UCHAR Value[sizeof(struct tcp_keepalive)];
} SOCKET_OPTION, *PSOCKET_OPTION;

/* A socket's context.  SOL_INTERNAL/SO_CONTEXT hands a copy of it out, and
 * takes one of the listening socket's for a socket it accepted. */
typedef struct _SOCKET_CONTEXT {
    INT AddressFamily;
    INT SocketType;
    INT Protocol;
    DWORD Flags;
    SOCKET_STATE SocketState;
    PQUEUED_REQUEST RequestQueue;   /* Not valid in a copy */
    BOOL DontRoute;
    BOOL KeepAlive;
    BOOLEAN NoDelay;
    DWORD UnicastIf;   /* IP_UNICAST_IF value as given by the caller (network byte order) */
    SOCKET_OPTION Options[OptionSlotCount];
} SOCKET_CONTEXT, *PSOCKET_CONTEXT;

INT
WSHIoctl_GetInterfaceList(
    IN  LPVOID OutputBuffer,
    IN  DWORD OutputBufferLength,
    OUT LPDWORD NumberOfBytesReturned,
    OUT LPBOOL NeedsCompletion);

INT
SendRequest(
    _In_ HANDLE Handle,
    _In_ PTCP_REQUEST_SET_INFORMATION_EX Info);

#endif /* __WSHTCPIP_H */

/* EOF */

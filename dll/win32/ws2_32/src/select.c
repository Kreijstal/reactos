/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS WinSock 2 API
 * FILE:        dll/win32/ws2_32/src/select.c
 * PURPOSE:     Socket Select Support
 * PROGRAMMER:  Alex Ionescu (alex@relsoft.net)
 */

/* INCLUDES ******************************************************************/

/* WSAPoll and WSAPOLLFD are gated behind _WIN32_WINNT >= 0x600 in the PSDK
 * winsock2.h. Raise the target here so the types are visible to ws2_32's
 * own implementation; callers using <=WinServer2003 simply can't call
 * WSAPoll, but ws2_32 must still compile it. */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600

#include <ws2_32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
INT
WSPAPI
__WSAFDIsSet(SOCKET s,
             LPFD_SET set)
{
    INT i = set->fd_count;
    INT Return = FALSE;

    /* Loop until a match is found */
    while (i--) if (set->fd_array[i] == s) Return = TRUE;

    /* Return */
    return Return;
}

/*
 * @implemented
 */
INT
WSAAPI
select(IN INT s,
       IN OUT LPFD_SET readfds,
       IN OUT LPFD_SET writefds,
       IN OUT LPFD_SET exceptfds,
       IN CONST struct timeval *timeout)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    SOCKET Handle;
    LPWSPSELECT WSPSelect;

    DPRINT("select: %lx %p %p %p %p\n", s, readfds, writefds, exceptfds, timeout);

    /* Check for WSAStartup */
    ErrorCode = WsQuickProlog();

    if (ErrorCode != ERROR_SUCCESS)
    {
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Use the first Socket from the first valid set */
    if (readfds && readfds->fd_count)
    {
        Handle = readfds->fd_array[0];
    }
    else if (writefds && writefds->fd_count)
    {
        Handle = writefds->fd_array[0];
    }
    else if (exceptfds && exceptfds->fd_count)
    {
        Handle = exceptfds->fd_array[0];
    }
    else
    {
        /* Invalid handles */
        SetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    /* Get the Socket Context */
    Socket = WsSockGetSocket(Handle);

    if (!Socket)
    {
        /* No Socket Context Found */
        SetLastError(WSAENOTSOCK);
        return SOCKET_ERROR;
    }

    /* Get the select procedure */
    WSPSelect = Socket->Provider->Service.lpWSPSelect;

    /* Make the call */
    Status = WSPSelect(s, readfds, writefds, exceptfds, (struct timeval *)timeout,
                       &ErrorCode);

    /* Deference the Socket Context */
    WsSockDereference(Socket);

    /* Return Provider Value */
    if (Status != SOCKET_ERROR)
        return Status;

    /* If everything seemed fine, then the WSP call failed itself */
    if (ErrorCode == NO_ERROR)
        ErrorCode = WSASYSCALLFAILURE;

    /* Return with an error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSPAPI
WPUFDIsSet(IN SOCKET s,
           IN LPFD_SET set)
{
    UNIMPLEMENTED;
    return (SOCKET)0;
}

/*
 * @implemented
 *
 * WSAPoll() built on top of select(). The underlying WSP provider exposes
 * lpWSPSelect but not a native poll entry, so the pollfd array is translated
 * into read/write/except fd_sets, select() is called, and the result mapped
 * back. This matches the semantics clients such as libsmb2 rely on for
 * async connect() completion (POLLOUT once the TCP handshake finishes).
 */
INT
WSAAPI
WSAPoll(LPWSAPOLLFD fdArray,
        ULONG fds,
        INT timeout)
{
    fd_set rfds, wfds, efds;
    struct timeval tv, *ptv;
    ULONG i;
    INT nready;
    INT nfds_max = 0;

    if (fds == 0 || fdArray == NULL)
    {
        SetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    /* fd_set only holds FD_SETSIZE sockets; bail if the caller exceeds it. */
    if (fds > FD_SETSIZE)
    {
        SetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    for (i = 0; i < fds; i++)
    {
        fdArray[i].revents = 0;
        if (fdArray[i].fd == INVALID_SOCKET)
            continue;

        if (fdArray[i].events & (POLLRDNORM | POLLRDBAND))
            FD_SET(fdArray[i].fd, &rfds);
        if (fdArray[i].events & POLLWRNORM)
            FD_SET(fdArray[i].fd, &wfds);
        /* Error/HUP signalling surfaces via the except fd_set. */
        FD_SET(fdArray[i].fd, &efds);

        if ((INT)fdArray[i].fd > nfds_max)
            nfds_max = (INT)fdArray[i].fd;
    }

    if (timeout < 0)
    {
        ptv = NULL;
    }
    else
    {
        tv.tv_sec  = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }

    nready = select(nfds_max + 1, &rfds, &wfds, &efds, ptv);
    if (nready == SOCKET_ERROR)
        return SOCKET_ERROR;
    if (nready == 0)
        return 0;

    nready = 0;
    for (i = 0; i < fds; i++)
    {
        if (fdArray[i].fd == INVALID_SOCKET)
            continue;

        if (__WSAFDIsSet(fdArray[i].fd, &rfds))
            fdArray[i].revents |= (fdArray[i].events & (POLLRDNORM | POLLRDBAND));
        if (__WSAFDIsSet(fdArray[i].fd, &wfds))
            fdArray[i].revents |= (fdArray[i].events & POLLWRNORM);
        if (__WSAFDIsSet(fdArray[i].fd, &efds))
        {
            /* select() signals both "urgent data" and "connect failure /
             * OOB" via the except set. Report POLLERR so callers that
             * poll connecting sockets can distinguish failure from
             * readiness by inspecting SO_ERROR. */
            fdArray[i].revents |= POLLERR;
        }

        if (fdArray[i].revents)
            nready++;
    }

    return nready;
}

/*
 * @implemented
 */
INT
WSAAPI
WSAAsyncSelect(IN SOCKET s,
               IN HWND hWnd,
               IN UINT wMsg,
               IN LONG lEvent)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    DPRINT("WSAAsyncSelect: %lx, %lx, %lx, %lx\n", s, hWnd, wMsg, lEvent);

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickProlog()) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Make the call */
            Status = Socket->Provider->Service.lpWSPAsyncSelect(s,
                                                                hWnd,
                                                                wMsg,
                                                                lEvent,
                                                                &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return Status;

            /* If everything seemed fine, then the WSP call failed itself */
            if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSAEventSelect(IN SOCKET s,
               IN WSAEVENT hEventObject,
               IN LONG lNetworkEvents)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickProlog()) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Make the call */
            Status = Socket->Provider->Service.lpWSPEventSelect(s,
                                                        hEventObject,
                                                        lNetworkEvents,
                                                        &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return Status;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

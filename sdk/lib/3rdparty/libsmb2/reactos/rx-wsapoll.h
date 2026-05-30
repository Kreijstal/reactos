/* WSAPoll polyfill for pre-Vista ReactOS targets.
 * Force-included by CMake so upstream sources see struct pollfd/WSAPoll.
 */

#ifndef LIBSMB2_RX_WSAPOLL_H
#define LIBSMB2_RX_WSAPOLL_H

#include <winsock2.h>
#include <mswsock.h>

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600

#define POLLRDNORM 0x0100
#define POLLRDBAND 0x0200
#define POLLIN     (POLLRDNORM | POLLRDBAND)
#define POLLPRI    0x0400
#define POLLWRNORM 0x0010
#define POLLOUT    (POLLWRNORM)
#define POLLWRBAND 0x0020
#define POLLERR    0x0001
#define POLLHUP    0x0002
#define POLLNVAL   0x0004

typedef struct pollfd {
    SOCKET fd;
    SHORT  events;
    SHORT  revents;
} WSAPOLLFD, *PWSAPOLLFD, *LPWSAPOLLFD;

int rx_WSAPoll(LPWSAPOLLFD fdArray, ULONG fds, INT timeout);
#define WSAPoll rx_WSAPoll

#endif /* pre-Vista */

#endif /* LIBSMB2_RX_WSAPOLL_H */

/* select()-based WSAPoll polyfill. */

#include "rx-wsapoll.h"

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600

#undef WSAPoll

int
rx_WSAPoll(LPWSAPOLLFD fdArray, ULONG fds, INT timeout)
{
    fd_set rfds, wfds, efds;
    struct timeval tv, *ptv;
    ULONG i;
    int nready;
    SOCKET maxfd = 0;

    if (fds == 0 || fdArray == NULL) {
        return 0;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    for (i = 0; i < fds; i++) {
        fdArray[i].revents = 0;
        if (fdArray[i].fd == INVALID_SOCKET) {
            continue;
        }
        if (fdArray[i].events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI)) {
            FD_SET(fdArray[i].fd, &rfds);
        }
        if (fdArray[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
            FD_SET(fdArray[i].fd, &wfds);
        }
        FD_SET(fdArray[i].fd, &efds);
        if (fdArray[i].fd > maxfd) {
            maxfd = fdArray[i].fd;
        }
    }

    if (timeout < 0) {
        ptv = NULL;
    } else {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }

    nready = select((int)maxfd + 1, &rfds, &wfds, &efds, ptv);
    if (nready <= 0) {
        return nready;
    }

    nready = 0;
    for (i = 0; i < fds; i++) {
        if (fdArray[i].fd == INVALID_SOCKET) {
            continue;
        }
        if (FD_ISSET(fdArray[i].fd, &rfds)) {
            fdArray[i].revents |= (fdArray[i].events & (POLLIN | POLLRDNORM | POLLRDBAND));
        }
        if (FD_ISSET(fdArray[i].fd, &wfds)) {
            fdArray[i].revents |= (fdArray[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND));
        }
        if (FD_ISSET(fdArray[i].fd, &efds)) {
            fdArray[i].revents |= POLLERR;
        }
        if (fdArray[i].revents) {
            nready++;
        }
    }
    return nready;
}

#endif /* _WIN32 */

/* POSIX stubs libsmb2's compat.h declares but does not implement on Win32. */

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void srandom(unsigned int seed)
{
    srand(seed);
}

int random(void)
{
    /* rand() is 15-bit on MSVCRT; compose to 31 bits. */
    return (rand() << 16) ^ (rand() << 1) ^ (rand() & 1);
}

int getlogin_r(char *buf, size_t size)
{
    DWORD len = (DWORD)size;
    if (!buf || size == 0) {
        return -1;
    }
    if (!GetUserNameA(buf, &len)) {
        buf[0] = '\0';
        return -1;
    }
    return 0;
}

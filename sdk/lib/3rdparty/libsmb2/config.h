/* Static libsmb2 configuration for ReactOS (replaces autoconf output). */

#ifndef LIBSMB2_REACTOS_CONFIG_H
#define LIBSMB2_REACTOS_CONFIG_H

/* MinGW / Win32 headers that are reachable from ReactOS targets. */
#define HAVE_STDINT_H      1
#define HAVE_STDIO_H       1
#define HAVE_STDLIB_H      1
#define HAVE_STRING_H      1
#define HAVE_FCNTL_H       1
#define HAVE_ERRNO_H       1
#define HAVE_TIME_H        1
#define HAVE_SYS_STAT_H    1
#define HAVE_SYS_TYPES_H   1
#define HAVE_UNISTD_H      1

/* Winsock2 struct linger + sockaddr_storage are available. */
#define HAVE_LINGER            1
#define HAVE_SOCKADDR_STORAGE  1

/* TCP socket linger behavior: disabled by default. */
#define CONFIGURE_OPTION_TCP_LINGER 0

/* Kerberos disabled; krb5-wrapper.c is excluded from the build. */
/* #undef HAVE_LIBKRB5 */

#endif /* LIBSMB2_REACTOS_CONFIG_H */

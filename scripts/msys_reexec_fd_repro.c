#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct _REPRO_WSADATA
{
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
} REPRO_WSADATA;

typedef int (WINAPI *REPRO_WSASTARTUP)(WORD, REPRO_WSADATA *);

static int wsaprobe_main(int argc, char **argv)
{
    HMODULE ws2;
    REPRO_WSASTARTUP startup;
    REPRO_WSADATA data;
    int ret;
    int i;

    for (i = 2; i < argc; i++)
    {
        HMODULE mod = LoadLibraryA(argv[i]);
        fprintf(stderr, "wsaprobe: LoadLibraryA(%s)=%p gle=%lu\n",
                argv[i], mod, GetLastError());
    }

    ws2 = LoadLibraryA("ws2_32.dll");
    fprintf(stderr, "wsaprobe: LoadLibraryA(ws2_32.dll)=%p gle=%lu\n",
            ws2, GetLastError());
    if (ws2 == NULL)
        return 31;

    startup = (REPRO_WSASTARTUP)GetProcAddress(ws2, "WSAStartup");
    fprintf(stderr, "wsaprobe: GetProcAddress(WSAStartup)=%p gle=%lu\n",
            startup, GetLastError());
    if (startup == NULL)
        return 32;

    memset(&data, 0, sizeof(data));
    ret = startup(MAKEWORD(2, 2), &data);
    fprintf(stderr,
            "wsaprobe: WSAStartup ret=%d gle=%lu version=%u.%u high=%u.%u status='%s'\n",
            ret, GetLastError(),
            LOBYTE(data.wVersion), HIBYTE(data.wVersion),
            LOBYTE(data.wHighVersion), HIBYTE(data.wHighVersion),
            data.szSystemStatus);
    return ret == 0 ? 0 : 33;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;

    while (done < len)
    {
        ssize_t ret = read(fd, p + done, len - done);
        if (ret < 0)
        {
            fprintf(stderr, "child: read fd %d failed: errno=%d %s\n",
                    fd, errno, strerror(errno));
            return -1;
        }
        if (ret == 0)
        {
            fprintf(stderr, "child: EOF after %zu/%zu bytes on fd %d\n",
                    done, len, fd);
            return -1;
        }
        done += (size_t)ret;
    }

    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t done = 0;

    while (done < len)
    {
        ssize_t ret = write(fd, p + done, len - done);
        if (ret < 0)
        {
            fprintf(stderr, "parent: write fd %d failed: errno=%d %s\n",
                    fd, errno, strerror(errno));
            return -1;
        }
        done += (size_t)ret;
    }

    return 0;
}

static void close_from_fd(int lowfd)
{
    long maxfd = sysconf(_SC_OPEN_MAX);
    int fd;

    if (maxfd < lowfd || maxfd > 4096)
        maxfd = 4096;

    for (fd = lowfd; fd < maxfd; fd++)
        close(fd);
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int child_main(int fd, int read_two, int big_msg, int be_msg)
{
    uint32_t len;
    unsigned char lenbuf[4];
    char *buf;
    int flags;
    int i;

    flags = fcntl(8, F_GETFD);
    fprintf(stderr, "child: fd8 fcntl(F_GETFD)=%d errno=%d %s\n",
            flags, errno, strerror(errno));

    for (i = 0; i < (read_two ? 2 : 1); i++)
    {
        fprintf(stderr, "child: reading message %d length from fd %d\n",
                i + 1, fd);
        if (read_full(fd, lenbuf, sizeof(lenbuf)) < 0)
            return 2;
        if (be_msg)
            len = load_be32(lenbuf);
        else
            memcpy(&len, lenbuf, sizeof(len));
        fprintf(stderr, "child: message %d length=%u\n", i + 1, len);

        if (len > 1024 * 1024)
        {
            fprintf(stderr, "child: length too large\n");
            return 3;
        }

        buf = malloc(len + 1);
        if (buf == NULL)
        {
            fprintf(stderr, "child: malloc failed for %u bytes\n", len);
            return 6;
        }

        if (read_full(fd, buf, len) < 0)
        {
            free(buf);
            return 4;
        }
        buf[len] = 0;

        fprintf(stderr, "child: message %d first='%c' last='%c'\n",
                i + 1, len != 0 ? buf[0] : '?', len != 0 ? buf[len - 1] : '?');
        if (!big_msg &&
            strcmp(buf, i == 0 ? "rexec-state-ok" : "hostkeys-ok") != 0)
        {
            free(buf);
            return 5;
        }
        free(buf);
    }
    return 0;
}

static int write_message(int fd, const char *tag, char fill, uint32_t len, int be_msg)
{
    unsigned char lenbuf[4];
    char stack_buf[256];
    char *buf = stack_buf;
    int ret;

    if (len > sizeof(stack_buf))
    {
        buf = malloc(len);
        if (buf == NULL)
        {
            fprintf(stderr, "%s: malloc failed for %u bytes\n", tag, len);
            return -1;
        }
    }

    memset(buf, fill, len);
    if (be_msg)
        store_be32(lenbuf, len);
    else
        memcpy(lenbuf, &len, sizeof(lenbuf));
    fprintf(stderr, "%s: writing length=%u fill=%c endian=%s\n",
            tag, len, fill, be_msg ? "be" : "host");
    ret = write_full(fd, lenbuf, sizeof(lenbuf)) < 0 ||
          write_full(fd, buf, len) < 0 ? -1 : 0;
    if (buf != stack_buf)
        free(buf);
    return ret;
}

static int make_tcp_pair(int *accepted_out, int *client_out)
{
    int listener = -1;
    int client = -1;
    int accepted = -1;
    int on = 1;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        goto fail;

    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listener, 1) < 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) < 0)
        goto fail;

    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0)
        goto fail;

    if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail;

    accepted = accept(listener, NULL, NULL);
    if (accepted < 0)
        goto fail;

    close(listener);
    *accepted_out = accepted;
    *client_out = client;
    return 0;

fail:
    fprintf(stderr, "parent: tcp pair failed: errno=%d %s\n",
            errno, strerror(errno));
    if (listener >= 0) close(listener);
    if (client >= 0) close(client);
    if (accepted >= 0) close(accepted);
    return -1;
}

static const char *get_other_exe_path(const char *self, char *buf, size_t len)
{
    const char *slash = strrchr(self, '/');
    const char *backslash = strrchr(self, '\\');
    const char *base = slash;

    if (backslash != NULL && (base == NULL || backslash > base))
        base = backslash;

    if (base == NULL)
        snprintf(buf, len, "msys_reexec_fd_child.exe");
    else
    {
        size_t dir_len = (size_t)(base - self + 1);
        if (dir_len >= len)
            dir_len = len - 1;
        memcpy(buf, self, dir_len);
        buf[dir_len] = 0;
        strncat(buf, "msys_reexec_fd_child.exe", len - strlen(buf) - 1);
    }

    return buf;
}

int main(int argc, char **argv)
{
    int fds[2];
    int child_fd;
    int accepted_fd = -1;
    int client_fd = -1;
    int use_spawn;
    int use_dup4;
    int use_tcp8;
    int use_writerfork;
    int use_forkexec;
    int use_twomsg;
    int use_otherexe;
    int use_bigmsg;
    int use_highfd;
    int use_sshdsession;
    int use_bemsg;
    pid_t writer_pid = -1;
    int status;
    pid_t pid;
    uint32_t len;
    const char payload[] = "rexec-state-ok";
    char fd_arg[32];
    char other_exe[512];
    char *child_argv[5];
    char *sshd_session_argv[6];
    const char *mode = argc > 1 ? argv[1] : "socketpair";

    if (argc >= 2 && strcmp(argv[1], "wsaprobe") == 0)
        return wsaprobe_main(argc, argv);

    if (argc >= 3 && strcmp(argv[1], "child") == 0)
        return child_main(atoi(argv[2]),
                          argc >= 4 && strstr(argv[3], "twomsg") != NULL,
                          argc >= 4 && strstr(argv[3], "bigmsg") != NULL,
                          argc >= 4 && strstr(argv[3], "bemsg") != NULL);

    if (strcmp(mode, "pipe") == 0)
    {
        if (pipe(fds) < 0)
        {
            fprintf(stderr, "parent: pipe failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 10;
        }
    }
    else
    {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        {
            fprintf(stderr, "parent: socketpair failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 11;
        }
    }

    use_spawn = strstr(mode, "spawn") != NULL;
    use_dup4 = strstr(mode, "dup4") != NULL;
    use_tcp8 = strstr(mode, "tcp8") != NULL;
    use_writerfork = strstr(mode, "writerfork") != NULL;
    use_forkexec = strstr(mode, "forkexec") != NULL;
    use_twomsg = strstr(mode, "twomsg") != NULL;
    use_otherexe = strstr(mode, "otherexe") != NULL;
    use_bigmsg = strstr(mode, "bigmsg") != NULL;
    use_highfd = strstr(mode, "highfd") != NULL;
    use_sshdsession = strstr(mode, "sshdsession") != NULL;
    use_bemsg = strstr(mode, "bemsg") != NULL;

    if (use_tcp8)
    {
        if (make_tcp_pair(&accepted_fd, &client_fd) < 0)
            return 18;

        if (accepted_fd != 8)
        {
            if (dup2(accepted_fd, 8) < 0)
            {
                fprintf(stderr, "parent: dup2(%d,8) failed: errno=%d %s\n",
                        accepted_fd, errno, strerror(errno));
                return 19;
            }
            close(accepted_fd);
            accepted_fd = 8;
        }

        if (fcntl(accepted_fd, F_SETFD, 0) < 0)
        {
            fprintf(stderr, "parent: clear close-on-exec fd8 failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 20;
        }
    }

    if (fcntl(fds[0], F_SETFD, 0) < 0 || fcntl(fds[1], F_SETFD, 0) < 0)
    {
        fprintf(stderr, "parent: clear close-on-exec failed: errno=%d %s\n",
                errno, strerror(errno));
        return 12;
    }

    if (use_highfd)
    {
        if (fds[0] != 9)
        {
            if (dup2(fds[0], 9) < 0)
            {
                fprintf(stderr, "parent: dup2(%d,9) failed: errno=%d %s\n",
                        fds[0], errno, strerror(errno));
                return 25;
            }
            close(fds[0]);
            fds[0] = 9;
        }
        if (fds[1] != 10)
        {
            if (dup2(fds[1], 10) < 0)
            {
                fprintf(stderr, "parent: dup2(%d,10) failed: errno=%d %s\n",
                        fds[1], errno, strerror(errno));
                return 26;
            }
            close(fds[1]);
            fds[1] = 10;
        }
        if (fcntl(fds[0], F_SETFD, 0) < 0 || fcntl(fds[1], F_SETFD, 0) < 0)
        {
            fprintf(stderr, "parent: clear close-on-exec high fds failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 27;
        }
    }

    child_fd = fds[1];
    if (use_dup4 && child_fd != 4)
    {
        if (dup2(child_fd, 4) < 0)
        {
            fprintf(stderr, "parent: dup2(%d,4) failed: errno=%d %s\n",
                    child_fd, errno, strerror(errno));
            return 16;
        }
        close(child_fd);
        child_fd = 4;
        if (fcntl(child_fd, F_SETFD, 0) < 0)
        {
            fprintf(stderr, "parent: clear close-on-exec fd4 failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 17;
        }
    }

    snprintf(fd_arg, sizeof(fd_arg), "%d", use_forkexec ? 4 : child_fd);
    child_argv[0] = argv[0];
    child_argv[1] = "child";
    child_argv[2] = fd_arg;
    child_argv[3] = use_bigmsg ?
        (use_bemsg ? "twomsg-bigmsg-bemsg" : "twomsg-bigmsg") :
        (use_twomsg ? (use_bemsg ? "twomsg-bemsg" : "twomsg") : NULL);
    child_argv[4] = NULL;

    sshd_session_argv[0] = "sshd-session";
    sshd_session_argv[1] = "-D";
    sshd_session_argv[2] = "-e";
    sshd_session_argv[3] = "-ddd";
    sshd_session_argv[4] = "-R";
    sshd_session_argv[5] = NULL;

    fprintf(stderr, "parent: mode=%s parent_fd=%d child_fd=%d accepted_fd=%d client_fd=%d\n",
            mode, fds[0], child_fd, accepted_fd, client_fd);

    if (use_forkexec)
    {
        pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "parent: forkexec fork failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 23;
        }
        if (pid == 0)
        {
            if (accepted_fd >= 0)
            {
                if (dup2(accepted_fd, STDIN_FILENO) < 0 ||
                    dup2(STDIN_FILENO, STDOUT_FILENO) < 0)
                {
                    fprintf(stderr, "child-preexec: socket dup2 failed: errno=%d %s\n",
                            errno, strerror(errno));
                    _exit(124);
                }
                if (accepted_fd > STDOUT_FILENO)
                    close(accepted_fd);
            }
            if (child_fd != 4)
            {
                if (dup2(child_fd, 4) < 0)
                {
                    fprintf(stderr, "child-preexec: config dup2 failed: errno=%d %s\n",
                            errno, strerror(errno));
                    _exit(125);
                }
                close(child_fd);
                child_fd = 4;
            }
            close_from_fd(5);
            if (use_sshdsession)
                execv("/usr/lib/ssh/sshd-session", sshd_session_argv);
            else
                execv(use_otherexe ? get_other_exe_path(argv[0], other_exe, sizeof(other_exe)) : argv[0],
                      child_argv);
            fprintf(stderr, "child-preexec: execv failed: errno=%d %s\n",
                    errno, strerror(errno));
            _exit(127);
        }
    }
    else if (use_spawn)
    {
        int ret = posix_spawn(&pid, argv[0], NULL, NULL, child_argv, environ);
        if (ret != 0)
        {
            fprintf(stderr, "parent: posix_spawn failed: errno=%d %s\n",
                    ret, strerror(ret));
            return 13;
        }
    }
    else
    {
        pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "parent: fork failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 13;
        }
        if (pid == 0)
        {
            execv(argv[0], child_argv);
            fprintf(stderr, "child: execv failed: errno=%d %s\n",
                    errno, strerror(errno));
            _exit(127);
        }
    }

    close(fds[1]);
    if (accepted_fd >= 0)
        close(accepted_fd);

    len = (uint32_t)(sizeof(payload) - 1);
    if (use_writerfork)
    {
        writer_pid = fork();
        if (writer_pid < 0)
        {
            fprintf(stderr, "parent: writer fork failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 21;
        }
        if (writer_pid == 0)
        {
            fprintf(stderr, "writer: writing fd=%d length=%u\n", fds[0], len);
            if (use_bigmsg)
            {
                if (write_message(fds[0], "writer", 'C', 3320, use_bemsg) < 0)
                    _exit(14);
            }
            else if (write_full(fds[0], &len, sizeof(len)) < 0 ||
                     write_full(fds[0], payload, len) < 0)
                _exit(14);
            if (use_twomsg)
            {
                const char payload2[] = "hostkeys-ok";

                if (use_bigmsg)
                {
                    if (write_message(fds[0], "writer second", 'K', 8192, use_bemsg) < 0)
                        _exit(24);
                }
                else
                {
                    uint32_t len2 = (uint32_t)(sizeof(payload2) - 1);

                    fprintf(stderr, "writer: writing second length=%u\n", len2);
                    if (write_full(fds[0], &len2, sizeof(len2)) < 0 ||
                        write_full(fds[0], payload2, len2) < 0)
                        _exit(24);
                }
            }
            fprintf(stderr, "writer: done\n");
            _exit(0);
        }
        fprintf(stderr, "parent: spawned pid=%ld writer_pid=%ld length=%u\n",
                (long)pid, (long)writer_pid, len);
    }
    else
    {
        fprintf(stderr, "parent: spawned pid=%ld writing length=%u\n",
                (long)pid, len);
        if (use_bigmsg)
        {
            if (write_message(fds[0], "parent", 'C', 3320, use_bemsg) < 0)
                return 14;
        }
        else if (write_full(fds[0], &len, sizeof(len)) < 0 ||
                 write_full(fds[0], payload, len) < 0)
            return 14;
        if (use_twomsg)
        {
            const char payload2[] = "hostkeys-ok";

            if (use_bigmsg)
            {
                if (write_message(fds[0], "parent second", 'K', 8192, use_bemsg) < 0)
                    return 24;
            }
            else
            {
                uint32_t len2 = (uint32_t)(sizeof(payload2) - 1);

                if (write_full(fds[0], &len2, sizeof(len2)) < 0 ||
                    write_full(fds[0], payload2, len2) < 0)
                    return 24;
            }
        }
    }

    close(fds[0]);
    if (client_fd >= 0)
        close(client_fd);

    if (waitpid(pid, &status, 0) < 0)
    {
        fprintf(stderr, "parent: waitpid failed: errno=%d %s\n",
                errno, strerror(errno));
        return 15;
    }

    fprintf(stderr, "parent: child status=0x%x\n", status);
    if (writer_pid > 0)
    {
        int writer_status;
        if (waitpid(writer_pid, &writer_status, 0) < 0)
        {
            fprintf(stderr, "parent: writer waitpid failed: errno=%d %s\n",
                    errno, strerror(errno));
            return 22;
        }
        fprintf(stderr, "parent: writer status=0x%x\n", writer_status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 16;
}

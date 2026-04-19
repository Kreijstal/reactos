/*
 * smbclient.exe — libsmb2-backed SMB2/3 client for ReactOS.
 * Supports: ls <smb-url>
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

static int
do_list(struct smb2_context *smb2, struct smb2_url *url)
{
    struct smb2dir *dir;
    struct smb2dirent *ent;

    dir = smb2_opendir(smb2, url->path ? url->path : "");
    if (!dir) {
        fprintf(stderr, "smb2_opendir failed: %s\n", smb2_get_error(smb2));
        return 1;
    }

    while ((ent = smb2_readdir(smb2, dir)) != NULL) {
        const char *tag = "?";
        switch (ent->st.smb2_type) {
            case SMB2_TYPE_DIRECTORY: tag = "<DIR>"; break;
            case SMB2_TYPE_FILE:      tag = "     "; break;
            case SMB2_TYPE_LINK:      tag = "<LNK>"; break;
        }
        printf("%s  %12llu  %s\n",
               tag,
               (unsigned long long)ent->st.smb2_size,
               ent->name);
    }

    smb2_closedir(smb2, dir);
    return 0;
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: smbclient ls  smb://[domain;][user[:pass]@]host[:port]/share[/path]\n"
        "       smbclient cat smb://.../share/path/to/file\n");
    ExitProcess(2);
}

static int
do_cat(struct smb2_context *smb2, struct smb2_url *url)
{
    struct smb2fh *fh;
    char buf[32768];
    int n;

    fh = smb2_open(smb2, url->path ? url->path : "", O_RDONLY);
    if (!fh) {
        fprintf(stderr, "smb2_open failed: %s\n", smb2_get_error(smb2));
        return 1;
    }

    while ((n = smb2_read(smb2, fh, (uint8_t *)buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }

    smb2_close(smb2, fh);
    return n < 0 ? 1 : 0;
}

int
main(int argc, char **argv)
{
    WSADATA wsadata;
    struct smb2_context *smb2;
    struct smb2_url *url;
    int rc;

    if (argc < 3) {
        usage();
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    smb2 = smb2_init_context();
    if (!smb2) {
        fprintf(stderr, "smb2_init_context failed\n");
        WSACleanup();
        return 1;
    }

    url = smb2_parse_url(smb2, argv[2]);
    if (!url) {
        fprintf(stderr, "smb2_parse_url failed: %s\n", smb2_get_error(smb2));
        smb2_destroy_context(smb2);
        WSACleanup();
        return 1;
    }

    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (smb2_connect_share(smb2,
                           url->server,
                           url->share,
                           url->user) != 0) {
        fprintf(stderr, "smb2_connect_share(%s,%s) failed: %s\n",
                url->server, url->share, smb2_get_error(smb2));
        smb2_destroy_url(url);
        smb2_destroy_context(smb2);
        WSACleanup();
        return 1;
    }

    if (strcmp(argv[1], "ls") == 0) {
        rc = do_list(smb2, url);
    } else if (strcmp(argv[1], "cat") == 0) {
        rc = do_cat(smb2, url);
    } else {
        usage();
        rc = 2;
    }

    smb2_disconnect_share(smb2);
    smb2_destroy_url(url);
    smb2_destroy_context(smb2);
    WSACleanup();
    return rc;
}

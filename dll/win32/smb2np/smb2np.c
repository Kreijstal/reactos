/*
 * SMB2/3 Network Provider for ReactOS, backed by libsmb2 in usermode.
 * Implements the mpr.dll NP contract (NPAddConnection3 etc.) so that
 * `net use Z: \\host\share` can route through libsmb2 without touching
 * the kernel SMB redirector.
 */

#define WIN32_NO_STATUS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winnetwk.h>
#include <npapi.h>
#include <lmcons.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(smb2np);

#define WNNC_NET_SMB2 WNNC_NET_LANMAN

#define SMB2NP_MAX_CONNECTIONS 32

typedef struct _SMB2NP_CONN {
    BOOL                 in_use;
    WCHAR                local_name[8];          /* "Z:" or empty */
    WCHAR                remote_name[MAX_PATH];  /* "\\\\host\\share" */
    WCHAR                user_name[UNLEN + 1];
    struct smb2_context *smb2;
} SMB2NP_CONN;

static SMB2NP_CONN      g_conns[SMB2NP_MAX_CONNECTIONS];
static CRITICAL_SECTION g_lock;
static BOOL             g_wsa_started;

/* ---------- small helpers ---------- */

static char *
wide_to_utf8(LPCWSTR w)
{
    int n;
    char *s;

    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static void
utf8_free(char *s)
{
    if (s) HeapFree(GetProcessHeap(), 0, s);
}

/*
 * Parse "\\\\host\\share[\\path]" (UNC) into host/share/path components.
 * All outputs are heap-allocated UTF-8; caller utf8_free()s them.
 * path may come back NULL (no trailing component).
 */
static BOOL
parse_unc(LPCWSTR remote, char **host, char **share, char **path)
{
    char *u;
    char *p, *q;

    *host = *share = *path = NULL;

    u = wide_to_utf8(remote);
    if (!u) return FALSE;

    /* Expect \\host\share[\path] */
    if (u[0] != '\\' || u[1] != '\\') {
        utf8_free(u);
        return FALSE;
    }
    p = u + 2;
    q = strchr(p, '\\');
    if (!q) {
        utf8_free(u);
        return FALSE;
    }
    *q = '\0';
    *host = _strdup(p);
    p = q + 1;

    q = strchr(p, '\\');
    if (q) {
        *q = '\0';
        *share = _strdup(p);
        *path  = _strdup(q + 1);
    } else {
        *share = _strdup(p);
        *path  = NULL;
    }
    utf8_free(u);
    return (*host && *share);
}

static SMB2NP_CONN *
find_conn_by_local(LPCWSTR local)
{
    int i;
    if (!local || !local[0]) return NULL;
    for (i = 0; i < SMB2NP_MAX_CONNECTIONS; i++) {
        if (g_conns[i].in_use &&
            _wcsicmp(g_conns[i].local_name, local) == 0)
            return &g_conns[i];
    }
    return NULL;
}

static SMB2NP_CONN *
alloc_conn(void)
{
    int i;
    for (i = 0; i < SMB2NP_MAX_CONNECTIONS; i++) {
        if (!g_conns[i].in_use) {
            memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].in_use = TRUE;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void
free_conn(SMB2NP_CONN *c)
{
    if (c->smb2) {
        smb2_disconnect_share(c->smb2);
        smb2_destroy_context(c->smb2);
        c->smb2 = NULL;
    }
    c->in_use = FALSE;
}

static void
ensure_wsa(void)
{
    WSADATA wsa;
    if (!g_wsa_started) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
            g_wsa_started = TRUE;
    }
}

/* ---------- NP exports ---------- */

DWORD WINAPI
NPGetCaps(DWORD nIndex)
{
    switch (nIndex) {
        case WNNC_SPEC_VERSION:    return WNNC_SPEC_VERSION51;
        case WNNC_NET_TYPE:        return WNNC_NET_SMB2;
        case WNNC_DRIVER_VERSION:  return 1;
        case WNNC_USER:            return WNNC_USR_GETUSER;
        case WNNC_CONNECTION:
            return WNNC_CON_ADDCONNECTION
                 | WNNC_CON_ADDCONNECTION3
                 | WNNC_CON_CANCELCONNECTION
                 | WNNC_CON_GETCONNECTIONS;
        case WNNC_ENUMERATION:     return WNNC_ENUM_GLOBAL;
        case WNNC_START:           return 1;
        default:                   return 0;
    }
}

DWORD WINAPI
NPAddConnection3(HWND hwndOwner,
                 LPNETRESOURCEW lpNetResource,
                 LPWSTR lpPassword,
                 LPWSTR lpUserName,
                 DWORD dwFlags)
{
    SMB2NP_CONN *c;
    struct smb2_context *smb2;
    char *host = NULL, *share = NULL, *path = NULL;
    char *u_user = NULL, *u_pass = NULL;
    DWORD rc = WN_SUCCESS;

    (void)hwndOwner; (void)dwFlags;

    if (!lpNetResource || !lpNetResource->lpRemoteName)
        return WN_BAD_POINTER;

    if (lpNetResource->dwType != RESOURCETYPE_DISK &&
        lpNetResource->dwType != RESOURCETYPE_ANY)
        return WN_BAD_VALUE;

    TRACE("remote=%s local=%s\n",
          debugstr_w(lpNetResource->lpRemoteName),
          debugstr_w(lpNetResource->lpLocalName));

    if (!parse_unc(lpNetResource->lpRemoteName, &host, &share, &path))
        return WN_BAD_NETNAME;

    ensure_wsa();

    smb2 = smb2_init_context();
    if (!smb2) { rc = WN_OUT_OF_MEMORY; goto out; }

    if (lpUserName && lpUserName[0]) {
        u_user = wide_to_utf8(lpUserName);
        if (u_user) smb2_set_user(smb2, u_user);
    }
    if (lpPassword && lpPassword[0]) {
        u_pass = wide_to_utf8(lpPassword);
        if (u_pass) smb2_set_password(smb2, u_pass);
    }

    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (smb2_connect_share(smb2, host, share, u_user) != 0) {
        WARN("smb2_connect_share(%s,%s) failed: %s\n",
             host, share, smb2_get_error(smb2));
        smb2_destroy_context(smb2);
        rc = WN_BAD_NETNAME;
        goto out;
    }

    EnterCriticalSection(&g_lock);
    c = alloc_conn();
    if (!c) {
        LeaveCriticalSection(&g_lock);
        smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        rc = WN_OUT_OF_MEMORY;
        goto out;
    }
    c->smb2 = smb2;
    if (lpNetResource->lpLocalName && lpNetResource->lpLocalName[0]) {
        lstrcpynW(c->local_name, lpNetResource->lpLocalName,
                  sizeof(c->local_name) / sizeof(WCHAR));
    }
    lstrcpynW(c->remote_name, lpNetResource->lpRemoteName,
              sizeof(c->remote_name) / sizeof(WCHAR));
    if (lpUserName) {
        lstrcpynW(c->user_name, lpUserName,
                  sizeof(c->user_name) / sizeof(WCHAR));
    }
    LeaveCriticalSection(&g_lock);

out:
    utf8_free(host); utf8_free(share); utf8_free(path);
    utf8_free(u_user); utf8_free(u_pass);
    return rc;
}

DWORD WINAPI
NPAddConnection(LPNETRESOURCEW lpNetResource,
                LPWSTR lpPassword,
                LPWSTR lpUserName)
{
    return NPAddConnection3(NULL, lpNetResource, lpPassword, lpUserName, 0);
}

DWORD WINAPI
NPCancelConnection(LPWSTR lpName, BOOL fForce)
{
    SMB2NP_CONN *c;
    (void)fForce;

    if (!lpName) return WN_BAD_POINTER;

    EnterCriticalSection(&g_lock);
    c = find_conn_by_local(lpName);
    if (!c) {
        /* try remote-name match */
        int i;
        for (i = 0; i < SMB2NP_MAX_CONNECTIONS; i++) {
            if (g_conns[i].in_use &&
                _wcsicmp(g_conns[i].remote_name, lpName) == 0) {
                c = &g_conns[i];
                break;
            }
        }
    }
    if (!c) {
        LeaveCriticalSection(&g_lock);
        return WN_NOT_CONNECTED;
    }
    free_conn(c);
    LeaveCriticalSection(&g_lock);
    return WN_SUCCESS;
}

DWORD WINAPI
NPGetConnection(LPWSTR lpLocalName,
                LPVOID lpRemoteName,
                LPDWORD lpBufferSize)
{
    SMB2NP_CONN *c;
    DWORD needed;

    if (!lpLocalName || !lpBufferSize) return WN_BAD_POINTER;

    EnterCriticalSection(&g_lock);
    c = find_conn_by_local(lpLocalName);
    if (!c) {
        LeaveCriticalSection(&g_lock);
        return WN_NOT_CONNECTED;
    }
    needed = (lstrlenW(c->remote_name) + 1) * sizeof(WCHAR);
    if (*lpBufferSize < needed) {
        *lpBufferSize = needed;
        LeaveCriticalSection(&g_lock);
        return WN_MORE_DATA;
    }
    memcpy(lpRemoteName, c->remote_name, needed);
    *lpBufferSize = needed;
    LeaveCriticalSection(&g_lock);
    return WN_SUCCESS;
}

DWORD WINAPI
NPGetUser(LPWSTR lpName,
          LPVOID lpUserName,
          LPDWORD lpBufferSize)
{
    SMB2NP_CONN *c;
    DWORD needed;
    LPCWSTR src = NULL;

    if (!lpBufferSize) return WN_BAD_POINTER;

    EnterCriticalSection(&g_lock);
    if (lpName && lpName[0]) {
        c = find_conn_by_local(lpName);
        if (c && c->user_name[0]) src = c->user_name;
    }
    if (!src) {
        LeaveCriticalSection(&g_lock);
        return WN_NOT_CONNECTED;
    }
    needed = (lstrlenW(src) + 1) * sizeof(WCHAR);
    if (*lpBufferSize < needed) {
        *lpBufferSize = needed;
        LeaveCriticalSection(&g_lock);
        return WN_MORE_DATA;
    }
    memcpy(lpUserName, src, needed);
    *lpBufferSize = needed;
    LeaveCriticalSection(&g_lock);
    return WN_SUCCESS;
}

/* ---------- enumeration (Phase-2 scope: active connections only) ---------- */

typedef struct _SMB2NP_ENUM {
    DWORD scope;
    DWORD type;
    DWORD index;
} SMB2NP_ENUM;

DWORD WINAPI
NPOpenEnum(DWORD dwScope,
           DWORD dwType,
           DWORD dwUsage,
           LPNETRESOURCEW lpNetResource,
           LPHANDLE lphEnum)
{
    SMB2NP_ENUM *e;
    (void)dwUsage; (void)lpNetResource;

    if (!lphEnum) return WN_BAD_POINTER;
    if (dwScope != RESOURCE_CONNECTED && dwScope != RESOURCE_GLOBALNET)
        return WN_NOT_SUPPORTED;

    e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e));
    if (!e) return WN_OUT_OF_MEMORY;
    e->scope = dwScope;
    e->type  = dwType;
    *lphEnum = (HANDLE)e;
    return WN_SUCCESS;
}

DWORD WINAPI
NPEnumResource(HANDLE hEnum,
               LPDWORD lpcCount,
               LPVOID lpBuffer,
               LPDWORD lpBufferSize)
{
    SMB2NP_ENUM *e = (SMB2NP_ENUM *)hEnum;
    NETRESOURCEW *nr = (NETRESOURCEW *)lpBuffer;
    char *string_tail;
    DWORD want_count, got = 0;
    DWORD nr_size;

    if (!e || !lpcCount || !lpBufferSize) return WN_BAD_POINTER;
    if (e->scope != RESOURCE_CONNECTED) return WN_NO_MORE_ENTRIES;

    want_count = *lpcCount;
    nr_size    = *lpBufferSize;
    string_tail = (char *)lpBuffer + want_count * sizeof(NETRESOURCEW);

    EnterCriticalSection(&g_lock);
    while (got < want_count && e->index < SMB2NP_MAX_CONNECTIONS) {
        SMB2NP_CONN *c = &g_conns[e->index++];
        DWORD rlen, llen;

        if (!c->in_use) continue;

        rlen = (lstrlenW(c->remote_name) + 1) * sizeof(WCHAR);
        llen = (lstrlenW(c->local_name)  + 1) * sizeof(WCHAR);

        if ((DWORD)((char *)string_tail - (char *)lpBuffer) + rlen + llen
            > nr_size) {
            if (got == 0) {
                *lpBufferSize = (got + 1) * sizeof(NETRESOURCEW)
                              + rlen + llen;
                LeaveCriticalSection(&g_lock);
                return WN_MORE_DATA;
            }
            break;
        }

        memset(&nr[got], 0, sizeof(nr[got]));
        nr[got].dwScope       = RESOURCE_CONNECTED;
        nr[got].dwType        = RESOURCETYPE_DISK;
        nr[got].dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
        nr[got].dwUsage       = RESOURCEUSAGE_CONNECTABLE;
        if (c->local_name[0]) {
            memcpy(string_tail, c->local_name, llen);
            nr[got].lpLocalName = (LPWSTR)string_tail;
            string_tail += llen;
        }
        memcpy(string_tail, c->remote_name, rlen);
        nr[got].lpRemoteName = (LPWSTR)string_tail;
        string_tail += rlen;
        got++;
    }
    LeaveCriticalSection(&g_lock);

    *lpcCount = got;
    return got == 0 ? WN_NO_MORE_ENTRIES : WN_SUCCESS;
}

DWORD WINAPI
NPCloseEnum(HANDLE hEnum)
{
    if (hEnum) HeapFree(GetProcessHeap(), 0, hEnum);
    return WN_SUCCESS;
}

/* ---------- unsupported leaf entry points ---------- */

DWORD WINAPI
NPGetResourceInformation(LPNETRESOURCEW lpNetResource,
                         LPVOID lpBuffer,
                         LPDWORD lpBufferSize,
                         LPWSTR *lplpSystem)
{
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI
NPGetResourceParent(LPNETRESOURCEW lpNetResource,
                    LPVOID lpBuffer,
                    LPDWORD lpBufferSize)
{
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI
NPFormatNetworkName(LPWSTR a, LPWSTR b, LPDWORD c,
                    DWORD d, DWORD e)
{
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI
NPPropertyDialog(HWND a, DWORD b, DWORD c,
                 LPWSTR d, DWORD e)
{
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI
NPGetDirectoryType(LPWSTR a, LPINT b, BOOL c)
{
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI
NPDirectoryNotify(HWND a, LPWSTR b, DWORD c)
{
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI
NPGetUniversalName(LPCWSTR a, DWORD b,
                   LPVOID c, LPDWORD d)
{
    return WN_NOT_SUPPORTED;
}

/* ---------- DllMain ---------- */

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)lpvReserved;
    switch (dwReason) {
        case DLL_PROCESS_ATTACH:
            InitializeCriticalSection(&g_lock);
            break;
        case DLL_PROCESS_DETACH: {
            int i;
            if (lpvReserved) break;  /* fast path on process exit */
            EnterCriticalSection(&g_lock);
            for (i = 0; i < SMB2NP_MAX_CONNECTIONS; i++)
                if (g_conns[i].in_use) free_conn(&g_conns[i]);
            LeaveCriticalSection(&g_lock);
            DeleteCriticalSection(&g_lock);
            if (g_wsa_started) WSACleanup();
            break;
        }
    }
    return TRUE;
}

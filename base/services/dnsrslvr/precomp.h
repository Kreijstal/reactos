#ifndef _DNSRSLVR_PCH_
#define _DNSRSLVR_PCH_

#include <stdarg.h>
#include <stdio.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winsvc.h>
#include <windns.h>
#include <windns_undoc.h>

#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <ndk/obfuncs.h>

#include <dnsrslvr_s.h>

#include <strsafe.h>

typedef struct _RESOLVER_CACHE_ENTRY
{
    LIST_ENTRY CacheLink;
    BOOL bHostsFileEntry;
    PDNS_RECORDW Record;
} RESOLVER_CACHE_ENTRY, *PRESOLVER_CACHE_ENTRY;

typedef struct _RESOLVER_CACHE
{
    LIST_ENTRY RecordList;
    CRITICAL_SECTION Lock;
} RESOLVER_CACHE, *PRESOLVER_CACHE;

/*
 * Negative DNS cache entry.
 *
 * Holds the failure status for a (name, type) pair that recently failed to
 * resolve, so that repeated lookups of the same unresolvable name short-circuit
 * the wire path. Matches the Windows DNS Client Service "negative cache"
 * (registry-tunable MaxNegativeCacheTtl, default 900 seconds per Microsoft's
 * "DNS Client Resolver Cache" documentation; see KB286834).
 */
typedef struct _RESOLVER_NEG_CACHE_ENTRY
{
    LIST_ENTRY CacheLink;
    LPWSTR Name;
    WORD wType;
    DNS_STATUS Status;
    ULONGLONG ExpireTickMs;
} RESOLVER_NEG_CACHE_ENTRY, *PRESOLVER_NEG_CACHE_ENTRY;

typedef struct _RESOLVER_NEG_CACHE
{
    LIST_ENTRY RecordList;
    ULONG EntryCount;
    CRITICAL_SECTION Lock;
} RESOLVER_NEG_CACHE, *PRESOLVER_NEG_CACHE;

/*
 * TTL for negative entries, in milliseconds. 900 s matches the documented
 * Microsoft default for MaxNegativeCacheTtl.
 */
#define NEGATIVE_CACHE_TTL_MS  (900 * 1000)

/* Hard cap on the number of negative entries to bound memory; FIFO eviction. */
#define NEGATIVE_CACHE_CAP     64


/* cache.c */

VOID DnsIntCacheInitialize(VOID);
VOID DnsIntCacheRemoveEntryItem(PRESOLVER_CACHE_ENTRY CacheEntry);
VOID DnsIntCacheFree(VOID);

VOID DnsIntNegativeCacheInitialize(VOID);
VOID DnsIntNegativeCacheFree(VOID);
VOID DnsIntNegativeCacheFlush(VOID);
BOOL DnsIntNegativeCacheRemoveEntry(_In_ LPCWSTR Name, _In_ WORD wType);

DNS_STATUS
DnsIntNegativeCacheLookup(
    _In_ LPCWSTR Name,
    _In_ WORD wType);

VOID
DnsIntNegativeCacheInsert(
    _In_ LPCWSTR Name,
    _In_ WORD wType,
    _In_ DNS_STATUS Status);

#define CACHE_FLUSH_HOSTS_FILE_ENTRIES     0x00000001
#define CACHE_FLUSH_NON_HOSTS_FILE_ENTRIES 0x00000002
#define CACHE_FLUSH_ALL                    0x00000003

DNS_STATUS
DnsIntCacheFlush(
    _In_ ULONG ulFlags);

DNS_STATUS
DnsIntFlushCacheEntry(
    _In_ LPCWSTR pszName,
    _In_ WORD wType);

DNS_STATUS
DnsIntCacheGetEntryByName(
    LPCWSTR Name,
    WORD wType,
    DWORD dwFlags,
    PDNS_RECORDW *Record);

VOID
DnsIntCacheAddEntry(
    _In_ PDNS_RECORDW Record,
    _In_ BOOL bHostsFileEntry);

BOOL
DnsIntCacheRemoveEntryByName(
    _In_ LPCWSTR Name);

DNS_STATUS
DnsIntCacheGetEntries(
    _Out_ DNS_CACHE_ENTRY **ppCacheEntries);


/* hostsfile.c */

BOOL
ReadHostsFile(VOID);

#endif /* _DNSRSLVR_PCH_ */

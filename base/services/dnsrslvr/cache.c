/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS system libraries
 * FILE:        base/services/dnsrslvr/cache.c
 * PURPOSE:     DNS cache functions
 * PROGRAMMER:  Peter Hater
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static RESOLVER_CACHE DnsCache;
static BOOL DnsCacheInitialized = FALSE;

#define DnsCacheLock()          do { EnterCriticalSection(&DnsCache.Lock); } while (0)
#define DnsCacheUnlock()        do { LeaveCriticalSection(&DnsCache.Lock); } while (0)

static RESOLVER_NEG_CACHE DnsNegCache;
static BOOL DnsNegCacheInitialized = FALSE;

#define DnsNegCacheLock()       do { EnterCriticalSection(&DnsNegCache.Lock); } while (0)
#define DnsNegCacheUnlock()     do { LeaveCriticalSection(&DnsNegCache.Lock); } while (0)

static
VOID
DnsIntNegativeCacheRemoveEntryItem(
    _In_ PRESOLVER_NEG_CACHE_ENTRY Entry)
{
    RemoveEntryList(&Entry->CacheLink);
    if (Entry->Name)
        HeapFree(GetProcessHeap(), 0, Entry->Name);
    HeapFree(GetProcessHeap(), 0, Entry);
    ASSERT(DnsNegCache.EntryCount > 0);
    DnsNegCache.EntryCount--;
}

/* Caller must hold DnsNegCacheLock. */
static
VOID
DnsIntNegativeCachePurgeExpired(
    _In_ ULONGLONG NowMs)
{
    PLIST_ENTRY Entry, Next;
    PRESOLVER_NEG_CACHE_ENTRY NegEntry;

    Entry = DnsNegCache.RecordList.Flink;
    while (Entry != &DnsNegCache.RecordList)
    {
        Next = Entry->Flink;
        NegEntry = CONTAINING_RECORD(Entry, RESOLVER_NEG_CACHE_ENTRY, CacheLink);
        if (NowMs >= NegEntry->ExpireTickMs)
            DnsIntNegativeCacheRemoveEntryItem(NegEntry);
        Entry = Next;
    }
}

VOID
DnsIntCacheInitialize(VOID)
{
    DPRINT("DnsIntCacheInitialize()\n");

    /* Check if we're initialized */
    if (DnsCacheInitialized)
        return;

    /* Initialize the cache lock and namespace list */
    InitializeCriticalSection((LPCRITICAL_SECTION)&DnsCache.Lock);
    InitializeListHead(&DnsCache.RecordList);
    DnsCacheInitialized = TRUE;

    DnsIntNegativeCacheInitialize();
}

VOID
DnsIntCacheFree(VOID)
{
    DPRINT("DnsIntCacheFree()\n");

    /* Check if we're initialized */
    if (!DnsCacheInitialized)
        return;

    if (!DnsCache.RecordList.Flink)
        return;

    DnsIntCacheFlush(CACHE_FLUSH_ALL);

    DeleteCriticalSection(&DnsCache.Lock);
    DnsCacheInitialized = FALSE;

    DnsIntNegativeCacheFree();
}

VOID
DnsIntNegativeCacheInitialize(VOID)
{
    if (DnsNegCacheInitialized)
        return;

    InitializeCriticalSection(&DnsNegCache.Lock);
    InitializeListHead(&DnsNegCache.RecordList);
    DnsNegCache.EntryCount = 0;
    DnsNegCacheInitialized = TRUE;
}

VOID
DnsIntNegativeCacheFlush(VOID)
{
    PLIST_ENTRY Entry, Next;
    PRESOLVER_NEG_CACHE_ENTRY NegEntry;

    if (!DnsNegCacheInitialized)
        return;

    DnsNegCacheLock();

    Entry = DnsNegCache.RecordList.Flink;
    while (Entry != &DnsNegCache.RecordList)
    {
        Next = Entry->Flink;
        NegEntry = CONTAINING_RECORD(Entry, RESOLVER_NEG_CACHE_ENTRY, CacheLink);
        DnsIntNegativeCacheRemoveEntryItem(NegEntry);
        Entry = Next;
    }
    ASSERT(DnsNegCache.EntryCount == 0);

    DnsNegCacheUnlock();
}

VOID
DnsIntNegativeCacheFree(VOID)
{
    if (!DnsNegCacheInitialized)
        return;

    DnsIntNegativeCacheFlush();
    DeleteCriticalSection(&DnsNegCache.Lock);
    DnsNegCacheInitialized = FALSE;
}

DNS_STATUS
DnsIntNegativeCacheLookup(
    _In_ LPCWSTR Name,
    _In_ WORD wType)
{
    PLIST_ENTRY Entry;
    PRESOLVER_NEG_CACHE_ENTRY NegEntry;
    DNS_STATUS Status = ERROR_SUCCESS;
    ULONGLONG NowMs;

    if (!DnsNegCacheInitialized || Name == NULL)
        return ERROR_SUCCESS;

    NowMs = GetTickCount64();

    DnsNegCacheLock();
    DnsIntNegativeCachePurgeExpired(NowMs);

    Entry = DnsNegCache.RecordList.Flink;
    while (Entry != &DnsNegCache.RecordList)
    {
        NegEntry = CONTAINING_RECORD(Entry, RESOLVER_NEG_CACHE_ENTRY, CacheLink);
        /* Cache key is (name, type) case-folded, matching Windows DNS lookup
         * semantics; _wcsicmp handles the case fold. */
        if (NegEntry->wType == wType &&
            _wcsicmp(NegEntry->Name, Name) == 0)
        {
            /* Successful queries must never be cached as negatives. */
            ASSERT(NegEntry->Status != ERROR_SUCCESS);
            Status = NegEntry->Status;
            break;
        }
        Entry = Entry->Flink;
    }

    DnsNegCacheUnlock();
    return Status;
}

VOID
DnsIntNegativeCacheInsert(
    _In_ LPCWSTR Name,
    _In_ WORD wType,
    _In_ DNS_STATUS Status)
{
    PLIST_ENTRY Entry;
    PRESOLVER_NEG_CACHE_ENTRY NegEntry;
    SIZE_T NameLen;
    ULONGLONG NowMs;

    /* Only failures belong in the negative cache. */
    ASSERT(Status != ERROR_SUCCESS);
    if (Status == ERROR_SUCCESS)
        return;

    if (!DnsNegCacheInitialized || Name == NULL)
        return;

    NowMs = GetTickCount64();

    DnsNegCacheLock();
    DnsIntNegativeCachePurgeExpired(NowMs);

    /* Refresh an existing entry rather than duplicate. */
    Entry = DnsNegCache.RecordList.Flink;
    while (Entry != &DnsNegCache.RecordList)
    {
        NegEntry = CONTAINING_RECORD(Entry, RESOLVER_NEG_CACHE_ENTRY, CacheLink);
        if (NegEntry->wType == wType &&
            _wcsicmp(NegEntry->Name, Name) == 0)
        {
            NegEntry->Status = Status;
            ASSERT(NowMs + NEGATIVE_CACHE_TTL_MS >= NegEntry->ExpireTickMs);
            NegEntry->ExpireTickMs = NowMs + NEGATIVE_CACHE_TTL_MS;
            DnsNegCacheUnlock();
            return;
        }
        Entry = Entry->Flink;
    }

    /* Evict oldest entries until we are below the cap. */
    while (DnsNegCache.EntryCount >= NEGATIVE_CACHE_CAP &&
           !IsListEmpty(&DnsNegCache.RecordList))
    {
        NegEntry = CONTAINING_RECORD(DnsNegCache.RecordList.Flink,
                                     RESOLVER_NEG_CACHE_ENTRY,
                                     CacheLink);
        DnsIntNegativeCacheRemoveEntryItem(NegEntry);
    }

    NameLen = wcslen(Name);
    NegEntry = HeapAlloc(GetProcessHeap(), 0, sizeof(*NegEntry));
    if (NegEntry == NULL)
    {
        DnsNegCacheUnlock();
        return;
    }
    NegEntry->Name = HeapAlloc(GetProcessHeap(), 0, (NameLen + 1) * sizeof(WCHAR));
    if (NegEntry->Name == NULL)
    {
        HeapFree(GetProcessHeap(), 0, NegEntry);
        DnsNegCacheUnlock();
        return;
    }
    StringCchCopyW(NegEntry->Name, NameLen + 1, Name);
    NegEntry->wType = wType;
    NegEntry->Status = Status;
    NegEntry->ExpireTickMs = NowMs + NEGATIVE_CACHE_TTL_MS;

    InsertTailList(&DnsNegCache.RecordList, &NegEntry->CacheLink);
    DnsNegCache.EntryCount++;

    DnsNegCacheUnlock();
}

BOOL
DnsIntNegativeCacheRemoveEntry(
    _In_ LPCWSTR Name,
    _In_ WORD wType)
{
    PLIST_ENTRY Entry;
    PRESOLVER_NEG_CACHE_ENTRY NegEntry;
    BOOL Removed = FALSE;

    if (!DnsNegCacheInitialized || Name == NULL)
        return FALSE;

    DnsNegCacheLock();

    Entry = DnsNegCache.RecordList.Flink;
    while (Entry != &DnsNegCache.RecordList)
    {
        NegEntry = CONTAINING_RECORD(Entry, RESOLVER_NEG_CACHE_ENTRY, CacheLink);
        Entry = Entry->Flink;
        if (_wcsicmp(NegEntry->Name, Name) == 0 &&
            (wType == DNS_TYPE_ANY || NegEntry->wType == wType))
        {
            DnsIntNegativeCacheRemoveEntryItem(NegEntry);
            Removed = TRUE;
        }
    }

    DnsNegCacheUnlock();
    return Removed;
}

VOID
DnsIntCacheRemoveEntryItem(PRESOLVER_CACHE_ENTRY CacheEntry)
{
    DPRINT("DnsIntCacheRemoveEntryItem(%p)\n", CacheEntry);

    /* Remove the entry from the list */
    RemoveEntryList(&CacheEntry->CacheLink);

    /* Free record */
    DnsRecordListFree(CacheEntry->Record, DnsFreeRecordList);

    /* Delete us */
    HeapFree(GetProcessHeap(), 0, CacheEntry);
}

DNS_STATUS
DnsIntCacheFlush(
    _In_ ULONG ulFlags)
{
    PLIST_ENTRY Entry, NextEntry;
    PRESOLVER_CACHE_ENTRY CacheEntry;

    DPRINT("DnsIntCacheFlush(%lu)\n", ulFlags);

    /* Lock the cache */
    DnsCacheLock();

    /* Loop every entry */
    Entry = DnsCache.RecordList.Flink;
    while (Entry != &DnsCache.RecordList)
    {
        NextEntry = Entry->Flink;

        /* Get this entry */
        CacheEntry = CONTAINING_RECORD(Entry, RESOLVER_CACHE_ENTRY, CacheLink);

        /* Remove it from list */
        if (((ulFlags & CACHE_FLUSH_HOSTS_FILE_ENTRIES) && (CacheEntry->bHostsFileEntry != FALSE)) ||
            ((ulFlags & CACHE_FLUSH_NON_HOSTS_FILE_ENTRIES) && (CacheEntry->bHostsFileEntry == FALSE)))
            DnsIntCacheRemoveEntryItem(CacheEntry);

        /* Move to the next entry */
        Entry = NextEntry;
    }

    /* Unlock the cache */
    DnsCacheUnlock();

    /* DnsFlushCache callers expect *all* resolver state for non-hosts entries
     * to be wiped; the negative cache is inherently non-hosts. */
    if (ulFlags & CACHE_FLUSH_NON_HOSTS_FILE_ENTRIES)
        DnsIntNegativeCacheFlush();

    return ERROR_SUCCESS;
}


DNS_STATUS
DnsIntFlushCacheEntry(
    _In_ LPCWSTR pszName,
    _In_ WORD wType)
{
    PLIST_ENTRY Entry, NextEntry;
    PRESOLVER_CACHE_ENTRY CacheEntry;

    DPRINT("DnsIntFlushCacheEntry(%S %x)\n", pszName, wType);

    /* Lock the cache */
    DnsCacheLock();

    /* Loop every entry */
    Entry = DnsCache.RecordList.Flink;
    while (Entry != &DnsCache.RecordList)
    {
        NextEntry = Entry->Flink;

        /* Get this entry */
        CacheEntry = CONTAINING_RECORD(Entry, RESOLVER_CACHE_ENTRY, CacheLink);

        /* Remove it from the list */
        if ((_wcsicmp(CacheEntry->Record->pName, pszName) == 0) &&
            (CacheEntry->bHostsFileEntry == FALSE))
        {
            if ((wType == DNS_TYPE_ANY) ||
                (CacheEntry->Record->wType == wType))
            {
                DnsIntCacheRemoveEntryItem(CacheEntry);
            }
        }

        /* Move to the next entry */
        Entry = NextEntry;
    }

    /* Unlock the cache */
    DnsCacheUnlock();

    /* Also evict any negative entry for the same name. */
    DnsIntNegativeCacheRemoveEntry(pszName, wType);

    return ERROR_SUCCESS;
}


DNS_STATUS
DnsIntCacheGetEntryByName(
    LPCWSTR Name,
    WORD wType,
    DWORD dwFlags,
    PDNS_RECORDW *Record)
{
    DNS_STATUS Status = DNS_INFO_NO_RECORDS;
    PRESOLVER_CACHE_ENTRY CacheEntry;
    PLIST_ENTRY NextEntry;

    DPRINT("DnsIntCacheGetEntryByName(%S %hu 0x%lx %p)\n",
           Name, wType, dwFlags, Record);

    /* Assume failure */
    *Record = NULL;

    /* Lock the cache */
    DnsCacheLock();

    /* Match the Id with all the entries in the List */
    NextEntry = DnsCache.RecordList.Flink;
    while (NextEntry != &DnsCache.RecordList)
    {
        /* Get the Current Entry */
        CacheEntry = CONTAINING_RECORD(NextEntry, RESOLVER_CACHE_ENTRY, CacheLink);

        /* Check if this is the Catalog Entry ID we want */
        if (_wcsicmp(CacheEntry->Record->pName, Name) == 0)
        {
            /* Copy the entry and return it */
            *Record = DnsRecordSetCopyEx(CacheEntry->Record, DnsCharSetUnicode, DnsCharSetUnicode);
            Status = ERROR_SUCCESS;
            break;
        }

        NextEntry = NextEntry->Flink;
    }

    /* Release the cache */
    DnsCacheUnlock();

    return Status;
}

BOOL
DnsIntCacheRemoveEntryByName(LPCWSTR Name)
{
    BOOL Ret = FALSE;
    PRESOLVER_CACHE_ENTRY CacheEntry;
    PLIST_ENTRY NextEntry;

    DPRINT("DnsIntCacheRemoveEntryByName(%S)\n", Name);

    /* Lock the cache */
    DnsCacheLock();

    /* Match the Id with all the entries in the List */
    NextEntry = DnsCache.RecordList.Flink;
    while (NextEntry != &DnsCache.RecordList)
    {
        /* Get the Current Entry */
        CacheEntry = CONTAINING_RECORD(NextEntry, RESOLVER_CACHE_ENTRY, CacheLink);

        /* Check if this is the Catalog Entry ID we want */
        if (_wcsicmp(CacheEntry->Record->pName, Name) == 0)
        {
            /* Remove the entry */
            DnsIntCacheRemoveEntryItem(CacheEntry);
            Ret = TRUE;
            break;
        }

        NextEntry = NextEntry->Flink;
    }

    /* Release the cache */
    DnsCacheUnlock();

    /* Return */
    return Ret;
}

VOID
DnsIntCacheAddEntry(
    _In_ PDNS_RECORDW Record,
    _In_ BOOL bHostsFileEntry)
{
    PRESOLVER_CACHE_ENTRY Entry;

    DPRINT("DnsIntCacheAddEntry(%p %u)\n",
           Record, bHostsFileEntry);

    DPRINT("Name: %S\n", Record->pName);
    DPRINT("TTL: %lu\n", Record->dwTtl);

    /* Lock the cache */
    DnsCacheLock();

    /* Match the Id with all the entries in the List */
    Entry = (PRESOLVER_CACHE_ENTRY)HeapAlloc(GetProcessHeap(), 0, sizeof(*Entry));
    if (!Entry)
        return;

    Entry->bHostsFileEntry = bHostsFileEntry;
    Entry->Record = DnsRecordSetCopyEx(Record, DnsCharSetUnicode, DnsCharSetUnicode);

    /* Insert it to our List */
    InsertTailList(&DnsCache.RecordList, &Entry->CacheLink);

    /* Release the cache */
    DnsCacheUnlock();
}

DNS_STATUS
DnsIntCacheGetEntries(
    _Out_ DNS_CACHE_ENTRY **ppCacheEntries)
{
    PRESOLVER_CACHE_ENTRY CacheEntry;
    PLIST_ENTRY NextEntry;
    PDNS_CACHE_ENTRY pLastEntry = NULL, pNewEntry;

    /* Lock the cache */
    DnsCacheLock();

    *ppCacheEntries = NULL;

    NextEntry = DnsCache.RecordList.Flink;
    while (NextEntry != &DnsCache.RecordList)
    {
        /* Get the Current Entry */
        CacheEntry = CONTAINING_RECORD(NextEntry, RESOLVER_CACHE_ENTRY, CacheLink);

        DPRINT("1 %S %lu\n", CacheEntry->Record->pName, CacheEntry->Record->wType);
        if (CacheEntry->Record->pNext)
        {
            DPRINT("2 %S %lu\n", CacheEntry->Record->pNext->pName, CacheEntry->Record->pNext->wType);
        }

        pNewEntry = midl_user_allocate(sizeof(DNS_CACHE_ENTRY));
        if (pNewEntry == NULL)
        {
            return ERROR_OUTOFMEMORY;
        }

        pNewEntry->pszName = midl_user_allocate((wcslen(CacheEntry->Record->pName) + 1) * sizeof(WCHAR));
        if (pNewEntry->pszName == NULL)
        {
            return ERROR_OUTOFMEMORY;
        }

        wcscpy(pNewEntry->pszName, CacheEntry->Record->pName);
        pNewEntry->wType1 = CacheEntry->Record->wType;
        pNewEntry->wType2 = 0;
        pNewEntry->wFlags = 0;

        if (pLastEntry == NULL)
            *ppCacheEntries = pNewEntry;
        else
            pLastEntry->pNext = pNewEntry;
        pLastEntry = pNewEntry;

        NextEntry = NextEntry->Flink;
    }

    /* Release the cache */
    DnsCacheUnlock();

    return ERROR_SUCCESS;
}

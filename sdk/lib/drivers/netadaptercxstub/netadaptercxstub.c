/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx client stub: function table, globals and class bind record
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A NetAdapterCx client links this library. It provides the storage the
 * inline API wrappers use (NetFunctions, NetDriverGlobals) and one class
 * bind record in .kmdfclassbind$b. Before DriverEntry runs, the KMDF client
 * stub walks the records between its .kmdfclassbind$a and $c markers and
 * hands each one to wdfldr!WdfVersionBindClass. NetAdapterCx then copies its
 * function table into NetFunctions and stores the client's globals pointer
 * into NetDriverGlobals.
 *
 * Build with NETADAPTER_VERSION_MINOR set to the version the client
 * targets; the record carries that version and its table size.
 */

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>

/* Same layout as WDF_CLASS_BIND_INFO in wdfldr */
typedef struct _NETCX_CLASS_BIND_INFO
{
    ULONG Size;
    PCWSTR ClassName;
    struct
    {
        ULONG Major;
        ULONG Minor;
        ULONG Build;
    } Version;
    NETFUNC * FunctionTable;
    ULONG FunctionTableCount;
    PVOID ClassBindInfo;
    PVOID ClientBindClass;
    PVOID ClientUnbindClass;
    PVOID ClassModule;
} NETCX_CLASS_BIND_INFO;

#ifdef _WIN64
C_ASSERT(sizeof(NETCX_CLASS_BIND_INFO) == 0x50);
C_ASSERT(FIELD_OFFSET(NETCX_CLASS_BIND_INFO, FunctionTable) == 0x20);
C_ASSERT(FIELD_OFFSET(NETCX_CLASS_BIND_INFO, ClassBindInfo) == 0x30);
#else
C_ASSERT(sizeof(NETCX_CLASS_BIND_INFO) == 0x2c);
#endif

NETFUNC NetFunctions[NetFunctionTableNumEntries];
PNET_DRIVER_GLOBALS NetDriverGlobals;

#if defined(_MSC_VER)
#pragma section(".kmdfclassbind$b", read, write)
#define NETCX_CLASS_BIND_RECORD __declspec(allocate(".kmdfclassbind$b"))
#else
/* "used" keeps the record although nothing references it by name */
#define NETCX_CLASS_BIND_RECORD __attribute__((section(".kmdfclassbind$b"), used, aligned(16)))
#endif

NETCX_CLASS_BIND_RECORD
NETCX_CLASS_BIND_INFO _NetAdapterCx_BIND_INFO =
{
    sizeof(NETCX_CLASS_BIND_INFO),
    L"NetAdapterCx",
    { NETADAPTER_VERSION_MAJOR, NETADAPTER_VERSION_MINOR, 0 },
    NetFunctions,
    NetFunctionTableNumEntries,
    &NetDriverGlobals,
    NULL,
    NULL,
    NULL
};

#if defined(_MSC_VER)
/* Pull the record in like the WDK stub does */
#ifdef _WIN64
#pragma comment(linker, "/include:_NetAdapterCx_BIND_INFO")
#else
#pragma comment(linker, "/include:__NetAdapterCx_BIND_INFO")
#endif
#endif

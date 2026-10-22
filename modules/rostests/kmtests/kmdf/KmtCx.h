/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Interface of the test KMDF class extension
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#pragma once

/*
 * A minimal class extension in the style of NetAdapterCx/UCX: clients bind
 * to it through a WDF_CLASS_BIND_INFO in .kmdfclassbind and call it through
 * the function table it fills in.
 */

#define KMTCX_CLASS_NAME     L"KmtCx"
#define KMTCX_MAJOR_VERSION  1
#define KMTCX_MINOR_VERSION  2
#define KMTCX_MAGIC          0x1337CAFE

typedef struct _KMTCX_DRIVER_GLOBALS
{
    ULONG Size;
    /* What the loader passed as the client's component globals */
    PVOID ClientGlobals;
    /* Size field of the client's class bind info */
    ULONG BindInfoSize;
    ULONG BindCount;
} KMTCX_DRIVER_GLOBALS, *PKMTCX_DRIVER_GLOBALS;

typedef enum _KMTCXFUNCENUM
{
    KmtCxGetMagicTableIndex = 0,
    KmtCxAddTableIndex,
    KmtCxGetLibraryStateTableIndex,
    KmtCxFunctionTableNumEntries
} KMTCXFUNCENUM;

typedef VOID (NTAPI *PFN_KMTCXFUNC)(VOID);

typedef
ULONG
(NTAPI *PFN_KMTCXGETMAGIC)(
    _In_ PKMTCX_DRIVER_GLOBALS Globals);

typedef
ULONG
(NTAPI *PFN_KMTCXADD)(
    _In_ PKMTCX_DRIVER_GLOBALS Globals,
    _In_ ULONG A,
    _In_ ULONG B);

/* Returns how often the library was initialized and clients were bound */
typedef
VOID
(NTAPI *PFN_KMTCXGETLIBRARYSTATE)(
    _In_ PKMTCX_DRIVER_GLOBALS Globals,
    _Out_ PULONG InitializeCount,
    _Out_ PULONG BindCount);

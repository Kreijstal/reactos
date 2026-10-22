/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx client handles, globals and common types
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#define _NETADAPTERCXTYPES_H_

#ifndef WDF_EXTERN_C_START
#error wdf.h must be included before netadaptercx.h
#endif

/*
 * The client names the NetAdapterCx version it is built for. Only the
 * versions these headers describe are accepted.
 */
#ifndef NETADAPTER_VERSION_MAJOR
#define NETADAPTER_VERSION_MAJOR 2
#endif
#ifndef NETADAPTER_VERSION_MINOR
#define NETADAPTER_VERSION_MINOR 1
#endif

#if (NETADAPTER_VERSION_MAJOR != 2) || (NETADAPTER_VERSION_MINOR > 1)
#error These headers describe NetAdapterCx 2.0 and 2.1 only
#endif

#include <ifdef.h>

WDF_EXTERN_C_START

typedef struct _NET_DRIVER_GLOBALS
{
    ULONG Unused;
} NET_DRIVER_GLOBALS, *PNET_DRIVER_GLOBALS;

typedef VOID (*NETFUNC)(VOID);

/* Defined by the client stub library, filled in by the class bind */
extern NETFUNC NetFunctions[];
extern PNET_DRIVER_GLOBALS NetDriverGlobals;

DECLARE_HANDLE(NETADAPTER);
DECLARE_HANDLE(NETPACKETQUEUE);
DECLARE_HANDLE(NETOFFLOAD);
DECLARE_HANDLE(NETCONFIGURATION);
DECLARE_HANDLE(NETPOWEROFFLOAD);
DECLARE_HANDLE(NETWAKESOURCE);
#if NETADAPTER_VERSION_MINOR >= 1
DECLARE_HANDLE(NETRECEIVEFILTER);
DECLARE_HANDLE(NETEXECUTIONCONTEXT);
#endif

/* Opaque init objects owned by the framework */
typedef struct _NETADAPTER_INIT NETADAPTER_INIT;
typedef struct _NETTXQUEUE_INIT NETTXQUEUE_INIT;
typedef struct _NETRXQUEUE_INIT NETRXQUEUE_INIT;

#if NETADAPTER_VERSION_MINOR >= 1
typedef union _NET_EUI48_ADDRESS
{
    UINT8 Value[6];
} NET_EUI48_ADDRESS;
#endif

typedef union _NET_IPV4_ADDRESS
{
    UINT32 Address;
    UINT8 Value[4];
} NET_IPV4_ADDRESS;

typedef union _NET_IPV6_ADDRESS
{
    struct
    {
        UINT64 NetworkPrefix;
        UINT64 InterfaceIdentifier;
    } Unicast;
    UINT8 Value[16];
} NET_IPV6_ADDRESS;

WDF_EXTERN_C_END

#endif /* _NETADAPTERCXTYPES_H_ */

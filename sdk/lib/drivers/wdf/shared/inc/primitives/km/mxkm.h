/*++

Copyright (c) Microsoft Corporation

ModuleName:

    MxKm.h

Abstract:

    This file includes ntddk.h and
    kernel mode versions of mode agnostic headers

    It also contains definitions pulled out from wdm.h

Author:



Revision History:



--*/

#pragma once

#include <ntddk.h>

#if (DLL_EXPORT_VERSION < 0xA00)
/* KMDF 1.33 uses the zeroing allocator even on pre-Windows 10 kernels. */
PVOID
NTAPI
FxAllocatePool2(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag);
#define ExAllocatePool2 FxAllocatePool2
#endif
// #include <procgrp.h>
// #include <wdmsec.h>

// #include <wmikm.h>
// #include <ntwmi.h>

typedef KDEFERRED_ROUTINE MdDeferredRoutineType, *MdDeferredRoutine;
typedef EXT_CALLBACK MdExtCallbackType, *MdExtCallback;
#define FX_DEVICEMAP_PATH L"\\REGISTRY\\MACHINE\\HARDWARE\\DEVICEMAP\\"

#include "mxgeneralkm.h"
#include "mxlockkm.h"
#include "mxpagedlockkm.h"
#include "mxeventkm.h"
#include "mxmemorykm.h"
#include "mxtimerkm.h"
#include "mxworkitemkm.h"
#include "mxdriverobjectkm.h"
#include "mxdeviceobjectkm.h"
#include "mxfileobjectkm.h"


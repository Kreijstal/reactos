/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Removal of a device whose child's PDO outlives it, declarations
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

/*
 * ioremovedchild_drv drives three levels of devices: a root-enumerated
 * bus, which reports one "mid" child as long as the Parameters value
 * "MidGone" is 0, and the mid device, a bus itself, which reports one
 * "leaf" child. It is the function driver of all three. When the bus stops
 * reporting the mid device (a USB device pulled out), the mid device and
 * its leaf child are surprise-removed and removed; their bus drivers then
 * delete their PDOs, as usbhub and usbstor do. The driver keeps a reference
 * on both PDOs, as a mounted volume keeps the PDO of a USB disk, until
 * "Release" is set and the bus is enumerated again. The leaf's PDO is named,
 * so that user mode can open the leaf's stack.
 */

#define IOREMOVEDCHILD_SERVICE_NAME     L"KmtIoRemovedChild"
#define IOREMOVEDCHILD_IMAGE_NAME       L"ioremovedchild_drv.sys"
#define IOREMOVEDCHILD_BUS_ID           L"KmtIoRemovedChild"

#define IOREMOVEDCHILD_ENUMERATOR       L"KmtIoRemovedChild"
#define IOREMOVEDCHILD_MID_DEVICE_ID    IOREMOVEDCHILD_ENUMERATOR L"\\Mid"
#define IOREMOVEDCHILD_LEAF_DEVICE_ID   IOREMOVEDCHILD_ENUMERATOR L"\\Leaf"
#define IOREMOVEDCHILD_LEAF_DOS_NAME  L"\\\\.\\KmtIoRemovedChildLeaf"
#define IOREMOVEDCHILD_INSTANCE_ID      L"0"
#define IOREMOVEDCHILD_MID_PATH         IOREMOVEDCHILD_MID_DEVICE_ID L"\\" IOREMOVEDCHILD_INSTANCE_ID
#define IOREMOVEDCHILD_LEAF_PATH        IOREMOVEDCHILD_LEAF_DEVICE_ID L"\\" IOREMOVEDCHILD_INSTANCE_ID

/* REG_DWORD values under Services\KmtIoRemovedChild\Parameters, set by user mode */
#define IOREMOVEDCHILD_VALUE_MID_GONE   L"MidGone"
#define IOREMOVEDCHILD_VALUE_RELEASE    L"Release"
/* ... and counters, set by the driver */
#define IOREMOVEDCHILD_VALUE_MID_REMOVES    L"MidPdoRemoves"
#define IOREMOVEDCHILD_VALUE_LEAF_REMOVES   L"LeafPdoRemoves"
#define IOREMOVEDCHILD_VALUE_LEAF_SURPRISES L"LeafPdoSurpriseRemovals"
#define IOREMOVEDCHILD_VALUE_RELEASED       L"Released"

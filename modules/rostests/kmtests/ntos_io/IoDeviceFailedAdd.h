/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Restart of a device whose AddDevice failed, declarations
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

/*
 * iofailedadd_drv is the bus driver of a root-enumerated device that
 * reports one child, and the function driver of that child. Like a PCI bus
 * driver, it keeps the child's PDO while the child is there, including
 * across IRP_MN_REMOVE_DEVICE. As the function driver of the child, its
 * AddDevice fails with STATUS_OBJECT_NAME_NOT_FOUND until the child's
 * hardware key (Device Parameters) has the "Installed" value: the way NDIS
 * fails the AddDevice of a network adapter whose installation did not write
 * its registry keys yet. The driver counts the child's AddDevice calls and
 * starts in its service key, under Parameters.
 */

#define IOFAILEDADD_SERVICE_NAME    L"KmtIoFailedAdd"
#define IOFAILEDADD_IMAGE_NAME      L"iofailedadd_drv.sys"
#define IOFAILEDADD_BUS_ID          L"KmtIoFailedAdd"

/* The child: device ID, instance ID (unique), so its instance path is fixed */
#define IOFAILEDADD_CHILD_ENUMERATOR    L"KmtIoFailedAdd"
#define IOFAILEDADD_CHILD_DEVICE_ID     IOFAILEDADD_CHILD_ENUMERATOR L"\\Child"
#define IOFAILEDADD_CHILD_INSTANCE_ID   L"0"
#define IOFAILEDADD_CHILD_INSTANCE_PATH IOFAILEDADD_CHILD_DEVICE_ID L"\\" IOFAILEDADD_CHILD_INSTANCE_ID

/* REG_DWORD in the child's Device Parameters key: its AddDevice succeeds */
#define IOFAILEDADD_VALUE_INSTALLED     L"Installed"

/* REG_DWORD values under Services\KmtIoFailedAdd\Parameters */
#define IOFAILEDADD_VALUE_FAILED_ADDS   L"ChildFailedAdds"
#define IOFAILEDADD_VALUE_DEVICE_ADDS   L"ChildAdds"
#define IOFAILEDADD_VALUE_STARTS        L"ChildStarts"

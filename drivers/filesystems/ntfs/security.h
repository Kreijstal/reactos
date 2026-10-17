/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Stored descriptor checks for opens and namespace changes
 */
#pragma once

NTSTATUS
NtfsCheckAccess(PDEVICE_EXTENSION Vcb,
                 ULONGLONG MftIndex,
                 PACCESS_STATE AccessState,
                 ACCESS_MASK DesiredAccess,
                 ACCESS_MASK PreviouslyGranted,
                 KPROCESSOR_MODE AccessMode,
                 PACCESS_MASK GrantedAccess);

NTSTATUS
NtfsCheckNamespaceAccess(PDEVICE_EXTENSION Vcb,
                         ULONGLONG MftIndex,
                         ULONGLONG Parent,
                         ACCESS_MASK DesiredAccess,
                         PIRP Irp);

/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     File access-check interface used by create and security paths
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

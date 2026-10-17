/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Sparse deallocation with an optional metadata persistence barrier
 */
#pragma once

typedef NTSTATUS (*PNTFS_SPARSE_COMMIT_BARRIER)(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsDeallocateSparseRangeEx(PDEVICE_EXTENSION Vcb,
                            PNTFS_ATTR_CONTEXT Context,
                            ULONG AttributeOffset,
                            PFILE_RECORD_HEADER Record,
                            ULONGLONG StartVcn,
                            ULONGLONG VcnCount,
                            PNTFS_SPARSE_COMMIT_BARRIER Barrier);

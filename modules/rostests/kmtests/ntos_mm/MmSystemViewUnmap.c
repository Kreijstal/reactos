/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Unmap system views of pagefile-backed sections with untouched pages
 */

#include <kmt_test.h>

#define VIEW_PAGES 16

/*
 * A system view of a committed pagefile-backed section starts with every PTE
 * pointing at its prototype PTE. Pages nobody touched keep that PTE, and
 * their prototype PTEs hold the demand-zero template, or are valid when
 * another view faulted the page in. Unmapping the view must accept that,
 * whether or not some of the other pages were touched.
 */
static
VOID
MapTouchUnmap(
    _In_ ULONG Allocation,
    _In_ ULONG TouchEvery)
{
    NTSTATUS Status;
    HANDLE SectionHandle;
    LARGE_INTEGER MaximumSize;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PVOID SectionObject;
    PUCHAR Base = NULL, Again = NULL;
    SIZE_T ViewSize = VIEW_PAGES * PAGE_SIZE;
    ULONG i;

    MaximumSize.QuadPart = VIEW_PAGES * PAGE_SIZE;
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&SectionHandle, SECTION_ALL_ACCESS, &ObjectAttributes,
                             &MaximumSize, PAGE_READWRITE, Allocation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = ObReferenceObjectByHandle(SectionHandle, SECTION_ALL_ACCESS, NULL,
                                       KernelMode, &SectionObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(SectionHandle);
    if (!NT_SUCCESS(Status))
        return;

    Status = MmMapViewInSystemSpace(SectionObject, (PVOID *)&Base, &ViewSize);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        for (i = 0; TouchEvery && i < VIEW_PAGES; i += TouchEvery)
            Base[i * PAGE_SIZE] = (UCHAR)(i + 1);

        /* A second view sees the same pages. Reading the untouched ones
         * through it makes their prototype PTEs valid under the prototype
         * pointer PTEs of the first view. */
        ViewSize = VIEW_PAGES * PAGE_SIZE;
        Status = MmMapViewInSystemSpace(SectionObject, (PVOID *)&Again, &ViewSize);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            for (i = 0; i < VIEW_PAGES; i += 2)
            {
                UCHAR Expected = (TouchEvery && !(i % TouchEvery)) ? (UCHAR)(i + 1) : 0;
                ok(Again[i * PAGE_SIZE] == Expected, "Page %lu reads 0x%x, expected 0x%x\n",
                   i, Again[i * PAGE_SIZE], Expected);
            }
        }

        Status = MmUnmapViewInSystemSpace(Base);
        ok_eq_hex(Status, STATUS_SUCCESS);

        if (Again)
        {
            Status = MmUnmapViewInSystemSpace(Again);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }

    ObDereferenceObject(SectionObject);
}

START_TEST(MmSystemViewUnmap)
{
    MapTouchUnmap(SEC_COMMIT, 0);
    MapTouchUnmap(SEC_COMMIT, 1);
    MapTouchUnmap(SEC_COMMIT, 4);
}

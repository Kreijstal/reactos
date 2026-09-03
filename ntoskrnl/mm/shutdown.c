/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/mm/shutdown.c
 * PURPOSE:         Memory Manager Shutdown
 * PROGRAMMERS:
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "ARM3/miarm.h"

/* PRIVATE FUNCTIONS *********************************************************/

VOID
MiShutdownSystem(VOID)
{
    ULONG i;
    PFN_NUMBER Page;
    BOOLEAN Written;

    /* Loop through all the paging files */
    for (i = 0; i < MmNumberOfPagingFiles; i++)
    {
        /* Free page file name */
        ASSERT(MmPagingFile[i]->PageFileName.Buffer != NULL);
        ExFreePoolWithTag(MmPagingFile[i]->PageFileName.Buffer, TAG_MM);
        MmPagingFile[i]->PageFileName.Buffer = NULL;

        /* The handles are NOT closed here. This runs while user processes are
         * still alive -- PopGracefulShutdown has just finished listing the
         * ones that are -- and everything after it, the cache manager flush
         * and both I/O manager passes included, can still fault. Closing the
         * paging file first makes every one of those faults unresolvable for
         * any page that happens to be paged out. Phase 1 closes them, which is
         * after the last of that work. */
    }

    /*
     * Loop through all the pages owned by the legacy Mm and page them out, if
     * needed.  Repeat while a pass actually wrote something, since writing
     * makes the file system dirty new (metadata) pages.  A page whose write
     * FAILS stays dirty in its segment (MmCheckDirtySegment re-marks it) and
     * is no reason for another pass: Windows' MiShutdownSystem flushes the
     * modified pages once and moves on, it never spins until the system is
     * clean.  Looping "while dirty" here hangs shutdown forever as soon as one
     * page can no longer be written -- fastfat fails every write to a volume
     * that has had its shutdown notification with STATUS_TOO_LATE, and
     * IoShutdownSystem(0) runs before this with user processes still alive.
     */
    do
    {
        Written = FALSE;

        Page = MmGetLRUFirstUserPage();
        while (Page)
        {
            LARGE_INTEGER SegmentOffset;
            PMM_SECTION_SEGMENT Segment = MmGetSectionAssociation(Page, &SegmentOffset);

            if (Segment)
            {
                if ((*Segment->Flags) & MM_DATAFILE_SEGMENT)
                {
                    MmLockSectionSegment(Segment);

                    ULONG_PTR Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);

                    if (!IS_SWAP_FROM_SSE(Entry) && IS_DIRTY_SSE(Entry))
                    {
                        IO_STATUS_BLOCK Iosb;

                        MmCheckDirtySegment(Segment, &SegmentOffset, FALSE, TRUE, &Iosb);
                        if (NT_SUCCESS(Iosb.Status))
                            Written = TRUE;
                    }

                    MmUnlockSectionSegment(Segment);
                }

                MmDereferenceSegment(Segment);
            }

            Page = MmGetLRUNextUserPage(Page, FALSE);
        }
    } while (Written);
}

VOID
MmShutdownSystem(IN ULONG Phase)
{
    if (Phase == 0)
    {
        MiShutdownSystem();
    }
    else if (Phase == 1)
    {
        ULONG i;

        /* Loop through all the paging files */
        for (i = 0; i < MmNumberOfPagingFiles; i++)
        {
            /* Close them -- see MiShutdownSystem for why not before now --
             * and then let go of the file object */
            ZwClose(MmPagingFile[i]->FileHandle);
            ObDereferenceObject(MmPagingFile[i]->FileObject);
        }
    }
    else
    {
        ASSERT(Phase == 2);

        UNIMPLEMENTED;
    }
}

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
    BOOLEAN Dirty;

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

    /* Loop through all the pages owned by the legacy Mm and page them out, if needed. */
    /* We do it as long as there are dirty pages, since flushing can cause the FS to dirtify new ones. */
    do
    {
        Dirty = FALSE;

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
                        Dirty = TRUE;
                        MmCheckDirtySegment(Segment, &SegmentOffset, FALSE, TRUE, NULL);
                    }

                    MmUnlockSectionSegment(Segment);
                }

                MmDereferenceSegment(Segment);
            }

            Page = MmGetLRUNextUserPage(Page, FALSE);
        }
    } while (Dirty);
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

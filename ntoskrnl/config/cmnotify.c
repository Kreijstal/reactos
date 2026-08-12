/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/config/cmnotify.c
 * PURPOSE:         Configuration Manager - Wrappers for Hive Operations
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"
#define NDEBUG
#include "debug.h"

/* FUNCTIONS *****************************************************************/

static
VOID
CmpCompletePost(IN PCM_POST_BLOCK PostBlock,
                IN NTSTATUS Status)
{
    RemoveEntryList(&PostBlock->PostList);

    if (PostBlock->IoStatusBlock)
    {
        _SEH2_TRY
        {
            PostBlock->IoStatusBlock->Status = Status;
            PostBlock->IoStatusBlock->Information = 0;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        _SEH2_END;
    }

    if (PostBlock->Event)
    {
        KeSetEvent(PostBlock->Event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(PostBlock->Event);
    }

    ExFreePoolWithTag(PostBlock, TAG_CM);
}

VOID
NTAPI
CmpReportNotify(IN PCM_KEY_CONTROL_BLOCK Kcb,
                IN PHHIVE Hive,
                IN HCELL_INDEX Cell,
                IN ULONG Filter)
{
    PLIST_ENTRY ListEntry, NextEntry;
    PCM_NOTIFY_BLOCK NotifyBlock;
    PCM_POST_BLOCK PostBlock;
    PCM_KEY_CONTROL_BLOCK CurrentKcb;
    PCMHIVE CmHive;

    UNREFERENCED_PARAMETER(Cell);

    CmHive = CONTAINING_RECORD(Hive, CMHIVE, Hive);
    ListEntry = CmHive->NotifyList.Flink;
    while (ListEntry != &CmHive->NotifyList)
    {
        NextEntry = ListEntry->Flink;
        NotifyBlock = CONTAINING_RECORD(ListEntry, CM_NOTIFY_BLOCK, HiveList);

        if (NotifyBlock->KeyControlBlock->NotifyOwner != NotifyBlock->KeyBody)
        {
            ListEntry = NextEntry;
            continue;
        }

        CurrentKcb = Kcb;
        if ((Filter & REG_NOTIFY_CHANGE_NAME) && CurrentKcb->ParentKcb)
            CurrentKcb = CurrentKcb->ParentKcb;

        while (CurrentKcb)
        {
            if (CurrentKcb == NotifyBlock->KeyControlBlock)
            {
                PLIST_ENTRY PostEntry, NextPostEntry;

                PostEntry = NotifyBlock->PostList.Flink;
                while (PostEntry != &NotifyBlock->PostList)
                {
                    NextPostEntry = PostEntry->Flink;
                    PostBlock = CONTAINING_RECORD(PostEntry,
                                                  CM_POST_BLOCK,
                                                  PostList);
                    if ((!PostBlock->Filter || (PostBlock->Filter & Filter)) &&
                        ((CurrentKcb == Kcb) ||
                         (Filter & REG_NOTIFY_CHANGE_NAME) ||
                         PostBlock->WatchTree))
                    {
                        CmpCompletePost(PostBlock, STATUS_SUCCESS);
                    }
                    PostEntry = NextPostEntry;
                }
                return;
            }

            CurrentKcb = CurrentKcb->ParentKcb;
        }

        ListEntry = NextEntry;
    }
}

VOID
NTAPI
CmpFlushNotify(IN PCM_KEY_BODY KeyBody,
               IN BOOLEAN LockHeld)
{
    PCM_NOTIFY_BLOCK NotifyBlock;

    if (!LockHeld)
        CmpLockRegistryExclusive();

    if (KeyBody->NotifyBlock)
    {
        NotifyBlock = KeyBody->NotifyBlock;
        while (!IsListEmpty(&NotifyBlock->PostList))
        {
            CmpCompletePost(CONTAINING_RECORD(NotifyBlock->PostList.Flink,
                                              CM_POST_BLOCK,
                                              PostList),
                            STATUS_NOTIFY_CLEANUP);
        }

        RemoveEntryList(&NotifyBlock->HiveList);
        if (NotifyBlock->KeyControlBlock->NotifyOwner == KeyBody)
            NotifyBlock->KeyControlBlock->NotifyOwner = NULL;
        KeyBody->NotifyBlock = NULL;
        ExFreePoolWithTag(NotifyBlock, TAG_CM);
    }

    if (!LockHeld)
        CmpUnlockRegistry();
}

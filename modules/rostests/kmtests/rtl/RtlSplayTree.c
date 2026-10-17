/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite RtlGenericTable
 * PROGRAMMER:      arty
 */

#define KMT_EMULATE_KERNEL
#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

static LIST_ENTRY Allocations;

static RTL_GENERIC_COMPARE_RESULTS NTAPI
CompareCharTable(PRTL_GENERIC_TABLE Table, PVOID A, PVOID B)
{
    RTL_GENERIC_COMPARE_RESULTS Result = (*((PCHAR)A) < *((PCHAR)B)) ? GenericLessThan :
        (*((PCHAR)A) > *((PCHAR)B)) ? GenericGreaterThan :
        GenericEqual;
    return Result;
}

static PVOID NTAPI
AllocRoutine(PRTL_GENERIC_TABLE Table, CLONG ByteSize)
{
    PLIST_ENTRY Entry = ExAllocatePool
        (NonPagedPool, sizeof(LIST_ENTRY) + ByteSize);
    InsertTailList(&Allocations, Entry);
    return &Entry[1];
}

static VOID NTAPI
FreeRoutine(PRTL_GENERIC_TABLE Table, PVOID Buffer)
{
    PLIST_ENTRY Entry = (PLIST_ENTRY)(((PCHAR)Buffer) - sizeof(LIST_ENTRY));
    RemoveEntryList(Entry);
    ExFreePool(Entry);
}

static void RtlSplayTreeTest()
{
    ULONG i, del;
    PCHAR Ch;
    CHAR Text[] = "the quick_brown!fOx-jUmp3d/0vER+THe^lazy.D@g";
    CHAR NewE[] = "11111111111111111111111111111111110111111111";
    RTL_GENERIC_TABLE Table;
    RtlInitializeGenericTable
        (&Table,
         CompareCharTable,
         AllocRoutine,
         FreeRoutine,
         NULL);
    for (i = 0; Text[i]; i++) {
        BOOLEAN WasNew;
        Ch = (PCHAR)RtlInsertElementGenericTable
            (&Table,
             &Text[i],
             sizeof(Text[i]),
             &WasNew);
        ok(Ch && *Ch == Text[i], "Copy character into node\n");
        ok(WasNew == (NewE[i] == '1'),
           "Character newness didn't match for char %u: '%c'\n",
           i, Text[i]);
    }
    for (Ch = (PCHAR)RtlEnumerateGenericTable(&Table, TRUE), i = 0;
         Ch;
         Ch = (PCHAR)RtlEnumerateGenericTable(&Table, FALSE), i++) {
        ok(strchr(Text, *Ch) != NULL, "Nonexistent character\n");
    }
    ok(RtlNumberGenericTableElements(&Table) == strlen(Text) - 1, "Not the right number of elements\n");
    ok(RtlLookupElementGenericTable(&Table, "q") != NULL, "Could not lookup q\n");
    ok(!RtlLookupElementGenericTable(&Table, "#"), "Found a character that shouldn't appear\n");
    ok(strlen(Text) == i + 1, "Didn't enumerate enough characters\n");
    del = 0;
    for (i = 0; Text[i]; i++) {
        if (NewE[i] == '1') {
            BOOLEAN WasDeleted;
            WasDeleted = RtlDeleteElementGenericTable(&Table, &Text[i]);
            del += WasDeleted;
        }
    }
    ok(!RtlNumberGenericTableElements(&Table), "Not zero elements\n");
    ok(!RtlGetElementGenericTable(&Table, 0), "Elements left when we removed them all\n");
    ok(strlen(Text) == del + 1, "Deleted too many times\n");
    ok(IsListEmpty(&Allocations), "Didn't free all memory\n");
}

#ifndef RTL_USE_AVL_TABLES
/* Indexed enumeration is in insertion order, independently of the previous
 * index queried, tree rotations, deletion, or a repeated insertion. */
static VOID
TestIndexedElements(VOID)
{
    RTL_GENERIC_TABLE Table;
    CHAR Values[] = "hdbfaceg";
    CHAR Remaining[] = "hdbaceg";
    PCHAR Element;
    ULONG i, j;
    BOOLEAN Inserted, Deleted;

    RtlInitializeGenericTable(&Table, CompareCharTable, AllocRoutine, FreeRoutine, NULL);
    for (i = 0; i < sizeof(Values) - 1; ++i)
    {
        Element = RtlInsertElementGenericTable(&Table, &Values[i], sizeof(CHAR), &Inserted);
        ok(Element != NULL && Inserted, "failed to insert element %lu\n", i);
    }
    for (i = 0; i < sizeof(Values) - 1; ++i)
    {
        Element = RtlGetElementGenericTable(&Table, i);
        ok(Element != NULL && *Element == Values[i], "wrong forward element %lu\n", i);
    }
    for (i = 0; i < sizeof(Values) - 1; ++i)
    {
        for (j = 0; j < sizeof(Values) - 1; ++j)
        {
            Element = RtlGetElementGenericTable(&Table, i);
            ok(Element != NULL && *Element == Values[i], "wrong index %lu before %lu\n", i, j);
            Element = RtlGetElementGenericTable(&Table, j);
            ok(Element != NULL && *Element == Values[j], "wrong index %lu after %lu\n", j, i);
        }
    }
    Element = RtlGetElementGenericTable(&Table, sizeof(Values) - 1);
    ok(Element == NULL, "out-of-range index returned %p\n", Element);
    Element = RtlGetElementGenericTable(&Table, MAXULONG);
    ok(Element == NULL, "MAXULONG index returned %p\n", Element);

    Deleted = RtlDeleteElementGenericTable(&Table, &Values[3]);
    ok(Deleted, "failed to delete middle acquisition\n");
    for (i = 0; i < sizeof(Remaining) - 1; ++i)
    {
        Element = RtlGetElementGenericTable(&Table, i);
        ok(Element != NULL && *Element == Remaining[i], "wrong index %lu after deletion\n", i);
    }
    Element = RtlInsertElementGenericTable(&Table, &Values[3], sizeof(CHAR), &Inserted);
    ok(Element != NULL && Inserted, "failed to reinsert removed element\n");
    Element = RtlGetElementGenericTable(&Table, sizeof(Values) - 2);
    ok(Element != NULL && *Element == Values[3], "reinserted element was not appended\n");
    for (i = 0; i < sizeof(Values) - 1; ++i)
    {
        Deleted = RtlDeleteElementGenericTable(&Table, &Values[i]);
        ok(Deleted, "failed to delete element %lu\n", i);
    }
    ok(IsListEmpty(&Allocations), "indexed enumeration leaked allocations\n");
}
#endif

START_TEST(RtlSplayTree)
{
    InitializeListHead(&Allocations);
    RtlSplayTreeTest();
#ifndef RTL_USE_AVL_TABLES
    TestIndexedElements();
#endif
}

/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for registry change notifications
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <kmt_test.h>

#define TEST_KEY L"\\Registry\\Machine\\SOFTWARE\\KmtestCmNotify"

static
NTSTATUS
CreateVolatileKey(
    _Out_ PHANDLE Key,
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name)
{
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, NULL);
    return ZwCreateKey(Key, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL,
                       REG_OPTION_VOLATILE, NULL);
}

static
NTSTATUS
CreateEventHandle(
    _Out_ PHANDLE Event)
{
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwCreateEvent(Event, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
}

/* A change below two watched keys completes both watches */
static
VOID
TestNestedWatchers(VOID)
{
    HANDLE Parent = NULL, Child = NULL, ParentEvent = NULL, ChildEvent = NULL;
    IO_STATUS_BLOCK ParentIosb, ChildIosb;
    UNICODE_STRING ValueName;
    LARGE_INTEGER Timeout;
    ULONG Value = 1;
    NTSTATUS Status;

    Status = CreateVolatileKey(&Parent, NULL, TEST_KEY);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = CreateVolatileKey(&Child, Parent, L"Child");
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = CreateEventHandle(&ParentEvent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateEventHandle(&ChildEvent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!ParentEvent || !ChildEvent)
        goto Cleanup;

    /* The parent watches its tree, the child itself; the parent arms first */
    Status = ZwNotifyChangeKey(Parent, ParentEvent, NULL, NULL, &ParentIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, TRUE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);
    Status = ZwNotifyChangeKey(Child, ChildEvent, NULL, NULL, &ChildIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);

    RtlInitUnicodeString(&ValueName, L"Value");
    Status = ZwSetValueKey(Child, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_SUCCESS);

    Timeout.QuadPart = -10000LL * 1000;
    Status = ZwWaitForSingleObject(ParentEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Parent watch not completed (0x%08lx)\n", Status);
    Status = ZwWaitForSingleObject(ChildEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Child watch not completed (0x%08lx)\n", Status);

Cleanup:
    /* Closing the keys cancels a watch that is still pending */
    if (Child)
    {
        ZwDeleteKey(Child);
        ZwClose(Child);
    }
    if (Parent)
    {
        ZwDeleteKey(Parent);
        ZwClose(Parent);
    }
    if (ChildEvent)
        ZwClose(ChildEvent);
    if (ParentEvent)
        ZwClose(ParentEvent);
}

/*
 * Two handles to one key, each watching it: a change completes both, and the
 * watch of the handle that stays open keeps working after the other one is
 * closed.
 */
static
VOID
TestSameKeyWatchers(VOID)
{
    HANDLE First = NULL, Second = NULL, FirstEvent = NULL, SecondEvent = NULL;
    IO_STATUS_BLOCK FirstIosb, SecondIosb;
    UNICODE_STRING ValueName;
    LARGE_INTEGER Timeout;
    ULONG Value = 1;
    NTSTATUS Status;

    Status = CreateVolatileKey(&First, NULL, TEST_KEY);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = CreateVolatileKey(&Second, NULL, TEST_KEY);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = CreateEventHandle(&FirstEvent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateEventHandle(&SecondEvent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!FirstEvent || !SecondEvent)
        goto Cleanup;
    RtlInitUnicodeString(&ValueName, L"Value");
    Timeout.QuadPart = -10000LL * 1000;

    /* Both watch the same key */
    Status = ZwNotifyChangeKey(First, FirstEvent, NULL, NULL, &FirstIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);
    Status = ZwNotifyChangeKey(Second, SecondEvent, NULL, NULL, &SecondIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);

    Status = ZwSetValueKey(First, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwWaitForSingleObject(FirstEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "First watch not completed (0x%08lx)\n", Status);
    Status = ZwWaitForSingleObject(SecondEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Second watch not completed (0x%08lx)\n", Status);

    /* Both watch again; the first handle goes away, the second one's watch stays */
    ZwClearEvent(FirstEvent);
    ZwClearEvent(SecondEvent);
    Status = ZwNotifyChangeKey(First, FirstEvent, NULL, NULL, &FirstIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);
    Status = ZwNotifyChangeKey(Second, SecondEvent, NULL, NULL, &SecondIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);

    /* Closing the handle completes its own watch, not the other one */
    ZwClose(First);
    First = NULL;
    Status = ZwWaitForSingleObject(FirstEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Watch of the closed handle not completed (0x%08lx)\n", Status);
    ok_eq_hex(FirstIosb.Status, STATUS_NOTIFY_CLEANUP);
    Timeout.QuadPart = 0;
    Status = ZwWaitForSingleObject(SecondEvent, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    Value = 2;
    Status = ZwSetValueKey(Second, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_SUCCESS);
    Timeout.QuadPart = -10000LL * 1000;
    Status = ZwWaitForSingleObject(SecondEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Watch not completed after another handle was closed (0x%08lx)\n", Status);
    ok_eq_hex(SecondIosb.Status, STATUS_SUCCESS);

Cleanup:
    if (Second)
    {
        ZwDeleteKey(Second);
        ZwClose(Second);
    }
    if (First)
        ZwClose(First);
    if (SecondEvent)
        ZwClose(SecondEvent);
    if (FirstEvent)
        ZwClose(FirstEvent);
}

START_TEST(CmNotify)
{
    TestNestedWatchers();
    TestSameKeyWatchers();
}

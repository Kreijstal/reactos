/*
 * PROJECT:     ReactOS browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IShellTaskScheduler implementation
 * COPYRIGHT:   Copyright 2020 Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 */

#include "precomp.h"

/*
 * The shell's background task scheduler (CLSID_ShellTaskScheduler): the
 * tasks added to one scheduler run one at a time, highest priority first
 * (first come, first served among equals), on a worker thread of its own
 * that goes away after it has been idle for a while.  Releasing the
 * scheduler lets the queued tasks run to completion, or with
 * ITSSFLAG_KILL_ON_DESTROY drops them and kills the one running.
 */

struct SHELL_TASK
{
    SHELL_TASK *Next;
    IRunnableTask *Task;
    GUID Toid;
    DWORD_PTR lParam;
    DWORD Priority;
};

struct SHELL_TASK_QUEUE
{
    LONG Refs;                  /* the scheduler's and the worker's */
    CRITICAL_SECTION Lock;
    HANDLE Wake;                /* auto-reset: a task was queued, or closing */
    HANDLE Thread;              /* the worker, NULL when there is none */
    DWORD ThreadId;
    SHELL_TASK *Head;           /* by priority, then in the order added */
    SHELL_TASK Running;         /* .Task is NULL while none runs */
    BOOL Closing;
    DWORD ReleaseStatus;
    DWORD ThreadTimeout;
};

static void
QueueRelease(SHELL_TASK_QUEUE *Queue)
{
    if (InterlockedDecrement(&Queue->Refs) != 0)
        return;
    ATLASSERT(Queue->Head == NULL && Queue->Running.Task == NULL);
    if (Queue->Thread)
        CloseHandle(Queue->Thread);
    CloseHandle(Queue->Wake);
    DeleteCriticalSection(&Queue->Lock);
    delete Queue;
}

static BOOL
TaskMatches(const SHELL_TASK *Task, REFGUID rtoid, DWORD_PTR lParam)
{
    return (IsEqualGUID(rtoid, TOID_NULL) || IsEqualGUID(rtoid, Task->Toid)) &&
           (lParam == ITSAT_DEFAULT_LPARAM || lParam == Task->lParam);
}

/* Waits for Handle, letting messages sent to this thread through: a task may
 * be sending one to a window of the thread that waits for it. */
static void
WaitDispatchingSent(HANDLE Handle)
{
    MSG msg;

    while (MsgWaitForMultipleObjects(1, &Handle, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1)
        PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

static DWORD WINAPI
TaskWorker(LPVOID Parameter)
{
    SHELL_TASK_QUEUE *Queue = (SHELL_TASK_QUEUE *)Parameter;
    SHELL_TASK *Task;
    IRunnableTask *Done;
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    for (;;)
    {
        EnterCriticalSection(&Queue->Lock);
        Task = Queue->Head;
        if (Task == NULL)
        {
            if (Queue->Closing)
            {
                LeaveCriticalSection(&Queue->Lock);
                break;
            }
            LeaveCriticalSection(&Queue->Lock);
            if (WaitForSingleObject(Queue->Wake, Queue->ThreadTimeout) == WAIT_TIMEOUT)
            {
                /* Idle: the next task added starts another worker */
                EnterCriticalSection(&Queue->Lock);
                if (Queue->Head == NULL)
                {
                    CloseHandle(Queue->Thread);
                    Queue->Thread = NULL;
                    Queue->ThreadId = 0;
                    LeaveCriticalSection(&Queue->Lock);
                    break;
                }
                LeaveCriticalSection(&Queue->Lock);
            }
            continue;
        }
        Queue->Head = Task->Next;
        Queue->Running = *Task;
        LeaveCriticalSection(&Queue->Lock);
        delete Task;

        /* Only this thread changes Running.Task */
        Queue->Running.Task->Run();

        EnterCriticalSection(&Queue->Lock);
        Done = Queue->Running.Task;
        Queue->Running.Task = NULL;
        LeaveCriticalSection(&Queue->Lock);
        Done->Release();
    }

    if (SUCCEEDED(hrCo))
        CoUninitialize();
    QueueRelease(Queue);
    return 0;
}

CShellTaskScheduler::CShellTaskScheduler() :
    m_Queue(NULL)
{
}

HRESULT CShellTaskScheduler::FinalConstruct()
{
    m_Queue = new SHELL_TASK_QUEUE;
    if (!m_Queue)
        return E_OUTOFMEMORY;
    ZeroMemory(m_Queue, sizeof(*m_Queue));
    m_Queue->Refs = 1;
    m_Queue->ReleaseStatus = ITSSFLAG_COMPLETE_ON_DESTROY;
    m_Queue->ThreadTimeout = ITSS_THREAD_DESTROY_DEFAULT_TIMEOUT;
    m_Queue->Wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!m_Queue->Wake)
    {
        delete m_Queue;
        m_Queue = NULL;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    InitializeCriticalSection(&m_Queue->Lock);
    return S_OK;
}

CShellTaskScheduler::~CShellTaskScheduler()
{
    SHELL_TASK_QUEUE *Queue = m_Queue;
    SHELL_TASK *Dropped = NULL, *Next;
    IRunnableTask *Kill = NULL;
    HANDLE Thread = NULL;

    if (!Queue)
        return;

    EnterCriticalSection(&Queue->Lock);
    Queue->Closing = TRUE;
    if (Queue->ReleaseStatus & ITSSFLAG_KILL_ON_DESTROY)
    {
        Dropped = Queue->Head;
        Queue->Head = NULL;
        Kill = Queue->Running.Task;
        if (Kill)
            Kill->AddRef();
    }
    if (Queue->Thread && Queue->ThreadId != GetCurrentThreadId() &&
        !DuplicateHandle(GetCurrentProcess(), Queue->Thread, GetCurrentProcess(), &Thread,
                         SYNCHRONIZE, FALSE, 0))
    {
        Thread = NULL;
    }
    SetEvent(Queue->Wake);
    LeaveCriticalSection(&Queue->Lock);

    for (; Dropped; Dropped = Next)
    {
        Next = Dropped->Next;
        Dropped->Task->Release();
        delete Dropped;
    }
    if (Kill)
    {
        Kill->Kill(FALSE);
        Kill->Release();
    }
    /* The worker finishes what is left, then ends. Released from one of
     * its own tasks, the scheduler does not wait for itself. */
    if (Thread)
    {
        WaitDispatchingSent(Thread);
        CloseHandle(Thread);
    }
    QueueRelease(Queue);
}

// *** IShellTaskScheduler methods ***
STDMETHODIMP CShellTaskScheduler::AddTask(IRunnableTask *pTask, REFGUID rtoid, DWORD_PTR lParam, DWORD dwPriority)
{
    SHELL_TASK *Task, **Link;
    HRESULT hr = S_OK;

    TRACE("(%p, %p, %s, %Ix, %lx)\n", this, pTask, wine_dbgstr_guid(&rtoid), lParam, dwPriority);

    if (!pTask)
        return E_INVALIDARG;
    Task = new SHELL_TASK;
    if (!Task)
        return E_OUTOFMEMORY;
    Task->Task = pTask;
    Task->Toid = rtoid;
    Task->lParam = lParam;
    Task->Priority = dwPriority;
    pTask->AddRef();

    EnterCriticalSection(&m_Queue->Lock);
    for (Link = &m_Queue->Head; *Link && (*Link)->Priority >= dwPriority; Link = &(*Link)->Next);
    Task->Next = *Link;
    *Link = Task;
    if (!m_Queue->Thread)
    {
        InterlockedIncrement(&m_Queue->Refs);
        m_Queue->Thread = CreateThread(NULL, 0, TaskWorker, m_Queue, 0, &m_Queue->ThreadId);
        if (!m_Queue->Thread)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            InterlockedDecrement(&m_Queue->Refs);
            m_Queue->ThreadId = 0;
            *Link = Task->Next;
        }
    }
    if (SUCCEEDED(hr))
        SetEvent(m_Queue->Wake);
    LeaveCriticalSection(&m_Queue->Lock);

    if (FAILED(hr))
    {
        pTask->Release();
        delete Task;
    }
    return hr;
}

STDMETHODIMP CShellTaskScheduler::RemoveTasks(REFGUID rtoid, DWORD_PTR lParam, BOOL fWaitIfRunning)
{
    SHELL_TASK *Removed = NULL, *Next, **Link;
    IRunnableTask *Running = NULL;
    BOOL OwnThread;
    MSG msg;

    TRACE("(%p, %s, %Ix, %d)\n", this, wine_dbgstr_guid(&rtoid), lParam, fWaitIfRunning);

    EnterCriticalSection(&m_Queue->Lock);
    for (Link = &m_Queue->Head; *Link; )
    {
        if (TaskMatches(*Link, rtoid, lParam))
        {
            Next = (*Link)->Next;
            (*Link)->Next = Removed;
            Removed = *Link;
            *Link = Next;
        }
        else
        {
            Link = &(*Link)->Next;
        }
    }
    if (m_Queue->Running.Task && TaskMatches(&m_Queue->Running, rtoid, lParam))
    {
        Running = m_Queue->Running.Task;
        Running->AddRef();
    }
    OwnThread = m_Queue->ThreadId == GetCurrentThreadId();
    LeaveCriticalSection(&m_Queue->Lock);

    for (; Removed; Removed = Next)
    {
        Next = Removed->Next;
        Removed->Task->Release();
        delete Removed;
    }
    if (Running)
    {
        Running->Kill(fWaitIfRunning);
        /* Until the task returns from Run; a task cannot wait for itself */
        while (fWaitIfRunning && !OwnThread)
        {
            EnterCriticalSection(&m_Queue->Lock);
            BOOL Still = m_Queue->Running.Task == Running;
            LeaveCriticalSection(&m_Queue->Lock);
            if (!Still)
                break;
            if (MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_SENDMESSAGE) == WAIT_OBJECT_0)
                PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        Running->Release();
    }
    return S_OK;
}

UINT STDMETHODCALLTYPE CShellTaskScheduler::CountTasks(REFGUID rtoid)
{
    SHELL_TASK *Task;
    UINT Count = 0;

    EnterCriticalSection(&m_Queue->Lock);
    for (Task = m_Queue->Head; Task; Task = Task->Next)
    {
        if (TaskMatches(Task, rtoid, ITSAT_DEFAULT_LPARAM))
            Count++;
    }
    if (m_Queue->Running.Task && TaskMatches(&m_Queue->Running, rtoid, ITSAT_DEFAULT_LPARAM))
        Count++;
    LeaveCriticalSection(&m_Queue->Lock);
    return Count;
}

STDMETHODIMP CShellTaskScheduler::Status(DWORD dwReleaseStatus, DWORD dwThreadTimeout)
{
    TRACE("(%p, %lx, %lu)\n", this, dwReleaseStatus, dwThreadTimeout);

    EnterCriticalSection(&m_Queue->Lock);
    m_Queue->ReleaseStatus = dwReleaseStatus & ITSSFLAG_FLAGS_MASK;
    if (dwThreadTimeout != ITSS_THREAD_TIMEOUT_NO_CHANGE)
    {
        m_Queue->ThreadTimeout = dwThreadTimeout;
        /* An idle worker waits with the new timeout */
        SetEvent(m_Queue->Wake);
    }
    LeaveCriticalSection(&m_Queue->Lock);
    return S_OK;
}

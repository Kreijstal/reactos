/*
 * PROJECT:     ReactOS browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IShellTaskScheduler implementation
 * COPYRIGHT:   Copyright 2020 Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 */

#pragma once

/* What the scheduler and its worker thread share: the thread outlives the
 * scheduler when the scheduler is released from a task it runs. */
struct SHELL_TASK_QUEUE;

class CShellTaskScheduler :
    public CComCoClass<CShellTaskScheduler, &CLSID_ShellTaskScheduler>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IShellTaskScheduler
{
private:
    SHELL_TASK_QUEUE *m_Queue;
public:
    CShellTaskScheduler();
    ~CShellTaskScheduler();

    HRESULT FinalConstruct();

    // *** IShellTaskScheduler methods ***
    STDMETHODIMP AddTask(IRunnableTask *pTask, REFGUID rtoid, DWORD_PTR lParam, DWORD dwPriority) override;
    STDMETHODIMP RemoveTasks(REFGUID rtoid, DWORD_PTR lParam, BOOL fWaitIfRunning) override;
    STDMETHODIMP_(UINT) CountTasks(REFGUID rtoid) override;
    STDMETHODIMP Status(DWORD dwReleaseStatus, DWORD dwThreadTimeout) override;

public:

    DECLARE_REGISTRY_RESOURCEID(IDR_SHELLTASKSCHEDULER)
    DECLARE_NOT_AGGREGATABLE(CShellTaskScheduler)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CShellTaskScheduler)
        COM_INTERFACE_ENTRY_IID(IID_IShellTaskScheduler, IShellTaskScheduler)
    END_COM_MAP()
};

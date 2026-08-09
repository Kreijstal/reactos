@echo off
REM Force $INDEX_ALLOCATION -> $ATTRIBUTE_LIST migration, then keep extending
REM the directory afterwards - the post-migration extension is the buggy path.
REM
REM Migration is driven by mapping-pair size vs free room in the 1024-byte MFT
REM record, NOT by directory size (a 21k-file dir reached 1865 index clusters
REM on only 4 runs and never migrated).  Hence: long directory names shrink the
REM record slack, and round-robin across 8 dirs interleaves their index
REM clusters so nearly every cluster becomes its own run.
REM
REM v2 bug: "for /L %%i in (1,1,%ROUNDS%)" expands %ROUNDS% at PARSE time, and
REM it was still empty, so the loop ran zero times - dirs appeared, no files.
REM The count is passed to a subroutine instead.
setlocal enabledelayedexpansion

set ROOT=C:\frag
set DPAD=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
set FPAD=fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff

if exist %ROOT% rmdir /s /q %ROOT%
mkdir %ROOT%
echo MIGRATE_START > C:\frag_progress.txt
for /L %%d in (1,1,8) do mkdir "%ROOT%\d%%d_%DPAD%"

if "%1"=="smoke" (call :run 20) else (call :run 1500)

echo CREATE_DONE >> C:\frag_progress.txt
for /L %%d in (1,1,8) do (
    dir /b "%ROOT%\d%%d_%DPAD%" 2>nul | find /c /v "" >> C:\frag_progress.txt
)
echo MIGRATE_END >> C:\frag_progress.txt
goto :eof

:run
for /L %%i in (1,1,%1) do (
    for /L %%d in (1,1,8) do (
        type nul > "%ROOT%\d%%d_%DPAD%\f%%i_%FPAD%.t"
    )
    set /a mod=%%i %% 100
    if !mod!==0 echo PROGRESS round=%%i >> C:\frag_progress.txt
)
goto :eof

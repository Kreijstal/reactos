@echo off
rem Unattended MSYS2 installer launcher for the bootcdntfs test harness.
rem Runs once (via [GuiRunOnce]) after the 3rd boot of an unattended NTFS
rem install.  Pulls the QtIFW installer from the host SMB share
rem \\10.0.2.2\public (served over the SLIRP NIC) and drives it
rem non-interactively.  Falls back to a locally attached volume if the share
rem is unreachable, so the harness still completes when SMB is unavailable.
rem
rem Progress is reported through dbgprint (kernel debug port -> COM1), the
rem only reliable headless channel on ReactOS -- a plain "echo > COM1" opens
rem the serial.sys COM1 device, which collides with the kernel debugger that
rem owns the port, so it never reaches the host serial socket.  A copy is also
rem written to C:\instmsys.log for post-mortem inspection.

set MSYS2_EXE=
set MSYS2_ROOT=C:\msys64
set MSYS2_LOG=C:\instmsys.log
set MSYS2_SHARE=\\10.0.2.2\public
set MSYS2_LOCAL=C:\msys2-x86_64-latest.exe

rem QtIFW 4.x non-interactive install verb + answers. Tweak here if the
rem bundled installer needs different flags.  NOTE: --accept-messages and
rem --default-answer are mutually exclusive in QtIFW; passing both makes the
rem installer print usage and exit 1, so only --accept-messages is used.
set MSYS2_ARGS=in --root %MSYS2_ROOT% --accept-licenses --accept-messages --confirm-command

dbgprint "[instmsys] starting"
echo [instmsys] starting > "%MSYS2_LOG%"

rem === DNS-fix verification (registry key inheritance -> NetworkService can read
rem ServiceCurrent -> Dnscache starts -> name resolution works).  Give the SCM a
rem moment to bring NetworkService svchost up, then probe Dnscache + resolve a
rem hostname.  ping prints "could not find host" only on a DNS failure, so its
rem output (not exit code, since SLIRP may drop ICMP) is the resolution signal. ===
ping -n 6 127.0.0.1 >nul 2>&1
sc query Dnscache > C:\dnschk.txt 2>&1
type C:\dnschk.txt | find "RUNNING" >nul && (dbgprint "[dnschk] Dnscache RUNNING") || (dbgprint "[dnschk] Dnscache NOT-RUNNING")
type C:\dnschk.txt >> "%MSYS2_LOG%"
ping -n 1 repo.msys2.org > C:\dnsping.txt 2>&1
type C:\dnsping.txt | find "could not find" >nul && (dbgprint "[dnschk] DNS resolve FAIL") || (dbgprint "[dnschk] DNS resolve OK")
type C:\dnsping.txt >> "%MSYS2_LOG%"

rem The SMB redirector (smb2d, Start=2) races winlogon and the share is often
rem not reachable for the first few seconds of the interactive session.  Retry
rem the copy from \\10.0.2.2\public for up to ~60s before giving up on SMB.
set MSYS2_TRY=0
:smbretry
set /a MSYS2_TRY+=1
dbgprint "[instmsys] SMB copy attempt %MSYS2_TRY% from %MSYS2_SHARE%"
echo [instmsys] SMB copy attempt %MSYS2_TRY% from %MSYS2_SHARE% >> "%MSYS2_LOG%"
copy /Y "%MSYS2_SHARE%\msys2-x86_64-latest.exe" "%MSYS2_LOCAL%" >> "%MSYS2_LOG%" 2>&1
if exist "%MSYS2_LOCAL%" (
    set MSYS2_EXE=%MSYS2_LOCAL%
    dbgprint "[instmsys] fetched installer over SMB"
    echo [instmsys] fetched installer over SMB >> "%MSYS2_LOG%"
    goto :gotexe
)
if %MSYS2_TRY% GEQ 12 goto :trylocal
rem sleep ~5s without where/timeout (ping the SLIRP gateway 5 times)
ping -n 6 10.0.2.2 >nul 2>&1
goto :smbretry

:trylocal
dbgprint "[instmsys] SMB unreachable, scanning local volumes"
echo [instmsys] SMB unreachable, scanning local volumes >> "%MSYS2_LOG%"
for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%D:\msys2-x86_64-latest.exe" set MSYS2_EXE=%%D:\msys2-x86_64-latest.exe
)
if "%MSYS2_EXE%"=="" (
    for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
        for %%F in ("%%D:\msys2-*.exe") do if exist "%%F" set MSYS2_EXE=%%F
    )
)

:gotexe
if "%MSYS2_EXE%"=="" (
    dbgprint "[instmsys] ERROR: no msys2 installer found (SMB or local)"
    echo [instmsys] ERROR: no msys2 installer found (SMB or local) >> "%MSYS2_LOG%"
    goto :eof
)

dbgprint "[instmsys] launching %MSYS2_EXE% %MSYS2_ARGS%"
echo [instmsys] launching %MSYS2_EXE% %MSYS2_ARGS% >> "%MSYS2_LOG%"
"%MSYS2_EXE%" %MSYS2_ARGS%
dbgprint "[instmsys] installer exit code %ERRORLEVEL%"
echo [instmsys] installer exit code %ERRORLEVEL% >> "%MSYS2_LOG%"

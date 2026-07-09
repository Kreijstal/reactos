@echo off
set WINETEST_DEBUG=0
set WINETEST_PLATFORM=reactos

:: A host control disk, if attached, names the test(s) to run.  This is an
:: explicit one-shot request, so do not defer it through the Run key bootstrap.
set ROSTEST_SEL=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do if exist %%d:\ROSTEST.CMD set ROSTEST_SEL=%%d:\ROSTEST.CMD

:: On the first boot, we're started through RunOnce.
:: Add us to the Run key, so we're also started on the next reboot in case ReactOS crashed *and* the registry has been saved.
:: Exit right after that, because Explorer processes the Run key right after RunOnce and therefore picks up regtest.cmd a second time during the first boot.
if not defined ROSTEST_SEL reg query HKLM\Software\Microsoft\Windows\CurrentVersion\Run /v regtest
if not defined ROSTEST_SEL if "%errorlevel%"=="1" (
    reg add HKLM\Software\Microsoft\Windows\CurrentVersion\Run /v regtest /t REG_SZ /d "%SystemRoot%\system32\cmd.exe /c regtest.cmd"
    exit 0
)

move "%WINDIR%\bin\redirtest1.dll" "%WINDIR%\bin\kernel32test_versioned.dll"
move "%WINDIR%\bin\testdata\redirtest2.dll" "%WINDIR%\bin\testdata\kernel32test_versioned.dll"
if exist "%WINDIR%\bin\AHKAppTests.cmd" (
    dbgprint "Preparing AHK Application testing suite."
    call "%WINDIR%\bin\AHKAppTests.cmd"
    del "%WINDIR%\bin\AHKAppTests.cmd"
)

dbgprint --process "ipconfig"
cd "%WINDIR%\bin"
if defined ROSTEST_SEL (
    call "%ROSTEST_SEL%"
) else (
    start rosautotest /r /s /n
)

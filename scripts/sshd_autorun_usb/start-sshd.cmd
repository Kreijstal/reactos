@echo off
setlocal

set LOG=%~d0\sshd-autorun.log
echo ==== sshd autorun %DATE% %TIME% ====>>"%LOG%"

set MSYSROOT=
for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined MSYSROOT if exist %%D:\msys64\usr\bin\bash.exe set MSYSROOT=%%D:\msys64
)

if not defined MSYSROOT (
    echo msys64 not found on C: through Z:>>"%LOG%"
    exit /b 1
)

echo using %MSYSROOT%>>"%LOG%"

if not exist "%MSYSROOT%\etc\ssh\ssh_host_ed25519_key" (
    echo generating ssh host keys>>"%LOG%"
    "%MSYSROOT%\usr\bin\bash.exe" -lc "/usr/bin/ssh-keygen -A" >>"%LOG%" 2>&1
)

echo starting sshd>>"%LOG%"
start "ReactOS MSYS sshd" /min "%MSYSROOT%\usr\bin\bash.exe" -lc "exec /usr/bin/sshd -D -e -f /etc/ssh/sshd_config >>/var/log/sshd-autorun.log 2>&1"

exit /b 0

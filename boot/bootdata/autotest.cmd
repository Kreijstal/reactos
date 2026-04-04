@echo off
REM Scan drive letters for hello_serial.exe or ntfs_test.exe and run it
for %%d in (A B D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist %%d:\hello_serial.exe (
        %%d:\hello_serial.exe
        goto :done
    )
    if exist %%d:\ntfs_test.exe (
        %%d:\ntfs_test.exe
        goto :done
    )
)
:done

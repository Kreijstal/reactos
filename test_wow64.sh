#!/bin/bash
# WoW64 hello-world test script
# Usage: ./test_wow64.sh [install|inject|test]
#
# Steps:
#   1. ./test_wow64.sh install  - Install ReactOS from bootcd to disk image
#   2. ./test_wow64.sh inject   - Mount disk, copy WoW64 DLLs + hello32.exe
#   3. ./test_wow64.sh test     - Boot ReactOS and try running hello32.exe

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
WOW64_DIR="$SCRIPT_DIR/build-wow64"
DISK_IMG="/tmp/wow64test.img"
SERIAL_LOG="/tmp/wow64-serial.log"
MONITOR_SOCK="/tmp/wow64.sock"

send() { echo "sendkey $1" | socat - UNIX-CONNECT:"$MONITOR_SOCK" 2>/dev/null; sleep 0.3; }
wait_for() {
    local pattern="$1" timeout="${2:-30}" start=$SECONDS
    while ! grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; do
        [ $((SECONDS - start)) -ge $timeout ] && { echo "TIMEOUT: $pattern"; return 1; }
        sleep 1
    done
    echo "Found '$pattern' after $((SECONDS - start))s"
}

case "${1:-help}" in

install)
    echo "=== Step 1: Install ReactOS ==="
    [ ! -f "$BUILD_DIR/bootcd.iso" ] && { echo "Build bootcd first: ninja -C build bootcd"; exit 1; }

    pkill -9 -f wow64test 2>/dev/null || true
    truncate -s 10G "$DISK_IMG"
    rm -f "$SERIAL_LOG"

    qemu-system-x86_64 \
        -drive file="$DISK_IMG",format=raw,if=ide \
        -cdrom "$BUILD_DIR/bootcd.iso" \
        -boot d -m 512 \
        -serial file:"$SERIAL_LOG" \
        -monitor unix:"$MONITOR_SOCK",server,nowait \
        -display vnc=:85 \
        -no-reboot -nic none -enable-kvm -daemonize

    echo "QEMU started. VNC :85"
    echo "Navigate the installer manually via VNC (vncviewer :85)"
    echo "Use FAT quick format for simplicity."
    echo "When install finishes and QEMU exits, run: $0 inject"
    ;;

inject)
    echo "=== Step 2: Inject WoW64 files ==="
    [ ! -f "$DISK_IMG" ] && { echo "No disk image. Run '$0 install' first."; exit 1; }
    [ ! -f "$WOW64_DIR/dll/ntdll/ntdll.dll" ] && { echo "Build wow64 DLLs first: ninja -C build-wow64 ntdll kernel32"; exit 1; }

    # Build hello32 if not present
    if [ ! -f /tmp/hello32.exe ]; then
        cat > /tmp/hello32.c << 'HELLO_EOF'
#include <windows.h>
void __cdecl mainCRTStartup(void) {
    DWORD written;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(h, "Hello from WoW64!\n", 18, &written, NULL);
    ExitProcess(0);
}
HELLO_EOF
        i686-w64-mingw32-gcc -nostdlib -o /tmp/hello32.exe /tmp/hello32.c -lkernel32 -Wl,--entry,_mainCRTStartup
        echo "Built hello32.exe"
    fi

    # Find partition offset
    PART_START=$(fdisk -l "$DISK_IMG" 2>/dev/null | awk '/^\/tmp.*\*/{print $3}')
    [ -z "$PART_START" ] && PART_START=$(fdisk -l "$DISK_IMG" 2>/dev/null | awk '/^\/tmp/{print $2; exit}')
    [ -z "$PART_START" ] && { echo "Could not find partition. Check: fdisk -l $DISK_IMG"; exit 1; }
    OFFSET=$((PART_START * 512))
    echo "Partition at offset $OFFSET"

    # Mount and copy
    MOUNT_DIR=$(mktemp -d)
    sudo mount -o loop,offset=$OFFSET "$DISK_IMG" "$MOUNT_DIR"

    echo "Mounted at $MOUNT_DIR"
    ls "$MOUNT_DIR"

    # Create SysWOW64 and copy files
    sudo mkdir -p "$MOUNT_DIR/ReactOS/SysWOW64"
    sudo cp "$WOW64_DIR/dll/ntdll/ntdll.dll" "$MOUNT_DIR/ReactOS/SysWOW64/"
    sudo cp "$WOW64_DIR/dll/win32/kernel32/kernel32.dll" "$MOUNT_DIR/ReactOS/SysWOW64/"
    sudo cp /tmp/hello32.exe "$MOUNT_DIR/ReactOS/"

    echo "Copied:"
    ls -la "$MOUNT_DIR/ReactOS/SysWOW64/"
    ls -la "$MOUNT_DIR/ReactOS/hello32.exe"

    sudo umount "$MOUNT_DIR"
    rmdir "$MOUNT_DIR"
    echo "Done. Run: $0 test"
    ;;

test)
    echo "=== Step 3: Test hello32.exe ==="
    [ ! -f "$DISK_IMG" ] && { echo "No disk image."; exit 1; }

    pkill -9 -f wow64test 2>/dev/null || true
    rm -f "$SERIAL_LOG"

    qemu-system-x86_64 \
        -drive file="$DISK_IMG",format=raw,if=ide \
        -m 512 \
        -serial file:"$SERIAL_LOG" \
        -monitor unix:"$MONITOR_SOCK",server,nowait \
        -display vnc=:85 \
        -no-reboot -nic none -enable-kvm -daemonize

    echo "QEMU booting ReactOS. VNC :85"
    echo "Once at desktop, open cmd.exe and run: hello32.exe"
    echo "Watch serial log: tail -f $SERIAL_LOG"
    ;;

*)
    echo "Usage: $0 [install|inject|test]"
    echo ""
    echo "Prerequisites:"
    echo "  ninja -C build bootcd           # Build amd64 bootcd"
    echo "  ninja -C build-wow64 ntdll kernel32  # Build i386 WoW64 DLLs"
    echo ""
    echo "Steps:"
    echo "  $0 install  - Create disk, boot installer (VNC :85)"
    echo "  $0 inject   - Mount disk, copy SysWOW64 DLLs + hello32.exe"
    echo "  $0 test     - Boot ReactOS, run hello32.exe from cmd"
    ;;
esac

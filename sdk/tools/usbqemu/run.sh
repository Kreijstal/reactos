#!/bin/bash
# run.sh <kmtestcd.iso> <tag> <smp> <ide|usb>
#
# Boots one QEMU guest (KVM, -snapshot) with a qemu-xhci controller carrying
# three usb-storage disks, a USB CD and a tablet; kmtestcd boots from IDE or as
# a usb-storage CD, and kmtcdrunner runs RUN.LST from the IDE control disk
# (images from mkimgs.sh in $WORK/img). Prints and appends to
# $WORK/results.txt one verdict line: DONE (KMTCD-SUMMARY reached), HANG
# (hard timeout), HANG-STALL (COM1 silent for $STALL s or no KMTCD marker for
# $KMSTALL s: all vCPU registers, CPU0's stack and the PE image around its RIP
# are saved first), EXITn, "+CRASH" when COM1 shows a bugcheck or debugger.
# kmtcdrunner powers off through isa-debug-exit, so a finished run exits.
# COM1 carries the debug log (KDBG builds, _WINKD_ off), COM2 the test output.
#
# Environment: WORK (required), TMO hard timeout (1800 s), STALL (150),
# KMSTALL (300), HOTPLUG=1 (once UsbDiskQueue starts: unplug the usb0 disk
# while it is being read, re-plug it 5 s later, three times).
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:?set WORK}; I=$WORK/img
ISO=$1; TAG=$2; SMP=$3; BOOT=$4; TMO=${TMO:-1800}
O=$WORK/out/$TAG; mkdir -p "$O"; rm -f "$O"/*
MON=$O/mon.sock
if [ "$BOOT" = usb ]; then
  BOOTARGS=(-drive if=none,id=bootcd,format=raw,media=cdrom,readonly=on,file=$ISO
            -device usb-storage,bus=xhci.0,drive=bootcd,bootindex=0 -boot menu=off)
else
  BOOTARGS=(-drive file=$ISO,media=cdrom,if=ide,index=2,readonly=on -boot d)
fi
mon() { printf '%s\n' "$1" | socat -t"${2:-2}" - UNIX-CONNECT:"$MON"; }

timeout -k 10 "$TMO" qemu-system-x86_64 -name "usbqemu-$TAG" -m 3072 -smp "$SMP" -enable-kvm -snapshot \
  -device qemu-xhci,id=xhci,p2=8,p3=8 \
  "${BOOTARGS[@]}" \
  -drive file="$I/ctrl.img",format=raw,if=ide,index=0 \
  -drive if=none,id=u0,format=raw,file="$I/usb0.img" -device usb-storage,bus=xhci.0,drive=u0,id=uhp0 \
  -drive if=none,id=u1,format=raw,file="$I/usb1.img" -device usb-storage,bus=xhci.0,drive=u1 \
  -drive if=none,id=u2,format=raw,file="$I/usb2.img" -device usb-storage,bus=xhci.0,drive=u2 \
  -drive if=none,id=ucd,format=raw,media=cdrom,readonly=on,file="$I/usbcd.iso" -device usb-storage,bus=xhci.0,drive=ucd \
  -device usb-tablet,bus=xhci.0 \
  -netdev user,id=n0 -device rtl8139,netdev=n0 \
  -device isa-debug-exit,iobase=0xf4,iosize=0x01 \
  -serial file:"$O/com1.log" -serial file:"$O/com2.log" \
  -monitor unix:"$MON",server,nowait -display none -no-reboot > "$O/qemu.out" 2>&1 &
QPID=$!
START=$(date +%s)
alive() { kill -0 $QPID 2>/dev/null; }

if [ -n "$HOTPLUG" ]; then
  ( until cat "$O/com1.log" "$O/com2.log" 2>/dev/null | command grep -a -q 'KMTCD-BEGIN .*UsbDiskQueue'; do
      sleep 1; alive || exit 0; done
    sleep 2
    for c in 1 2 3; do
      mon "device_del uhp0" >> "$O/hotplug.log" 2>&1; echo "del $c $(date +%T)" >> "$O/hotplug.log"; sleep 5
      mon "drive_add 0 if=none,id=u0r$c,format=raw,file=$I/usb0.img,snapshot=on" >> "$O/hotplug.log" 2>&1
      mon "device_add usb-storage,bus=xhci.0,drive=u0r$c,id=uhp0" >> "$O/hotplug.log" 2>&1
      echo "add $c $(date +%T)" >> "$O/hotplug.log"; sleep 5
    done ) &
fi

# Stall watchdog.
sleep 20; last=0; since=$(date +%s); lastkm=-1; kmsince=$(date +%s)
while alive; do
  sz=$(cat "$O/com1.log" "$O/com2.log" 2>/dev/null | wc -c)
  [ "$sz" != "$last" ] && { last=$sz; since=$(date +%s); }
  km=$(cat "$O/com1.log" "$O/com2.log" 2>/dev/null | command grep -a -c KMTCD)
  [ "$km" != "$lastkm" ] && { lastkm=$km; kmsince=$(date +%s); }
  now=$(date +%s)
  if [ $((now - since)) -ge "${STALL:-150}" ] || [ $((now - kmsince)) -ge "${KMSTALL:-300}" ]; then
    echo "STALL at $(date +%T) com1=$sz" > "$O/stall.txt"
    for k in 1 2 3; do mon 'info registers -a' 5 >> "$O/regs$k.txt" 2>&1; sleep 2; done
    R1=$(sed 's/\x1b\[[0-9]*[A-Z]//g' "$O/regs1.txt")
    RIP=$(printf '%s\n' "$R1" | awk '/^CPU#/{c++} c==1 && /RIP=/{for(i=1;i<=NF;i++) if($i ~ /^RIP=/){sub("RIP=","",$i); print $i; exit}}')
    RSP=$(printf '%s\n' "$R1" | awk '/^CPU#/{c++} c==1 && /RSP=/{for(i=1;i<=NF;i++) if($i ~ /^RSP=/){sub("RSP=","",$i); print $i; exit}}')
    [ -n "$RSP" ] && mon "x /96gx 0x$RSP" 5 > "$O/stack0.txt" 2>&1
    [ -n "$RIP" ] && timeout 600 python3 -I "$HERE/findmod.py" "$MON" "$RIP" > "$O/findmod.txt" 2>&1
    mon quit 5 >/dev/null 2>&1
    break
  fi
  if [ $((now - START)) -ge "$TMO" ]; then break; fi
  sleep 5
done
wait $QPID; RC=$?
EL=$(( $(date +%s) - START ))
C1=$O/com1.log
SUM=$(cat "$O/com2.log" "$C1" | command grep -a -m1 KMTCD-SUMMARY | tr -d '\r')
ENDS=$(command grep -a KMTCD-END "$O/com2.log" | tr -d '\r' | sed 's/^.*KMTCD-END //; s/ exit=/|/' | paste -sd';')
CRASH=$(command grep -a -m1 -E 'Entered debugger|\*\*\* Fatal System Error|KeBugCheck|STOP: 0x|kdb:>|Unhandled exception|Assertion failed' "$C1" | tr -d '\r' | cut -c1-160)
if [ -f "$O/stall.txt" ]; then V=HANG-STALL; elif [ -n "$SUM" ]; then V=DONE; elif [ $RC = 124 ] || [ $RC = 137 ]; then V=HANG; else V=EXIT$RC; fi
[ -n "$CRASH" ] && V="$V+CRASH"
echo "$TAG verdict=$V t=${EL}s | $SUM | ends=$ENDS | crash=$CRASH" | tee -a "$WORK/results.txt"

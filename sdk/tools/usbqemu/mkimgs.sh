#!/bin/bash
# [LIST=removal] mkimgs.sh <work-dir> <build-dir>
#
# Builds the scratch images run.sh boots with, in <work-dir>/img, without
# mounting anything: three USB disks (usb0 FAT with a 96 MB payload file,
# usb1/usb2 carrying the per-sector pattern UsbDiskVerify checks), a USB data
# CD, and the IDE control disk holding RUN.LST, kernel32_apitest.exe and the
# reference copy of the payload. Needs sfdisk, mtools and python3.
set -e
WORK=${1:?work dir}; B=${2:?build dir}
HERE=$(cd "$(dirname "$0")" && pwd)
I=$WORK/img; mkdir -p "$I"; cd "$I"
mkdisk() { # name sizeMB label
  rm -f "$1"; truncate -s "${2}M" "$1"
  printf 'label: dos\n\n,,c,*\n' | sfdisk -q "$1"
  mformat -i "$1@@1048576" -F -v "$3" ::
}
mkdisk usb0.img 256 USB0
python3 -I "$HERE/mkpattern.py" usb1.img 64
python3 -I "$HERE/mkpattern.py" usb2.img 64
head -c 96M /dev/urandom > payload.bin
mcopy -i usb0.img@@1048576 payload.bin ::/PAYLOAD.BIN
rm -rf cdroot; mkdir cdroot; head -c 64M /dev/urandom > cdroot/data.bin
MK=$(find "$B/host-tools" -name mkisofs -type f -perm -u+x | head -1)
"$MK" -quiet -o usbcd.iso -J -R cdroot
rm -rf cdroot
rm -f ctrl.img; truncate -s 192M ctrl.img
printf 'label: dos\n\n,,6,*\n' | sfdisk -q ctrl.img
mformat -i ctrl.img@@1048576 -v CTRL ::
mcopy -i ctrl.img@@1048576 "$B/modules/rostests/apitests/kernel32/kernel32_apitest.exe" ::/
mcopy -i ctrl.img@@1048576 payload.bin ::/REF.BIN
rm -f payload.bin
# %D = control disk letter, %K = kmtest.exe (see kmtcdrunner). D: is usb0.
# LIST=removal: UsbDiskRemoval first, for run.sh HOTPLUG=1
# HOTPLUG_AFTER=USBDISK-UNPLUG-NOW (it waits for a disk to be unplugged).
if [ "$LIST" = removal ]; then
cat > RUN.LST <<'L'
%D:\kernel32_apitest.exe UsbDiskRemoval
%D:\kernel32_apitest.exe UsbDiskVerify
%D:\kernel32_apitest.exe UsbDiskQueue
%K UsbStorageRead
L
else
cat > RUN.LST <<'L'
cmd /c fc /b %D:\REF.BIN D:\PAYLOAD.BIN
%K UsbStorageRead
%D:\kernel32_apitest.exe UsbDiskVerify
%D:\kernel32_apitest.exe UsbDiskQueue
%D:\kernel32_apitest.exe UsbConcurrentDevices
%K UsbStorageRead
L
fi
sed -i 's/$/\r/' RUN.LST
mcopy -i ctrl.img@@1048576 RUN.LST ::/RUN.LST
rm -f RUN.LST
ls -la "$I"

# mkpattern.py <image> <MB>: raw disk whose every 512-byte sector starts with the
# UsbDiskVerify pattern header ("USBV" magic, LBA) followed by LBA-derived words.
import sys, struct
path, mb = sys.argv[1], int(sys.argv[2])
n = mb * 2048
with open(path, 'wb') as f:
    for lba in range(n):
        w = [0x56425355, lba, 0] + [((lba * 2654435761) ^ (i * 40503)) & 0xffffffff for i in range(3, 128)]
        f.write(struct.pack('<128I', *w))

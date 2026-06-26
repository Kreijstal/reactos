#!/usr/bin/env python3
#
# PROJECT:     ReactOS host-side test runner
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Build a tiny FAT "control disk" that steers the in-guest runners
#              to a single selected test, written entirely from user space
#              (mkfs.fat + mcopy + sfdisk -- no loopback mount, no root).
#
# A bare FAT image attached as a hard disk gets no drive letter (partmgr only
# enumerates partitions of an MBR-partitioned disk), so the image is built as a
# 16 MB disk with one FAT16 partition at LBA 2048, formatted and populated via
# mtools, then assembled with an MBR.  The in-guest hooks (kmtcdrunner and
# regtest.cmd) scan the drive letters for the well-known filename.

import os
import shutil
import subprocess

# Well-known filenames the in-guest hooks look for on the control volume.
KMTEST_FILE = "KMTEST.SEL"      # plain text: the kmtest name to run
ROSTESTS_FILE = "ROSTEST.CMD"   # a batch file regtest.cmd will "call"

LABEL = "ROSTESTCTL"
_PART_START = 2048              # sectors
_DISK_SECTORS = 32768          # 16 MB total


def available() -> bool:
    return all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "sfdisk"))


def write(img_path: str, filename: str, content: str, batch: bool = False):
    """Create img_path as an MBR+FAT16 control disk containing `filename` with
    `content`.  Returns img_path, or None if the host FAT tools are missing."""
    if not available():
        return None

    # Batch files the guest 'call's want CRLF line endings.
    data = content
    if batch and not data.endswith("\n"):
        data += "\n"
    if batch:
        data = data.replace("\n", "\r\n")

    part = img_path + ".part"
    payload = img_path + ".payload"
    part_sectors = _DISK_SECTORS - _PART_START
    try:
        with open(payload, "w", newline="") as f:
            f.write(data)

        subprocess.run(["truncate", "-s", str(part_sectors * 512), part], check=True)
        subprocess.run(["mkfs.fat", "-F", "16", "-n", LABEL, part],
                       check=True, capture_output=True)
        subprocess.run(["mcopy", "-i", part, payload, f"::{filename}"],
                       check=True, capture_output=True)

        subprocess.run(["truncate", "-s", str(_DISK_SECTORS * 512), img_path], check=True)
        subprocess.run(["sfdisk", img_path], check=True, capture_output=True,
                       input=f"label: dos\nstart={_PART_START}, size={part_sectors}, "
                             f"type=6, bootable\n".encode())
        subprocess.run(["dd", f"if={part}", f"of={img_path}", "bs=512",
                        f"seek={_PART_START}", "conv=notrunc"],
                       check=True, capture_output=True)
    finally:
        for f in (part, payload):
            if os.path.exists(f):
                os.unlink(f)
    return img_path

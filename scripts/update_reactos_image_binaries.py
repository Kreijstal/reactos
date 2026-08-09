#!/usr/bin/env python3
#
# Reproducibly update selected binaries in an installed ReactOS qcow2 image.
#

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


DEFAULT_FILES = (
    ("win32ss/win32k.sys", "/ReactOS/System32/win32k.sys"),
    ("dll/win32/msafd/msafd.dll", "/ReactOS/System32/msafd.dll"),
)


def run(argv, *, check=True, capture=False):
    kwargs = {
        "text": True,
        "check": check,
    }
    if capture:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.STDOUT
    return subprocess.run(argv, **kwargs)


def require_tool(name):
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required tool not found: {name}")
    return path


def check_image_unlocked(image):
    result = run(["qemu-img", "info", str(image)], check=False, capture=True)
    if result.returncode != 0:
        raise RuntimeError(
            "qemu-img cannot open the image. Is QEMU still running?\n" + result.stdout
        )


def parse_file_mapping(mapping):
    if ":" not in mapping:
        raise argparse.ArgumentTypeError("file mapping must be SRC:DST")
    src, dst = mapping.split(":", 1)
    if not src or not dst.startswith("/"):
        raise argparse.ArgumentTypeError("SRC:DST requires a non-empty SRC and absolute guest DST")
    return src, dst


def resolve_payload(build_dir, mappings):
    payload = []
    for src, dst in mappings:
        source = Path(src)
        if not source.is_absolute():
            source = build_dir / source
        source = source.resolve()
        if not source.is_file():
            raise RuntimeError(f"source file does not exist: {source}")
        payload.append((source, dst))
    return payload


def guestfish_update(image, payload, dry_run):
    require_tool("guestfish")

    commands = ["list-filesystems"]
    for source, dst in payload:
        commands.append(f"is-file {dst}")
        if not dry_run:
            commands.append(f"upload {source} {dst}")

    script = "\n".join(commands) + "\n"
    result = subprocess.run(
        ["guestfish", "-a", str(image), "-i"],
        input=script,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stdout)


def nbd_update(image, payload, dry_run, nbd_device):
    require_tool("sudo")
    require_tool("qemu-nbd")
    require_tool("ntfs-3g")
    require_tool("partprobe")

    mount_dir = Path(tempfile.mkdtemp(prefix="reactos-image-"))
    connected = False
    mounted = False
    try:
        run(["sudo", "-n", "modprobe", "nbd"])
        run(["sudo", "-n", "qemu-nbd", "--connect", nbd_device, str(image)])
        connected = True
        run(["sudo", "-n", "partprobe", nbd_device])

        partition = f"{nbd_device}p1"
        if not Path(partition).exists():
            raise RuntimeError(f"expected partition device not found: {partition}")

        run(["sudo", "-n", "ntfs-3g", partition, str(mount_dir), "-o", "windows_names"])
        mounted = True

        for source, dst in payload:
            target = mount_dir / dst.lstrip("/")
            if not target.exists():
                raise RuntimeError(f"guest target does not exist: {dst}")
            if not dry_run:
                run(["sudo", "-n", "cp", "-f", str(source), str(target)])
                run(["sync"])
    finally:
        if mounted:
            run(["sudo", "-n", "umount", str(mount_dir)], check=False)
        if connected:
            run(["sudo", "-n", "qemu-nbd", "--disconnect", nbd_device], check=False)
        mount_dir.rmdir()


def start_fuse_export(image, raw_path, pid_path):
    require_tool("sudo")
    require_tool("qemu-storage-daemon")
    raw_path.touch()
    run([
        "sudo",
        "-n",
        "qemu-storage-daemon",
        "--daemonize",
        "--pidfile",
        str(pid_path),
        "--blockdev",
        f"driver=file,node-name=file,filename={image}",
        "--blockdev",
        "driver=qcow2,node-name=qcow,file=file",
        "--export",
        f"type=fuse,id=reactos,node-name=qcow,mountpoint={raw_path},writable=on",
    ])
    for _ in range(50):
        if raw_path.exists() and pid_path.exists():
            return
        time.sleep(0.1)
    raise RuntimeError("qemu-storage-daemon did not create the FUSE export")


def stop_fuse_export(pid_path):
    if not pid_path.exists():
        return
    try:
        pid = int(pid_path.read_text().strip())
    except ValueError:
        return
    run(["sudo", "-n", "kill", str(pid)], check=False)
    for _ in range(50):
        if run(["sudo", "-n", "kill", "-0", str(pid)], check=False).returncode != 0:
            return
        time.sleep(0.1)
    run(["sudo", "-n", "kill", "-9", str(pid)], check=False)


def first_partition_offset(raw_path):
    require_tool("sudo")
    require_tool("sfdisk")
    result = run(["sudo", "-n", "sfdisk", "--json", str(raw_path)], capture=True)
    table = json.loads(result.stdout)["partitiontable"]
    sector_size = int(table.get("sector-size", 512))
    partitions = table.get("partitions", [])
    if not partitions:
        raise RuntimeError("no partitions found in image")
    return int(partitions[0]["start"]) * sector_size


def fuse_update(image, payload, dry_run):
    require_tool("sudo")
    require_tool("ntfs-3g")
    require_tool("fusermount3")

    temp_dir = Path(tempfile.mkdtemp(prefix="reactos-image-fuse-"))
    raw_path = temp_dir / "disk.raw"
    pid_path = temp_dir / "qemu-storage-daemon.pid"
    mount_dir = temp_dir / "mnt"
    mount_dir.mkdir()
    mounted = False
    try:
        start_fuse_export(image, raw_path, pid_path)
        offset = first_partition_offset(raw_path)
        run(["sudo", "-n", "ntfs-3g", str(raw_path), str(mount_dir), "-o", f"offset={offset},windows_names"])
        mounted = True

        for source, dst in payload:
            target = mount_dir / dst.lstrip("/")
            if not target.exists():
                raise RuntimeError(f"guest target does not exist: {dst}")
            if not dry_run:
                run(["sudo", "-n", "cp", "-f", str(source), str(target)])
                run(["sync"])
    finally:
        if mounted:
            run(["sudo", "-n", "fusermount3", "-u", str(mount_dir)], check=False)
        stop_fuse_export(pid_path)
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="reactos.qcow2", type=Path)
    parser.add_argument("--build-dir", default="build_nt10", type=Path)
    parser.add_argument("--backend", choices=("auto", "guestfish", "fuse", "nbd"), default="auto")
    parser.add_argument("--nbd-device", default="/dev/nbd0")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--file",
        action="append",
        type=parse_file_mapping,
        dest="files",
        help="file mapping as SRC:DST. Relative SRC is resolved below --build-dir.",
    )
    args = parser.parse_args()

    image = args.image.resolve()
    build_dir = args.build_dir.resolve()
    mappings = args.files or list(DEFAULT_FILES)
    payload = resolve_payload(build_dir, mappings)

    check_image_unlocked(image)

    print(f"image: {image}")
    print(f"build: {build_dir}")
    for source, dst in payload:
        action = "would update" if args.dry_run else "update"
        print(f"{action}: {source} -> {dst}")

    if args.backend in ("auto", "guestfish"):
        try:
            guestfish_update(image, payload, args.dry_run)
            return 0
        except Exception as exc:
            if args.backend == "guestfish":
                print(exc, file=sys.stderr)
                return 1
            print(f"guestfish backend failed, trying fuse: {exc}", file=sys.stderr)

    if args.backend in ("auto", "fuse"):
        try:
            fuse_update(image, payload, args.dry_run)
            return 0
        except Exception as exc:
            if args.backend == "fuse":
                print(exc, file=sys.stderr)
                return 1
            print(f"fuse backend failed, trying nbd: {exc}", file=sys.stderr)

    try:
        nbd_update(image, payload, args.dry_run, args.nbd_device)
    except Exception as exc:
        print(exc, file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
payload_dir="$repo_root/scripts/sshd_autorun_usb"
out="${1:-$repo_root/build_nt10/sshd_autorun_usb.img}"

mkdir -p "$(dirname "$out")"
rm -f "$out"
truncate -s 16M "$out"
mkfs.vfat -n SSHDUSB "$out" >/dev/null

mcopy -i "$out" "$payload_dir/autorun.inf" ::/autorun.inf
mcopy -i "$out" "$payload_dir/start-sshd.cmd" ::/start-sshd.cmd

mdir -i "$out" ::/
echo "$out"

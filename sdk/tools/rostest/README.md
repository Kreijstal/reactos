# rostest — host-side ReactOS test runner

`rostest.py` boots a ReactOS VM, runs a selected test suite (or a single test),
scrapes the serial log, and reports pass/fail. It drives the two in-guest
runners that already ship with ReactOS:

- **kmtest** — `kmtcdrunner` runs the kernel-mode kmtests from a bootable
  `kmtest.img` and emits `KMTCD-*` serial markers.
- **rostests** — `rosautotest` runs the user-mode Wine conformance tests in an
  installed system and prints `module: N tests executed (M todo, K failures)`.

The emulator is a **runtime dependency**, not a build dependency. Nothing here
is compiled; the tool is pure host-side Python.

## Usage

```sh
# one kernel test
rostest.py --suite kmtest --test Example --kmtest-img build/kmtest.img

# one winetest group on an existing install (fast: boot + run)
rostest.py --suite rostests --test gdi32:bitmap --image reactos.qcow2

# reinstall first, then run a group
rostest.py --suite rostests --test comctl32:imagelist --image reactos.qcow2 --iso build/bootcdregtest.iso --fresh

# everything
rostest.py --suite all --image reactos.qcow2 --kmtest-img build/kmtest.img
```

`--test` selects one group: a kmtest name (prefix match, `!name` to invert) for
the kmtest suite, or `module` / `module:test` for rostests. Omit it to run the
whole suite. Exit code is 0 only if every selected group reported 0 failures.

## CTest integration

Configure the main build with `-DENABLE_ROSTEST_CTEST=ON` and point it at an
installed image:

```sh
cmake -DENABLE_ROSTEST_CTEST=ON -DROSTEST_IMAGE=/path/reactos.qcow2 ...
ctest -R rostest_gdi32
ctest -R kmtest_NtfsDirIndex
ctest -R 'rostest_.*'        # every winetest module
```

One CTest entry is registered per winetest module and per kmtest name, plus
`rostest_all` / `kmtest_all`. If no VM backend is found at configure time the
entries are registered **DISABLED** rather than failing the build. Relevant
cache variables: `ROSTEST_BACKEND`, `ROSTEST_IMAGE`, `ROSTEST_ISO`,
`ROSTEST_KMTEST_IMG`, `ROSTEST_TIMEOUT`.

## Per-test selection on a reused install — the control disk

Both in-guest runners normally bake the test selection into the build artifact.
To pick a test on an already-built image without rebuilding, the tool writes a
tiny FAT "control disk" (via `mkfs.fat`/`mcopy`/`sfdisk`, no mount, no root) and
attaches it. Two small, strictly-additive guest hooks read it; when no control
disk is attached, behaviour is unchanged:

- `base/system/kmtcdrunner/kmtcdrunner.c` reads `KMTEST.SEL` if present.
- `boot/bootdata/bootcdregtest/regtest.cmd` calls `ROSTEST.CMD` if present.

If the FAT host tools are missing, selection falls back to `--fresh`
(bake-and-reinstall).

## Backends

- **qemu** (default) — `qemu-system-x86_64`. The verified backend.
- **virtualbox** — `VBoxManage`. Wired from the documented command surface but
  not part of the verified path on hosts without VirtualBox installed.

Override the emulator binaries via `ROSTEST_QEMU` / `ROSTEST_QEMU_IO` /
`ROSTEST_VBOXMANAGE`.

## Host dependencies

- A VM backend: `qemu-system-x86_64` (+ `qemu-io` for `--fresh`) or `VBoxManage`.
- For the control-disk fast-select path: `mkfs.fat` (dosfstools), `mcopy`
  (mtools), `sfdisk` (util-linux). Optional — absent ⇒ `--fresh` selection.
- For the kmtest NTFS data disk: `mkntfs` (ntfs-3g), `sfdisk`. Optional.

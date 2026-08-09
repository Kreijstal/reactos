# Local build notes (GCC, per-target NT version)

Out-of-tree GCC (mingw-w64) builds with ccache, one build dir per target NT
version. Toolchain: `toolchain-gcc.cmake` (uses `x86_64-w64-mingw32-gcc/g++`
for amd64).

## NT10 / amd64 (the usual one)

```sh
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake \
  -DARCH=amd64 -DREACTOS_TARGET_NT=0x0A00 -DENABLE_CCACHE=1 \
  -DCMAKE_BUILD_TYPE=Debug -S . -B build_nt10

ninja -C build_nt10 bootcd livecd      # or: ninja -C build_nt10 <target>
```

## Per-target matrix

Same command, change `REACTOS_TARGET_NT` and the build dir:

| Build dir    | Target      | REACTOS_TARGET_NT | ARCH  |
|--------------|-------------|-------------------|-------|
| build_nt52   | Server 2003 | `0x502`           | i386  |
| build_nt6    | Vista       | `0x600`           | i386  |
| build_nt61   | Win7        | `0x601`           | i386  |
| build_nt62   | Win8        | `0x602`           | i386  |
| build_nt10   | Win10       | `0x0A00`          | amd64 |

`REACTOS_TARGET_NT` derives `NTDDI_VERSION` automatically (override
`REACTOS_TARGET_NTDDI` directly to pick a non-RTM level). For i386 omit
`-DARCH` (default) or pass `-DARCH=i386`.

## ccache

`-DENABLE_CCACHE=1` is the built-in knob (sdk/cmake/gcc.cmake +
CMakeLists.txt): it sets `RULE_LAUNCH_COMPILE ccache` and turns precompiled
headers OFF. Because it flips PCH, enabling/disabling ccache on an *existing*
build dir forces a full rebuild — set it at first configure (or accept the
one-time rebuild when reconfiguring).

## ROSTESTS

Add `-DENABLE_ROSTESTS=1` to build the kmtests/rostests (build_nt62 has this on).

## Windows host

The line above is for a Linux cross-mingw host (compiler `x86_64-w64-mingw32-gcc`).
On Windows the supported path is **RosBE** (bundles the `x86_64-w64-mingw32-*`
cross tools + ninja + matching cmake); open the RosBE shell and the same line
works unchanged.

In MSYS2 (UCRT64) use the dedicated **`toolchain-msys64.cmake`** — do NOT use
`toolchain-gcc.cmake` here. The MSYS2 file uses the unprefixed tools (`gcc`,
`windmc`, …) *and* forces `CMAKE_CROSSCOMPILING TRUE`; without that flag CMake
sees host==target==Windows, takes the native path, and never adds the ReactOS
targets (so `toolchain-gcc.cmake -DMINGW_TOOLCHAIN_PREFIX=` would configure but
build nothing).

```sh
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-ccache
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-msys64.cmake \
  -DARCH=amd64 -DREACTOS_TARGET_NT=0x0A00 -DENABLE_CCACHE=1 \
  -DCMAKE_BUILD_TYPE=Debug -S . -B build_nt10
ninja -C build_nt10 bootcd livecd
```

Note: ReactOS is validated against msvcrt mingw (RosBE), not the UCRT runtime.
UCRT64 affects only host tools (target is `-nostdlib`); usually fine, fall back
to RosBE if host-tool oddities appear.

GOTCHA: a *failed* configure still writes `MINGW_TOOLCHAIN_PREFIX` (and the
generator) into `<dir>/CMakeCache.txt`. The msys64 file only sets the empty
prefix `if(NOT DEFINED ...)`, so a stale cached `x86_64-w64-mingw32-` from a
prior `toolchain-gcc.cmake` attempt sticks and it keeps hunting for
`x86_64-w64-mingw32-windmc`. When switching toolchain files, `rm -rf <dir>`
first (or pass `-DMINGW_TOOLCHAIN_PREFIX=` to overwrite the cache entry).

The configure line `Not cross-compiling, no special host-tools cmake command`
(host-tools.cmake:55) is expected when target ARCH == host arch (amd64 on an
amd64 Windows host): host tools build with the same gcc. It does NOT mean the
ReactOS targets were skipped — that path is gated separately on
CMAKE_CROSSCOMPILING, which toolchain-msys64.cmake forces TRUE.

GCC note: MSYS2 ucrt64 ships very new GCC (e.g. 16.x), much newer than RosBE's.
ReactOS isn't routinely tested there, so expect possible new -Werror/codegen
issues during the build.

<p align=center>
  <a href="https://reactos.org/">
    <img alt="ReactOS" src="https://reactos.org/wiki/images/0/02/ReactOS_logo.png">
  </a>
</p>

---

<p align=center>
  <a href="https://reactos.org/project-news/reactos-0415-released/">
    <img alt="ReactOS 0.4.15 Release" src="https://img.shields.io/badge/release-0.4.15-0688CB.svg"></a>
  <a href="https://reactos.org/download/">
    <img alt="Download ReactOS" src="https://img.shields.io/badge/download-latest-0688CB.svg"></a>
  <a href="https://sourceforge.net/projects/reactos/">
    <img alt="SourceForge Download" src="https://img.shields.io/sourceforge/dm/reactos.svg?colorB=0688CB"></a>
  <a href="https://github.com/reactos/reactos/blob/master/COPYING">
    <img alt="License" src="https://img.shields.io/badge/license-GNU_GPL_2.0-0688CB.svg"></a>
  <a href="https://reactos.org/donate/">
    <img alt="Donate" src="https://img.shields.io/badge/%24-donate-E44E4A.svg"></a>
  <a href="https://twitter.com/reactos">
    <img alt="Follow on Twitter" src="https://img.shields.io/twitter/follow/reactos.svg?style=social&label=Follow%20%40reactos"></a>
</p>

## Quick Links
[Website](https://reactos.org/) &bull;
[Official chat](https://chat.reactos.org/) &bull;
[Wiki](https://reactos.org/wiki/) &bull;
[Forum](https://reactos.org/forum/) &bull;
[Community Discord](https://discord.gg/7knjvhT) &bull;
[JIRA Bug Tracker](https://jira.reactos.org/issues/) &bull;
[ReactOS Git mirror](https://git.reactos.org/) &bull;
[Testman](https://reactos.org/testman/)

## What is ReactOS?

ReactOS™ is an Open Source effort to develop a quality operating system that is compatible with applications and drivers written for the Microsoft® Windows™ NT family of operating systems (NT4, 2000, XP, 2003, Vista, 7).

The ReactOS project, although currently focused on Windows Server 2003 compatibility, is always keeping an eye toward compatibility with Windows Vista and future Windows NT releases.

The code of ReactOS is licensed under [GNU GPL 2.0](https://github.com/reactos/reactos/blob/master/COPYING).

### Product quality warning

**ReactOS is currently an Alpha quality operating system.** This means that ReactOS is under heavy development and you have to be ready to encounter some problems. Different things may not work well and it can corrupt the data present on your hard disk. It is HIGHLY recommended to test ReactOS on a virtual machine or on a computer with no sensitive or critical data!

## Building

![Build](https://github.com/reactos/reactos/workflows/Build/badge.svg) [![rosbewin.badge]][rosbewin.link] [![rosbeunix.badge]][rosbeunix.link] [![coverity.badge]][coverity.link]

### Recommended build entry points

1. RosBE (Windows, recommended)
2. MSYS2 (Windows, MinGW64 shell)
3. GNU/Linux with CMake + Ninja
4. MSVC 2019+ (official docs: ["Visual Studio or Microsoft Visual C++"](https://reactos.org/wiki/CMake#Visual_Studio_or_Microsoft_Visual_C.2B.2B))

RosBE package details are in ["Build Environment"](https://reactos.org/wiki/Build_Environment). For complete build and target details, use ["Building ReactOS"](https://reactos.org/wiki/Building_ReactOS).

### CMake + Ninja (all platforms)

From the repository root:

```bash
cmake -S . -B build -G Ninja \
  -DARCH=amd64 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Build only a specific target:

```bash
cmake --build build --target bootcd
```

This produces `build/bootcd.iso`.

### MSYS2 (Windows)

In the **MSYS2 MinGW 64-bit** shell:

```bash
pacman -S --needed --noconfirm \
  git base-devel python3 make ninja \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-flex \
  mingw-w64-x86_64-bison

cmake -S . -B build -G Ninja \
  -DARCH=amd64 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

If you are on MSYS2/ucrt64 and want to force the dedicated toolchain, use the integrated toolchain file:

```bash
cmake -S . -B build-msys64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-msys64.cmake \
  -DARCH=amd64 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-msys64
```

`toolchain-msys64.cmake` exists and is intended for this MSYS2 flow (it sets cross-compiling mode and uses the MSYS2-hosted GCC toolchain).

### MSVC (Windows)

Use Microsoft Visual C++ 2019+ and the guidance linked above.

### WoW64 (amd64 with 32-bit guest)

Use two builds:

```bash
cmake -S . -B build-i386 -G Ninja \
  -DARCH=i386 \
  -DSARCH=wow64 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-i386

cmake -S . -B build-amd64 -G Ninja \
  -DARCH=amd64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DREACTOS_WOW64_GUEST_DIR=$PWD/build-i386
cmake --build build-amd64 --target bootcd
```

Legacy helpers (`configure.cmd` / `configure.sh`) are still present, but current instructions use direct CMake calls as shown above.

### CMake integrado (RosBE / entorno ReactOS)

RosBE includes a preconfigured CMake flow through `configure.sh` (MSYS2/Unix shell) and `configure.cmd` (Windows cmd). It wires `toolchain-gcc.cmake` automatically and sets the build architecture from `ROS_ARCH`.

```bash
./configure.sh -DREACTOS_TARGET_NT=0x0A00 -DCMAKE_BUILD_TYPE=Debug
ninja -C output-MiniGW-amd64
```

Equivalent Windows flow:

```bat
configure.cmd
ninja -C output-MiniGW-amd64
```

You can pass any `-D...` CMake options to the configure scripts (for example `-DREACTOS_TARGET_NT=0x0A00`), and they forward directly to the CMake invocation they generate.

You can always download fresh binary builds from the ["Daily builds"](https://reactos.org/getbuilds/) page.

## Installing

By default, ReactOS currently can only be installed on a machine that has a FAT16 or FAT32 partition as the active (bootable) partition.
The partition on which ReactOS is to be installed (which may or may not be the bootable partition) must also be formatted as FAT16 or FAT32.
ReactOS Setup can format the partitions if needed.

Starting with 0.4.10, ReactOS can be installed using the BtrFS file system. But consider this as an experimental feature and thus regressions not triggered on FAT setup may be observed.

To install ReactOS from the bootable CD distribution, extract the archive contents. Then burn the CD image, boot from it, and follow the instructions.

See ["Installing ReactOS"](https://reactos.org/wiki/Installing_ReactOS) Wiki page or [INSTALL](INSTALL) for more details.

## Testing

If you discover a bug in ReactOS search on JIRA first - it might be reported already. If not report the bug providing logs and as much information as possible.

See ["File Bugs"](https://reactos.org/wiki/File_Bugs) for a guide.

__NOTE:__ The bug tracker is _not_ for discussions. Please use our [official chat](https://chat.reactos.org/) or our [forum](https://reactos.org/forum/).

## Contributing  [![prwelcome.badge]](https://reactos.org/wiki/Commiting_Changes)

We are always looking for developers! Check [how to contribute](CONTRIBUTING.md) if you are willing to participate.

__Legal notice__: If you have seen proprietary Microsoft Windows source code (including but not limited to the leaked Windows NT 3.5, NT 4, 2000 source code and the Windows Research Kernel), your contribution won't be accepted because of potential copyright violation.

Try out cloud-based ReactOS development using Gitpod and Docker:

[![Open in Gitpod](https://gitpod.io/button/open-in-gitpod.svg)](https://gitpod.io/#https://github.com/reactos/reactos)

You can also support ReactOS by [donating](https://reactos.org/donate/)! We rely on our backers to maintain our servers and accelerate development by [hiring full-time devs](https://reactos.org/contributing/#paid-jobs).

## More information

ReactOS is a Free and Open Source operating system based on the Windows architecture,
providing support for existing applications and drivers, and an alternative to the current dominant consumer operating system.

It is not another wrapper built on Linux, like WINE. It does not attempt or plan to compete with WINE; in fact, the user-mode part of ReactOS is almost entirely WINE-based and our two teams have cooperated closely in the past.

ReactOS is also not "yet another OS". It does not attempt to be a third player like any other alternative OS out there. People are not meant to uninstall Linux and use ReactOS instead; ReactOS is a replacement for Windows users who want a Windows replacement that behaves just like Windows.

More information is available at: [reactos.org](https://reactos.org/).

Also see the [media/doc](/media/doc/) subdirectory for some sparse notes.

## Who is responsible

Active devs are listed as members of [GitHub organization](https://github.com/orgs/reactos/people).
See also the [CREDITS](CREDITS) file for others.

## Code mirrors

The main development is done on [GitHub](https://github.com/reactos/reactos). We have an [alternative mirror](https://git.reactos.org/?p=reactos.git) in case GitHub is down.

There is also an obsolete [SVN archive repository](https://svn.reactos.org/reactos/) that is kept for historical purposes.

[coverity.badge]:   https://scan.coverity.com/projects/205/badge.svg?flat=1
[rosbewin.badge]:   https://img.shields.io/badge/RosBE_Windows-2.2.1-0688CB.svg
[rosbeunix.badge]:  https://img.shields.io/badge/RosBE_Unix-2.2.1-0688CB.svg
[prwelcome.badge]:  https://img.shields.io/badge/PR-welcome-0688CB.svg

[coverity.link]:    https://scan.coverity.com/projects/205
[rosbewin.link]:    https://sourceforge.net/projects/reactos/files/RosBE-Windows/i386/2.2.1/
[rosbeunix.link]:   https://sourceforge.net/projects/reactos/files/RosBE-Unix/2.2.1/

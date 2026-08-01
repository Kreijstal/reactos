# RTL8168 KDNET transport

This boot-time AMD64 transport uses the RTL8168/8111 PCI Ethernet controller
without NDIS. FreeLoader selects it as `kdnet.dll` with `DEBUGPORT=NET`.

Build ReactOS with the WinKD debugger core. The built-in KDBG core consumes
state-change packets locally and cannot drive a remote KDNET session:

```text
cmake -S . -B build-kdnet -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake \
  -DARCH=amd64 -DCMAKE_BUILD_TYPE=Debug \
  -D_WINKD_=TRUE -DKDBG=FALSE
cmake --build build-kdnet --target kdnet ntoskrnl
```

The boot-CD packaging rules also put `kdnet.dll` in the AMD64 setup source and
list it for installation into `system32`.

The initial implementation deliberately uses static IPv4 and polling. A
typical direct-link configuration is:

```text
/DEBUG /DEBUGPORT=NET /HOSTIP=192.168.250.1 /TARGETIP=192.168.250.2 /PORT=50000 /KEY=1.2.3.4
```

Configure the debugger host's Ethernet interface as `192.168.250.1/24` and
start the KDNET consumer before booting the target. For Radare2:

```text
r2 -d winkd://192.168.250.2:50000:1.2.3.4
```

The target resolves the host MAC with ARP. `HOSTMAC=xx-xx-xx-xx-xx-xx` can be
supplied to bypass ARP.

The transport reserves the NIC through the HAL debug-device interface. The
normal RTL8168 NDIS miniport therefore must not use the device while KDNET is
active.

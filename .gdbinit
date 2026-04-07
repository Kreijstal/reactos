# ReactOS GDB debug helper for QEMU
# Usage: gdb -x .gdbinit
#
# Auto-detects real mode (bootloader) vs long mode (kernel).
# Connects to QEMU gdbstub and loads symbols for ntoskrnl + drivers.

set architecture i386:x86-64
set disassembly-flavor intel
set pagination off
set confirm off

# QEMU gdbstub default — wrapped so sourcing twice doesn't abort
python
try:
    gdb.execute("target remote :1234")
except Exception as e:
    pass  # already connected or QEMU not running
end

python
import gdb
import struct
import os

KSEG0_BASE = 0xFFFFF80000000000
BUILD_DIR = os.path.join(os.getcwd(), 'build')

def detect_cpu_mode():
    """Detect CPU mode by reading CR0, CS, and EFER.
    Returns 'real', 'protected', or 'long'.
    Falls back to CS-based heuristic if EFER is unavailable
    (QEMU gdbstub doesn't expose MSRs in all configurations)."""
    try:
        cr0 = int(gdb.parse_and_eval("$cr0"))
        cs = int(gdb.parse_and_eval("$cs"))
    except:
        return "unknown"

    pe = cr0 & 1          # Protected mode enable
    if not pe:
        return "real"

    # Try EFER first (may return void in batch mode)
    try:
        efer_val = gdb.parse_and_eval("$efer")
        efer = int(efer_val)
        lma = efer & 0x400    # Long mode active
        if lma:
            return "long"
        return "protected"
    except (gdb.error, ValueError, TypeError):
        pass

    # EFER unavailable — use CS heuristic:
    # CS=0x10 is the typical kernel code segment in long mode
    # CS=0x33 is user-mode long mode
    # CS < 0x100 with PE set is likely long or protected mode
    if cs in (0x10, 0x33):
        return "long"

    # Check RIP — if above 4GB, must be long mode
    try:
        rip = int(gdb.parse_and_eval("$rip"))
        if rip > 0xFFFFFFFF:
            return "long"
    except:
        pass

    return "protected"

cpu_mode = detect_cpu_mode()
print(f"\n=== CPU mode: {cpu_mode} ===")

if cpu_mode == "real":
    # Real mode: bootloader debugging (16-bit)
    print("Bootloader debugging mode (16-bit real mode)")
    print("Note: GDB shows x86-64 registers but CPU is in real mode.")
    print("  Addresses are CS:IP style, use segment*16+offset.")
    print("")
    print("Useful addresses for NTFS VBR (boot/freeldr/bootsect/ntfs.S):")
    print("  0x7c00       VBR entry point")
    print("  0x7e00       Extra boot code (sectors 2-3)")

    # Try to detect if VBR is loaded
    try:
        inf = gdb.selected_inferior()
        oem = bytes(inf.read_memory(0x7c03, 8))
        if oem == b'NTFS    ':
            print("  NTFS VBR detected at 0x7c00")
    except:
        pass

    # Find error handler addresses from the VBR binary if available
    vbr_bin = os.path.join(BUILD_DIR, 'boot', 'freeldr', 'bootsect', 'ntfs.bin')
    if os.path.exists(vbr_bin):
        with open(vbr_bin, 'rb') as f:
            data = f.read()
        idx = data.find(b'Disk error')
        if idx >= 0:
            # Find mov si pointing to the string
            for i in range(idx - 10, idx):
                if i >= 0 and data[i] == 0xBE:
                    val = int.from_bytes(data[i+1:i+3], 'little')
                    if val == 0x7c00 + idx:
                        print(f"  PrintDiskError   @ 0x{0x7c00+i:x}")
                        break
        idx2 = data.find(b'File system error')
        if idx2 >= 0:
            for i in range(idx2 - 10, idx2):
                if i >= 0 and data[i] == 0xBE:
                    val = int.from_bytes(data[i+1:i+3], 'little')
                    if val == 0x7c00 + idx2:
                        print(f"  PrintFSError     @ 0x{0x7c00+i:x}")
                        break
        # Find INT 13h calls
        for i in range(len(data) - 1):
            if data[i] == 0xCD and data[i+1] == 0x13:
                print(f"  INT 13h          @ 0x{0x7c00+i:x}")
    print("")

else:
    # Long mode: kernel debugging (64-bit)
    gdb.execute("set architecture i386:x86-64")
    print("Kernel debugging mode (64-bit long mode)")
    print("")

# ============================================================
# Helper functions (used by both modes)
# ============================================================

def read_mem(addr, size):
    """Read memory from target, return bytes or None on failure."""
    try:
        inferior = gdb.selected_inferior()
        return bytes(inferior.read_memory(addr, size))
    except:
        return None

def read_u16(addr):
    data = read_mem(addr, 2)
    return struct.unpack('<H', data)[0] if data else None

def read_u32(addr):
    data = read_mem(addr, 4)
    return struct.unpack('<I', data)[0] if data else None

def read_u64(addr):
    data = read_mem(addr, 8)
    return struct.unpack('<Q', data)[0] if data else None

def read_unicode_string(addr):
    """Read a UNICODE_STRING struct {USHORT Length, USHORT MaxLen, padding, PWSTR Buffer}."""
    length = read_u16(addr)
    buf_ptr = read_u64(addr + 8)
    if length is None or buf_ptr is None or length == 0 or length > 512:
        return None
    data = read_mem(buf_ptr, length)
    if data is None:
        return None
    try:
        return data.decode('utf-16-le')
    except:
        return None

def read_i16(addr):
    data = read_mem(addr, 2)
    return struct.unpack('<h', data)[0] if data else None

def read_u8(addr):
    data = read_mem(addr, 1)
    return data[0] if data else None

# ============================================================
# PE helpers
# ============================================================

def find_pe_base_from_rip():
    """Walk backwards from current RIP to find the PE MZ header (page-aligned)."""
    try:
        rip = int(gdb.parse_and_eval("$rip"))
    except:
        return None
    if rip < KSEG0_BASE:
        return None
    page = rip & ~0xFFF
    for _ in range(4096):
        sig = read_u16(page)
        if sig == 0x5A4D:
            pe_off = read_u32(page + 0x3C)
            if pe_off and pe_off < 0x1000:
                pe_sig = read_u32(page + pe_off)
                if pe_sig == 0x00004550:
                    return page
        page -= 0x1000
    return None

def get_pe_all_sections(base):
    """Get all section names and RVAs from a loaded PE."""
    pe_off = read_u32(base + 0x3C)
    if not pe_off:
        return []
    num_sections = read_u16(base + pe_off + 6)
    size_opt = read_u16(base + pe_off + 20)
    if num_sections is None or size_opt is None:
        return []
    sec_start = base + pe_off + 24 + size_opt
    sections = []
    for i in range(num_sections):
        sec = sec_start + i * 40
        sec_name = read_mem(sec, 8)
        if sec_name is None:
            continue
        sec_name = sec_name.rstrip(b'\x00').decode('ascii', errors='replace')
        rva = read_u32(sec + 12)
        if rva:
            sections.append((sec_name, rva))
    return sections

def load_pe_symbols(base, name, filepath):
    """Load symbols for a PE at the given base, with all section offsets."""
    sections = get_pe_all_sections(base)
    text_rva = None
    extra_sections = []
    for sname, rva in sections:
        if sname == '.text':
            text_rva = rva
        elif not sname.startswith('.debug'):
            extra_sections.append((sname, rva))
    if text_rva is None:
        text_rva = 0x1000
    text_addr = base + text_rva
    section_args = ' '.join(f'-s {sname} 0x{base + rva:x}' for sname, rva in extra_sections)
    cmd = f'add-symbol-file {filepath} 0x{text_addr:x} {section_args}'
    try:
        gdb.execute(cmd, to_string=True)
        print(f"  Loaded {name} @ 0x{base:x} (.text=0x{text_addr:x})")
        return True
    except Exception as e:
        print(f"  Failed to load {name}: {e}")
        return False

# ============================================================
# Kernel-mode commands (only registered in long mode)
# ============================================================

if cpu_mode == "long":

    class ReactosLoadSymbols(gdb.Command):
        """Load ReactOS kernel symbols. Finds ntoskrnl base by scanning from RIP."""

        def __init__(self):
            super().__init__("ros-load-symbols", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            ntos_path = os.path.join(BUILD_DIR, 'ntoskrnl', 'ntoskrnl.exe')
            if not os.path.exists(ntos_path):
                print(f"ERROR: {ntos_path} not found. Set BUILD_DIR or run from repo root.")
                return

            base = None
            known_base = KSEG0_BASE + 0x400000
            sig = read_u16(known_base)
            if sig == 0x5A4D:
                pe_off = read_u32(known_base + 0x3C)
                if pe_off and pe_off < 0x1000:
                    pe_sig = read_u32(known_base + pe_off)
                    if pe_sig == 0x00004550:
                        base = known_base

            if base is None:
                print("  Known base not found, scanning from RIP...")
                base = find_pe_base_from_rip()

            if base is None:
                print("  RIP scan failed, brute-force scanning KSEG0_BASE...")
                for offset in range(0, 64 * 1024 * 1024, 0x1000):
                    addr = KSEG0_BASE + offset
                    sig = read_u16(addr)
                    if sig == 0x5A4D:
                        pe_off = read_u32(addr + 0x3C)
                        if pe_off and pe_off < 0x1000:
                            pe_sig = read_u32(addr + pe_off)
                            if pe_sig == 0x00004550:
                                base = addr
                                break

            if base:
                print(f"  Found ntoskrnl at 0x{base:x}")
                load_pe_symbols(base, "ntoskrnl.exe", ntos_path)
            else:
                print("  Could not find ntoskrnl. Try: ros-load-at <address>")

    ReactosLoadSymbols()


    class ReactosLoadAt(gdb.Command):
        """Manually load ntoskrnl symbols at a given base address.
        Usage: ros-load-at 0xFFFFF80000400000"""

        def __init__(self):
            super().__init__("ros-load-at", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-load-at <base-address>")
                return
            base = int(arg.strip(), 0)
            ntos_path = os.path.join(BUILD_DIR, 'ntoskrnl', 'ntoskrnl.exe')
            if not os.path.exists(ntos_path):
                print(f"ERROR: {ntos_path} not found")
                return
            load_pe_symbols(base, "ntoskrnl.exe", ntos_path)

    ReactosLoadAt()


    class ReactosLoadModule(gdb.Command):
        """Load symbols for a specific module.
        Usage: ros-load-module <base-address> <build-relative-path>
        Example: ros-load-module 0xFFFFF88001234000 drivers/filesystems/ntfs/ntfs.sys"""

        def __init__(self):
            super().__init__("ros-load-module", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            parts = arg.strip().split(None, 1)
            if len(parts) < 2:
                print("Usage: ros-load-module <base-addr> <build/relative/path>")
                return
            base = int(parts[0], 0)
            relpath = parts[1]
            filepath = os.path.join(BUILD_DIR, relpath)
            if not os.path.exists(filepath):
                print(f"ERROR: {filepath} not found")
                return
            name = os.path.basename(relpath)
            load_pe_symbols(base, name, filepath)

    ReactosLoadModule()


    class ReactosModuleList(gdb.Command):
        """Walk PsLoadedModuleList and display all loaded kernel modules.
        Usage: ros-lsmod [--load]"""

        def __init__(self):
            super().__init__("ros-lsmod", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            do_load = '--load' in arg

            try:
                list_head = int(gdb.parse_and_eval("(unsigned long long)&PsLoadedModuleList"))
            except:
                print("PsLoadedModuleList not found. Load ntoskrnl symbols first (ros-load-symbols).")
                return

            flink = read_u64(list_head)
            if flink is None:
                print("Cannot read PsLoadedModuleList")
                return

            MODULE_PATHS = {
                'ntoskrnl.exe': 'ntoskrnl/ntoskrnl.exe',
                'hal.dll':      'hal/halx86/hal.dll',
                'kdcom.dll':    'drivers/base/kdcom/kdcom.dll',
                'bootvid.dll':  'drivers/base/bootvid/bootvid.dll',
                'ntfs.sys':     'drivers/filesystems/ntfs/ntfs.sys',
                'win32k.sys':   'win32ss/win32k.sys',
                'scsiport.sys': 'drivers/storage/port/scsiport/scsiport.sys',
            }

            print(f"{'Base':>20s}  {'Size':>10s}  Name")
            print(f"{'----':>20s}  {'----':>10s}  ----")

            entry_addr = flink
            count = 0
            while entry_addr != list_head and count < 256:
                dll_base = read_u64(entry_addr + 0x30)
                size = read_u32(entry_addr + 0x40)
                name = read_unicode_string(entry_addr + 0x58)

                if dll_base is None:
                    break

                name_str = name if name else "<unknown>"
                size_str = f"0x{size:x}" if size else "?"
                print(f"  0x{dll_base:016x}  {size_str:>10s}  {name_str}")

                if do_load and name:
                    lname = name.lower()
                    if lname in MODULE_PATHS:
                        fpath = os.path.join(BUILD_DIR, MODULE_PATHS[lname])
                        if os.path.exists(fpath):
                            load_pe_symbols(dll_base, name, fpath)

                entry_addr = read_u64(entry_addr)
                if entry_addr is None:
                    break
                count += 1

            print(f"\n{count} modules loaded")
            if not do_load:
                print("Tip: use 'ros-lsmod --load' to auto-load symbols for known modules")

    ReactosModuleList()


    class ReactosFindModule(gdb.Command):
        """Find which module an address belongs to.
        Usage: ros-addr2mod <address>"""

        def __init__(self):
            super().__init__("ros-addr2mod", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-addr2mod <address>")
                return

            target = int(arg.strip(), 0)

            try:
                list_head = int(gdb.parse_and_eval("(unsigned long long)&PsLoadedModuleList"))
            except:
                print("PsLoadedModuleList not found. Load ntoskrnl symbols first.")
                return

            flink = read_u64(list_head)
            if flink is None:
                return

            entry_addr = flink
            count = 0
            while entry_addr != list_head and count < 256:
                dll_base = read_u64(entry_addr + 0x30)
                size = read_u32(entry_addr + 0x40)
                name = read_unicode_string(entry_addr + 0x58)

                if dll_base and size and dll_base <= target < dll_base + size:
                    offset = target - dll_base
                    name_str = name if name else "<unknown>"
                    print(f"0x{target:x} => {name_str} + 0x{offset:x} (base=0x{dll_base:x})")
                    return

                entry_addr = read_u64(entry_addr)
                if entry_addr is None:
                    break
                count += 1

            print(f"0x{target:x} not found in any loaded module")

    ReactosFindModule()


    # KTHREAD field offsets for ReactOS amd64
    KTHREAD_INITIAL_STACK = 0x28
    KTHREAD_KERNEL_STACK  = 0x38
    KTHREAD_APC_STATE     = 0x48
    KTHREAD_KERNEL_APC_DISABLE = 0x1B4
    KTHREAD_SPECIAL_APC_DISABLE = 0x1B2

    KAPC_STATE_KERNEL_LIST   = 0x00
    KAPC_STATE_USER_LIST     = 0x10
    KAPC_STATE_PROCESS       = 0x20
    KAPC_STATE_IN_PROGRESS   = 0x28
    KAPC_STATE_KAPC_PENDING  = 0x29
    KAPC_STATE_UAPC_PENDING  = 0x2A

    KAPC_APC_LIST_ENTRY    = 0x10
    KAPC_KERNEL_ROUTINE    = 0x20
    KAPC_NORMAL_ROUTINE    = 0x30

    KWAIT_BLOCK_THREAD     = 0x10
    KWAIT_BLOCK_OBJECT     = 0x18

    def addr_to_sym(addr):
        """Resolve address to symbol string, falling back to module+offset."""
        try:
            sym = gdb.execute(f"info symbol 0x{addr:x}", to_string=True).strip()
            if "No symbol" not in sym:
                parts = sym.split(" in section ")
                return parts[0]
        except:
            pass
        return None

    def dump_thread_stack(thread_addr):
        """Dump the kernel stack of a thread with symbol resolution."""
        init_stack = read_u64(thread_addr + KTHREAD_INITIAL_STACK)
        kern_stack = read_u64(thread_addr + KTHREAD_KERNEL_STACK)

        if not init_stack or not kern_stack:
            print(f"  Cannot read stack pointers")
            return

        print(f"  KernelStack  = 0x{kern_stack:x}")
        print(f"  InitialStack = 0x{init_stack:x}")
        print(f"  Stack usage  = {init_stack - kern_stack} bytes\n")

        for off in range(0, min(init_stack - kern_stack, 0x1000), 8):
            addr = kern_stack + off
            val = read_u64(addr)
            if val is None:
                continue
            if val >= 0xFFFFF80000000000 and val < 0xFFFFFFFFFFC00000:
                sym = addr_to_sym(val)
                if sym:
                    print(f"  [0x{addr:x}] 0x{val:x}  {sym}")


    class ReactosThreadInfo(gdb.Command):
        """Inspect a kernel thread: stack trace, APC state, wait info.
        Usage: ros-thread <thread-address>"""

        def __init__(self):
            super().__init__("ros-thread", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-thread <kthread-address>")
                return

            thread = int(arg.strip(), 0)
            print(f"=== KTHREAD 0x{thread:x} ===\n")

            init_stack = read_u64(thread + KTHREAD_INITIAL_STACK)
            kern_stack = read_u64(thread + KTHREAD_KERNEL_STACK)
            print(f"InitialStack = 0x{init_stack:x}" if init_stack else "InitialStack = ?")
            print(f"KernelStack  = 0x{kern_stack:x}" if kern_stack else "KernelStack  = ?")

            apc_base = thread + KTHREAD_APC_STATE
            k_list_head = apc_base + KAPC_STATE_KERNEL_LIST
            k_flink = read_u64(k_list_head)
            process = read_u64(apc_base + KAPC_STATE_PROCESS)
            in_progress = read_u8(apc_base + KAPC_STATE_IN_PROGRESS)
            kapc_pending = read_u8(apc_base + KAPC_STATE_KAPC_PENDING)
            uapc_pending = read_u8(apc_base + KAPC_STATE_UAPC_PENDING)

            kapc_disable = read_i16(thread + KTHREAD_KERNEL_APC_DISABLE)
            special_disable = read_i16(thread + KTHREAD_SPECIAL_APC_DISABLE)

            print(f"\nAPC State:")
            print(f"  KernelApcDisable  = {kapc_disable}")
            print(f"  SpecialApcDisable = {special_disable}")
            print(f"  KernelApcPending  = {kapc_pending}")
            print(f"  UserApcPending    = {uapc_pending}")
            print(f"  InProgressFlags   = {in_progress}")
            print(f"  Process           = 0x{process:x}" if process else "  Process = ?")

            if k_flink and k_flink != k_list_head:
                print(f"\n  Queued Kernel APCs:")
                entry = k_flink
                count = 0
                while entry != k_list_head and count < 16:
                    kapc = entry - KAPC_APC_LIST_ENTRY
                    kern_routine = read_u64(kapc + KAPC_KERNEL_ROUTINE)
                    norm_routine = read_u64(kapc + KAPC_NORMAL_ROUTINE)
                    ksym = addr_to_sym(kern_routine) if kern_routine else None
                    nsym = addr_to_sym(norm_routine) if norm_routine else None
                    print(f"    KAPC 0x{kapc:x}:")
                    print(f"      KernelRoutine = 0x{kern_routine:x} ({ksym})" if ksym else f"      KernelRoutine = 0x{kern_routine:x}" if kern_routine else "      KernelRoutine = NULL")
                    if norm_routine:
                        print(f"      NormalRoutine = 0x{norm_routine:x} ({nsym})" if nsym else f"      NormalRoutine = 0x{norm_routine:x}")
                    entry = read_u64(entry)
                    if entry is None:
                        break
                    count += 1
            else:
                print(f"\n  Kernel APC queue: empty")

            print(f"\nStack trace:")
            dump_thread_stack(thread)

    ReactosThreadInfo()


    class ReactosFindWaiters(gdb.Command):
        """Find threads waiting on a dispatcher object (Event, Mutex, etc).
        Usage: ros-waiters <event-address>"""

        def __init__(self):
            super().__init__("ros-waiters", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-waiters <event-address>")
                return

            event = int(arg.strip(), 0)
            print(f"=== Dispatcher object at 0x{event:x} ===\n")

            hdr = read_mem(event, 4)
            if not hdr:
                print("Cannot read object header")
                return

            obj_type = hdr[0]
            signal_state = read_u32(event + 4)
            wait_list_head = event + 8
            flink = read_u64(wait_list_head)

            type_names = {0: "NotificationEvent", 1: "SynchronizationEvent",
                          2: "Mutant", 3: "Process", 5: "Thread",
                          8: "NotificationTimer", 9: "SynchronizationTimer",
                          29: "Semaphore"}
            type_str = type_names.get(obj_type, f"Unknown({obj_type})")

            print(f"Type        = {type_str}")
            print(f"SignalState = {signal_state}")

            if not flink or flink == wait_list_head:
                print("WaitList    = empty (no waiters)")
                return

            print(f"\nWaiters:")
            entry = flink
            count = 0
            while entry != wait_list_head and count < 32:
                thread_ptr = read_u64(entry + KWAIT_BLOCK_THREAD)
                object_ptr = read_u64(entry + KWAIT_BLOCK_OBJECT)
                wait_key = read_u16(entry + 0x28)

                print(f"  WaitBlock 0x{entry:x}:")
                print(f"    Thread  = 0x{thread_ptr:x}" if thread_ptr else "    Thread = NULL")
                print(f"    Object  = 0x{object_ptr:x}" if object_ptr else "    Object = NULL")
                if wait_key is not None:
                    print(f"    WaitKey = {wait_key}")

                if thread_ptr:
                    kapc_disable = read_i16(thread_ptr + KTHREAD_KERNEL_APC_DISABLE)
                    kapc_pending = read_u8(thread_ptr + KTHREAD_APC_STATE + KAPC_STATE_KAPC_PENDING)
                    print(f"    KernelApcDisable={kapc_disable} KernelApcPending={kapc_pending}")

                entry = read_u64(entry)
                if entry is None:
                    break
                count += 1

            print(f"\n{count} waiter(s) found")

    ReactosFindWaiters()


    class ReactosIrpInfo(gdb.Command):
        """Inspect an IRP structure.
        Usage: ros-irp <irp-address>"""

        def __init__(self):
            super().__init__("ros-irp", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-irp <irp-address>")
                return

            irp = int(arg.strip(), 0)
            print(f"=== IRP 0x{irp:x} ===\n")

            irp_type = read_u16(irp + 0x00)
            irp_size = read_u16(irp + 0x02)
            mdl      = read_u64(irp + 0x08)
            flags    = read_u32(irp + 0x10)
            sys_buf  = read_u64(irp + 0x18)
            io_stat  = read_u32(irp + 0x30)
            user_iosb = read_u64(irp + 0x48)
            user_event = read_u64(irp + 0x50)
            user_buffer = read_u64(irp + 0x68)
            thread   = read_u64(irp + 0x78)

            flag_names = []
            if flags:
                if flags & 0x01: flag_names.append("NOCACHE")
                if flags & 0x02: flag_names.append("PAGING_IO")
                if flags & 0x08: flag_names.append("SYNCHRONOUS_API")
                if flags & 0x10: flag_names.append("INPUT_OPERATION")
                if flags & 0x100: flag_names.append("READ_OPERATION")
                if flags & 0x200: flag_names.append("WRITE_OPERATION")

            type_ok = "OK" if irp_type == 0x06 else f"BAD(0x{irp_type:x})"
            flags_str = ' | '.join(flag_names) if flag_names else "0x0"

            print(f"Type        = 0x{irp_type:x} ({type_ok})")
            print(f"Size        = 0x{irp_size:x}")
            print(f"Flags       = 0x{flags:x} ({flags_str})")
            print(f"MdlAddress  = 0x{mdl:x}" if mdl else "MdlAddress  = NULL")
            print(f"SystemBuf   = 0x{sys_buf:x}" if sys_buf else "SystemBuf   = NULL")
            print(f"IoStatus    = 0x{io_stat:x}" if io_stat is not None else "IoStatus    = ?")
            print(f"UserIosb    = 0x{user_iosb:x}" if user_iosb else "UserIosb    = NULL")
            print(f"UserEvent   = 0x{user_event:x}" if user_event else "UserEvent   = NULL")
            print(f"UserBuffer  = 0x{user_buffer:x}" if user_buffer else "UserBuffer  = NULL")
            print(f"Thread      = 0x{thread:x}" if thread else "Thread      = NULL")

            if user_event:
                sig = read_u32(user_event + 4)
                print(f"\nUserEvent SignalState = {sig}")

    ReactosIrpInfo()


    # ============================================================
    # KTRAP_FRAME layout for ReactOS amd64
    # Source: sdk/include/ndk/amd64/ketypes.h
    # Built offsets: build/sdk/include/asm/ksamd64.inc
    # Total size: 0x190 (400 bytes)
    #
    # CAUTION: These offsets are for the ReactOS amd64 KTRAP_FRAME.
    # If they don't match your build, check ketypes.h or ksamd64.inc.
    # The non-volatile registers (Rbx, Rdi, Rsi, Rbp at 0x140-0x158)
    # are NOT saved by the CPU or EnterTrap — they come from
    # KeTrapFrameToContext merging with KEXCEPTION_FRAME. When
    # reading a raw trap frame on the stack, these fields may be
    # stale or zero. Recover non-volatiles from the function's own
    # stack frame (push rbx etc.) instead.
    # ============================================================
    TF_RAX           = 0x030
    TF_RCX           = 0x038
    TF_RDX           = 0x040
    TF_R8            = 0x048
    TF_R9            = 0x050
    TF_R10           = 0x058
    TF_R11           = 0x060
    TF_GSBASE        = 0x068
    TF_FAULT_ADDR    = 0x0D0  # CR2 on page fault (union with ContextRecord)
    TF_DR0           = 0x0D8
    TF_SEG_DS        = 0x130
    TF_SEG_ES        = 0x132
    TF_SEG_FS        = 0x134
    TF_SEG_GS        = 0x136
    TF_TRAP_LINK     = 0x138  # Pointer to previous KTRAP_FRAME (if nested)
    TF_RBX           = 0x140  # WARNING: see note above — may not reflect fault-time value
    TF_RDI           = 0x148
    TF_RSI           = 0x150
    TF_RBP           = 0x158
    TF_ERROR_CODE    = 0x160  # Union with ExceptionFrame pointer
    TF_RIP           = 0x168
    TF_SEG_CS        = 0x170
    TF_EFLAGS        = 0x178
    TF_RSP           = 0x180
    TF_SEG_SS        = 0x188
    TF_SIZE          = 0x190
    TF_PREV_IRQL     = 0x029  # PreviousIrql (UCHAR)
    TF_PREV_MODE     = 0x028  # PreviousMode (CHAR)

    # KEXCEPTION_FRAME layout (sdk/include/ndk/amd64/ketypes.h)
    # Size: 0x140 (320 bytes). Saves non-volatile registers.
    EF_RBP           = 0x0F8
    EF_RBX           = 0x100
    EF_RDI           = 0x108
    EF_RSI           = 0x110
    EF_R12           = 0x118
    EF_R13           = 0x120
    EF_R14           = 0x128
    EF_R15           = 0x130
    EF_RETURN        = 0x138
    EF_TRAP_FRAME    = 0x0D0


    class ReactosFindTrapFrames(gdb.Command):
        """Scan a kernel stack range for KTRAP_FRAMEs.
        Heuristic: look for SegCs == 0x10 (kernel) or 0x33 (user)
        and a valid kernel-space Rip at the expected offset.

        Usage: ros-trapframes [stack_low] [stack_high]
        Default: scans from RSP to RSP + 0x2000.

        KNOWN LIMITATIONS:
        - False positives if random stack data matches SegCs pattern.
        - The Rbx/Rdi/Rsi/Rbp fields in the trap frame may NOT reflect
          the actual register values at fault time (see notes above).
          Use ros-frame-regs to recover pushed non-volatiles from the
          function's own prologue.
        - If the kernel was built with a different KTRAP_FRAME layout
          (different ReactOS branch or Windows), offsets will be wrong.
        """

        def __init__(self):
            super().__init__("ros-trapframes", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            args = arg.strip().split()
            if len(args) >= 2:
                low = int(args[0], 0)
                high = int(args[1], 0)
            elif len(args) == 1:
                low = int(args[0], 0)
                high = low + 0x2000
            else:
                try:
                    low = int(gdb.parse_and_eval("$rsp"))
                except:
                    print("Cannot read RSP. Provide stack range explicitly.")
                    return
                high = low + 0x4000

            found = 0
            for pos in range(low, high - TF_SIZE, 8):
                cs_data = read_mem(pos + TF_SEG_CS, 2)
                if cs_data is None:
                    continue
                seg_cs = struct.unpack('<H', cs_data)[0]
                if seg_cs not in (0x10, 0x33):
                    continue

                rip = read_u64(pos + TF_RIP)
                if rip is None:
                    continue
                # Kernel RIP heuristic: must be in KSEG0 range
                if not (0xFFFFF80000000000 <= rip <= 0xFFFFFFFFFFC00000):
                    continue

                rsp = read_u64(pos + TF_RSP) or 0
                fault_addr = read_u64(pos + TF_FAULT_ADDR) or 0
                prev_irql = read_u8(pos + TF_PREV_IRQL)
                prev_irql = prev_irql if prev_irql is not None else -1
                error_code = read_u64(pos + TF_ERROR_CODE) or 0
                rax = read_u64(pos + TF_RAX) or 0
                rcx = read_u64(pos + TF_RCX) or 0
                trap_link = read_u64(pos + TF_TRAP_LINK) or 0

                # Try to resolve RIP to a symbol
                sym = addr_to_sym(rip)
                sym_str = f" ({sym})" if sym else ""

                found += 1
                print(f"KTRAP_FRAME @ 0x{pos:x}:")
                print(f"  Rip       = 0x{rip:x}{sym_str}")
                print(f"  Rsp       = 0x{rsp:x}")
                print(f"  FaultAddr = 0x{fault_addr:x}")
                print(f"  ErrorCode = 0x{error_code:x}")
                print(f"  PrevIRQL  = {prev_irql}")
                print(f"  Rax=0x{rax:x}  Rcx=0x{rcx:x}")
                print(f"  TrapLink  = 0x{trap_link:x}")
                # Warn about non-volatile registers
                print(f"  (Rbx/Rdi/Rsi/Rbp in TF may be stale; use ros-frame-regs)")
                print()

            if found == 0:
                print("No KTRAP_FRAMEs found in range.")
            else:
                print(f"{found} trap frame(s) found.")

    ReactosFindTrapFrames()


    class ReactosFrameRegs(gdb.Command):
        """Recover pushed non-volatile registers from a function's stack frame.
        Given the RSP at fault time (from KTRAP_FRAME.Rsp) and a typical
        amd64 prologue pattern (push r14; push rdi; push rsi; push rbx; sub $N,rsp),
        reads the saved register values from the stack.

        Usage: ros-frame-regs <rsp_at_fault> [prologue_pattern]
        Prologue patterns (predefined):
          cc-release   : CcRosReleaseFileCache (push r14,rdi,rsi,rbx; sub $0x38)
          cc-uninit    : CcUninitializeCacheMap (push r14,rbp,rdi,rsi,rbx; sub $0x20)
          ntfs-release : NtfsReleaseFCB (push rdi,rsi,rbx; sub $0x20)

        Or specify raw: "r14,rdi,rsi,rbx:0x38" meaning those regs pushed then sub $0x38.

        NOTE: These patterns are based on GCC 15 output for ReactOS amd64.
        Different compiler versions or optimization levels WILL produce different
        prologues. Always verify with 'x/10i <function_start>'.
        """

        PATTERNS = {
            'cc-release':   (['r14', 'rdi', 'rsi', 'rbx'], 0x38),
            'cc-uninit':    (['r14', 'rbp', 'rdi', 'rsi', 'rbx'], 0x20),
            'ntfs-release': (['rdi', 'rsi', 'rbx'], 0x20),
        }

        def __init__(self):
            super().__init__("ros-frame-regs", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            args = arg.strip().split()
            if len(args) < 1:
                print("Usage: ros-frame-regs <rsp_at_fault> [pattern_name|reg,reg:sub_size]")
                print("Patterns:", ', '.join(self.PATTERNS.keys()))
                return

            rsp = int(args[0], 0)
            if len(args) >= 2:
                pat_name = args[1]
                if pat_name in self.PATTERNS:
                    regs, sub_size = self.PATTERNS[pat_name]
                elif ':' in pat_name:
                    parts = pat_name.split(':')
                    regs = parts[0].split(',')
                    sub_size = int(parts[1], 0)
                else:
                    print(f"Unknown pattern '{pat_name}'")
                    return
            else:
                regs, sub_size = ['rbx'], 0x20  # minimal guess

            # Stack layout: sub $N lowered RSP, then pushes are above
            # At fault time RSP is after sub. Pushes are at RSP + sub_size upward.
            base = rsp + sub_size
            print(f"Stack frame (sub $0x{sub_size:x}, {len(regs)} pushes):")
            for i, reg in enumerate(reversed(regs)):
                addr = base + i * 8
                val = read_u64(addr)
                val_str = f"0x{val:x}" if val is not None else "?"
                sym = addr_to_sym(val) if val and val > 0xFFFFF80000000000 else None
                sym_str = f" ({sym})" if sym else ""
                print(f"  saved_{reg:4s} @ 0x{addr:x} = {val_str}{sym_str}")

            # Return address is above all pushes
            ret_addr_loc = base + len(regs) * 8
            ret_val = read_u64(ret_addr_loc)
            if ret_val:
                sym = addr_to_sym(ret_val)
                sym_str = f" ({sym})" if sym else ""
                print(f"  return     @ 0x{ret_addr_loc:x} = 0x{ret_val:x}{sym_str}")

    ReactosFrameRegs()


    class ReactosVerifyBinary(gdb.Command):
        """Compare a loaded module's .text section against the build on disk.
        Detects stale installations where the qcow2 has an older driver
        than the current build directory.

        Usage: ros-verify <module_base> <build_path>
        Example: ros-verify 0xFFFFF8807505F000 drivers/filesystems/ntfs/ntfs.sys

        Reads .text section from both the QEMU memory and the build file,
        computes SHA256, and reports match/mismatch.

        KNOWN ISSUE: ReactOS relocates PE images, so .reloc-patched bytes
        will always differ. This command hashes .text which has fewer
        relocations, but mismatches can still be caused by relocation
        fixups rather than different builds. For definitive comparison,
        check a specific function's disassembly.
        """

        def __init__(self):
            super().__init__("ros-verify", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            import hashlib

            args = arg.strip().split(None, 1)
            if len(args) < 2:
                print("Usage: ros-verify <module_base> <build_path>")
                return

            base = int(args[0], 0)
            rel_path = args[1]
            filepath = os.path.join(BUILD_DIR, rel_path) if not os.path.isabs(rel_path) else rel_path
            if not os.path.exists(filepath):
                print(f"ERROR: {filepath} not found")
                return

            # Read .text section info from loaded PE
            sections = get_pe_all_sections(base)
            text_rva = None
            text_size = None
            for sname, rva in sections:
                if sname == '.text':
                    text_rva = rva
                    break

            if text_rva is None:
                print("ERROR: No .text section found in loaded PE")
                return

            # Get .text size from PE section header
            pe_off = read_u32(base + 0x3C)
            num_sections = read_u16(base + pe_off + 6)
            size_opt = read_u16(base + pe_off + 20)
            sec_start = base + pe_off + 24 + size_opt
            for i in range(num_sections):
                sec = sec_start + i * 40
                sec_name = read_mem(sec, 8)
                if sec_name and sec_name.rstrip(b'\x00') == b'.text':
                    text_size = read_u32(sec + 8)  # VirtualSize
                    break

            if text_size is None or text_size == 0:
                print("ERROR: Cannot determine .text size")
                return

            print(f"Comparing .text (RVA=0x{text_rva:x}, size=0x{text_size:x})...")

            # Hash loaded .text from memory
            loaded_data = bytearray()
            chunk = 4096
            for off in range(0, text_size, chunk):
                sz = min(chunk, text_size - off)
                d = read_mem(base + text_rva + off, sz)
                if d is None:
                    print(f"  ERROR: Memory read failed at base+0x{text_rva + off:x}")
                    return
                loaded_data.extend(d)

            loaded_hash = hashlib.sha256(bytes(loaded_data)).hexdigest()

            # Hash .text from build file
            # Need to find .text file offset in the PE on disk
            with open(filepath, 'rb') as f:
                pe_data = f.read(0x400)  # read headers
                disk_pe_off = struct.unpack_from('<I', pe_data, 0x3C)[0]
                disk_num_sec = struct.unpack_from('<H', pe_data, disk_pe_off + 6)[0]
                disk_opt_size = struct.unpack_from('<H', pe_data, disk_pe_off + 20)[0]
                disk_sec_start = disk_pe_off + 24 + disk_opt_size

                for i in range(disk_num_sec):
                    sec_off = disk_sec_start + i * 40
                    if sec_off + 40 > len(pe_data):
                        f.seek(0)
                        pe_data = f.read(sec_off + 40)
                    sec_name = pe_data[sec_off:sec_off + 8].rstrip(b'\x00')
                    if sec_name == b'.text':
                        disk_vsize = struct.unpack_from('<I', pe_data, sec_off + 8)[0]
                        disk_raw_off = struct.unpack_from('<I', pe_data, sec_off + 20)[0]
                        f.seek(disk_raw_off)
                        disk_text = f.read(min(disk_vsize, text_size))
                        break
                else:
                    print("  ERROR: .text not found in build file")
                    return

            build_hash = hashlib.sha256(disk_text).hexdigest()

            if loaded_hash == build_hash:
                print(f"  MATCH: {loaded_hash[:16]}...")
                print(f"  The loaded binary matches the build.")
            else:
                print(f"  MISMATCH!")
                print(f"  Loaded: {loaded_hash[:32]}...")
                print(f"  Build:  {build_hash[:32]}...")
                print(f"  The qcow2 has a DIFFERENT binary than the current build.")
                print(f"  Reinstall from the latest bootcd.iso to test current code.")

    ReactosVerifyBinary()


    class ReactosCallChain(gdb.Command):
        """Walk kernel stack looking for return addresses and resolve them.
        Scans the stack for values that look like kernel code pointers
        (in KSEG0 or driver space) and resolves them to symbols.

        Usage: ros-callchain [rsp] [depth]
        Default: from current RSP, scan 0x400 bytes (64 qwords).

        KNOWN LIMITATIONS:
        - This is a heuristic scan, NOT a proper frame-based unwind.
          It will show false positives (data values that happen to look
          like code addresses) and may miss frames with unusual layouts.
        - For accurate unwinding, use ros-trapframes to find exception
          frames, then ros-frame-regs to decode each function's frame.
        - Driver addresses (0xFFFFF880...) won't resolve to symbols
          unless you've loaded them with ros-lsmod --load or ros-load-module.
        """

        def __init__(self):
            super().__init__("ros-callchain", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            args = arg.strip().split()
            if len(args) >= 1:
                rsp = int(args[0], 0)
            else:
                try:
                    rsp = int(gdb.parse_and_eval("$rsp"))
                except:
                    print("Cannot read RSP.")
                    return
            depth = int(args[1], 0) if len(args) >= 2 else 0x400

            print(f"Scanning 0x{rsp:x} - 0x{rsp + depth:x} for code pointers:")
            for off in range(0, depth, 8):
                addr = rsp + off
                val = read_u64(addr)
                if val is None:
                    continue
                # Check if it looks like a kernel code address
                if 0xFFFFF80000400000 <= val <= 0xFFFFF800006FFFFF:
                    # ntoskrnl range (approximate)
                    sym = addr_to_sym(val)
                    sym_str = sym if sym else "ntoskrnl+?"
                    print(f"  [RSP+0x{off:03x}] 0x{val:x}  {sym_str}")
                elif 0xFFFFF88070000000 <= val <= 0xFFFFF880FFFFFFFF:
                    # Driver range
                    sym = addr_to_sym(val)
                    sym_str = sym if sym else "driver+?"
                    print(f"  [RSP+0x{off:03x}] 0x{val:x}  {sym_str}")

    ReactosCallChain()


    class ReactosFindModule(gdb.Command):
        """Find a loaded module by scanning for MZ/PE headers near an address.
        Walks backwards from the given address (page-aligned) looking for
        an MZ header followed by a valid PE signature.

        Usage: ros-findmod <address_in_module>
        Returns the base address of the module.

        Example: ros-findmod 0xFFFFF88075069565
        """

        def __init__(self):
            super().__init__("ros-findmod", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-findmod <address>")
                return
            target = int(arg.strip(), 0)
            page = target & ~0xFFF
            for i in range(4096):
                sig = read_u16(page)
                if sig == 0x5A4D:
                    pe_off = read_u32(page + 0x3C)
                    if pe_off and pe_off < 0x1000:
                        pe_sig = read_u32(page + pe_off)
                        if pe_sig == 0x00004550:
                            print(f"Module base: 0x{page:x}")
                            # Try to read export name
                            opt_start = page + pe_off + 24
                            export_rva = read_u32(opt_start + 0x70)
                            if export_rva and export_rva > 0:
                                name_rva = read_u32(page + export_rva + 12)
                                if name_rva:
                                    name_data = read_mem(page + name_rva, 64)
                                    if name_data:
                                        name = name_data.split(b'\x00')[0].decode('ascii', errors='replace')
                                        print(f"Export name: {name}")
                            sections = get_pe_all_sections(page)
                            for sname, rva in sections:
                                print(f"  {sname:8s}  @ 0x{page + rva:x} (RVA 0x{rva:x})")
                            return
                page -= 0x1000
            print(f"No MZ/PE found scanning back from 0x{target:x}")

    ReactosFindModule()


    # ============================================================
    # Pool debugging helpers
    # ============================================================

    # Pool header layout on amd64 (POOL_BLOCK_SIZE = 16)
    POOL_BLOCK_SIZE = 16

    def pool_tag_str(raw_bytes):
        """Format 4 pool tag bytes as a printable string."""
        return ''.join(chr(b) if 32 <= b < 127 else '.' for b in raw_bytes)

    def parse_pool_header(addr):
        """Read and parse a POOL_HEADER at addr. Returns dict or None."""
        data = read_mem(addr, POOL_BLOCK_SIZE)
        if data is None:
            return None
        prev_size = data[0]
        pool_index = data[1]
        block_size = data[2]
        pool_type = data[3]
        tag_bytes = data[4:8]
        tag = pool_tag_str(tag_bytes)
        return {
            'addr': addr,
            'prev_size': prev_size,
            'pool_index': pool_index,
            'block_size': block_size,
            'pool_type': pool_type,
            'tag': tag,
            'tag_bytes': tag_bytes,
            'actual_size': block_size * POOL_BLOCK_SIZE,
            'free': pool_type == 0,
        }

    def is_valid_pool_tag(tag_bytes):
        """Check if 4 bytes look like a valid pool tag.
        Pool tags are typically 4 printable ASCII chars (or spaces).
        Free blocks may have null tags. Tags with high bytes set are suspicious."""
        for b in tag_bytes:
            if b == 0:
                continue  # null bytes OK (free blocks)
            if b < 0x20 or b > 0x7e:
                return False  # non-printable = not a pool tag
        return True

    def is_pool_page(page_addr):
        """Heuristic: check if a page looks like a small-block pool page.
        Real pool pages have: PrevSize=0 for first block, valid tags,
        and the block chain fills exactly one page (0x1000 bytes)."""
        data = read_mem(page_addr, 0x1000)
        if data is None:
            return False
        # First block: PreviousSize must be 0
        if data[0] != 0:
            return False
        # BlockSize must be non-zero
        if data[2] == 0:
            return False
        # PoolIndex should be small (< 16 for standard pool descriptors)
        if data[1] > 16:
            return False
        # Tag should be valid ASCII
        if not is_valid_pool_tag(data[4:8]):
            return False
        # Walk a few blocks and verify consistency
        pos = 0
        prev_bs = 0
        for idx in range(5):  # Check first 5 blocks
            if pos >= 0x1000:
                break
            bs = data[pos + 2]
            if bs == 0:
                return pos == 0  # zero at start = not pool
            if idx > 0 and data[pos] != prev_bs:
                return False  # PrevSize mismatch in first 5 = not pool
            if not is_valid_pool_tag(data[pos + 4:pos + 8]):
                return False  # bad tag = not pool
            prev_bs = bs
            pos += bs * POOL_BLOCK_SIZE
        return True

    def walk_pool_page(page_addr):
        """Walk all pool blocks on a page. Returns list of header dicts + errors."""
        blocks = []
        errors = []
        pos = 0
        prev_bs = 0
        idx = 0
        while pos < 0x1000:
            hdr = parse_pool_header(page_addr + pos)
            if hdr is None:
                errors.append(f"Cannot read header at +0x{pos:03x}")
                break
            hdr['offset'] = pos
            hdr['index'] = idx

            # Validate PreviousSize
            if idx > 0 and hdr['prev_size'] != prev_bs:
                errors.append(
                    f"Block #{idx} at +0x{pos:03x}: PreviousSize={hdr['prev_size']} "
                    f"but previous block's BlockSize={prev_bs}")
            elif idx == 0 and hdr['prev_size'] != 0:
                errors.append(
                    f"Block #0 at +0x{pos:03x}: PreviousSize={hdr['prev_size']} != 0 "
                    f"(first block on page must be 0)")

            # Validate BlockSize
            if hdr['block_size'] == 0:
                errors.append(f"Block #{idx} at +0x{pos:03x}: BlockSize=0 (corrupted)")
                blocks.append(hdr)
                break

            blocks.append(hdr)
            prev_bs = hdr['block_size']
            pos += hdr['actual_size']
            idx += 1

            if idx > 256:
                errors.append("Too many blocks (>256), likely corruption")
                break

        # Validate that blocks fill the page exactly
        if pos != 0x1000 and not errors:
            errors.append(f"Blocks end at +0x{pos:03x}, expected +0x1000")

        return blocks, errors


    class ReactosPoolPage(gdb.Command):
        """Walk and validate all pool blocks on a page.
        Usage: ros-pool-page <address>
        Address can be any pointer within the page; it will be page-aligned.

        Shows each block's header, tag, size, and flags any inconsistencies
        (PreviousSize mismatch, zero BlockSize, blocks not filling page).
        """

        def __init__(self):
            super().__init__("ros-pool-page", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-pool-page <address>")
                return
            addr = int(arg.strip(), 0)
            page = addr & ~0xFFF
            print(f"=== Pool page 0x{page:x} ===\n")

            blocks, errors = walk_pool_page(page)

            # Print blocks
            for b in blocks:
                status = "FREE" if b['free'] else f"pt={b['pool_type']}"
                marker = ""
                if page + b['offset'] <= addr < page + b['offset'] + b['actual_size']:
                    marker = "  <<<"
                print(f"  #{b['index']:3d}  +0x{b['offset']:03x}  "
                      f"[{b['tag']:4s}]  bs={b['block_size']:3d} (0x{b['actual_size']:03x})  "
                      f"ps={b['prev_size']:3d}  {status}{marker}")

            # Print errors
            if errors:
                print(f"\n*** {len(errors)} error(s):")
                for e in errors:
                    print(f"  {e}")
            else:
                print(f"\n{len(blocks)} blocks, no errors.")

    ReactosPoolPage()


    class ReactosPoolBlock(gdb.Command):
        """Inspect a single pool block at the given allocation address.
        Usage: ros-pool-block <address>
        Address should point to the user data (after the POOL_HEADER).
        The header is at address - 0x10 (POOL_BLOCK_SIZE on amd64).

        Shows the pool header fields and a hex dump of the block data.
        """

        def __init__(self):
            super().__init__("ros-pool-block", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            if not arg.strip():
                print("Usage: ros-pool-block <address>")
                return
            data_addr = int(arg.strip(), 0)
            hdr_addr = data_addr - POOL_BLOCK_SIZE
            hdr = parse_pool_header(hdr_addr)
            if hdr is None:
                print(f"Cannot read pool header at 0x{hdr_addr:x}")
                return

            print(f"=== Pool block at 0x{data_addr:x} ===")
            print(f"  Header     @ 0x{hdr_addr:x}")
            print(f"  Tag        = '{hdr['tag']}'")
            print(f"  BlockSize  = {hdr['block_size']} (0x{hdr['actual_size']:x} bytes total)")
            print(f"  PrevSize   = {hdr['prev_size']}")
            print(f"  PoolType   = {hdr['pool_type']} ({'FREE' if hdr['free'] else 'allocated'})")
            print(f"  PoolIndex  = {hdr['pool_index']}")

            # Dump data (up to 256 bytes)
            data_size = min(hdr['actual_size'] - POOL_BLOCK_SIZE, 256)
            if data_size > 0:
                data = read_mem(data_addr, data_size)
                if data:
                    print(f"\n  Data ({data_size} bytes):")
                    for i in range(0, len(data), 16):
                        chunk = data[i:i+16]
                        hex_part = ' '.join(f'{b:02x}' for b in chunk)
                        hex_part = hex_part.ljust(47)
                        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
                        print(f"    +0x{i:03x}: {hex_part}  {ascii_part}")

                    # Try to identify pointers in the data
                    ptrs = []
                    for i in range(0, len(data) - 7, 8):
                        val = struct.unpack_from('<Q', data, i)[0]
                        if val >= 0xFFFFF80000000000 and val < 0xFFFFFFFFFFC00000:
                            sym = addr_to_sym(val)
                            if sym:
                                ptrs.append((i, val, sym))
                        elif val >= 0xFFFFFA8000000000 and val < 0xFFFFFA8100000000:
                            ptrs.append((i, val, "pool ptr"))
                    if ptrs:
                        print(f"\n  Identified pointers:")
                        for off, val, sym in ptrs:
                            print(f"    +0x{off:03x} = 0x{val:x}  ({sym})")

    ReactosPoolBlock()


    class ReactosPoolScan(gdb.Command):
        """Scan NonPagedPool pages for corruption.
        Usage: ros-pool-scan <start_page> [num_pages]
        Default: scan 256 pages (1MB).

        Walks pool headers on each page and reports any inconsistencies.
        Useful for finding which pool page is corrupted before/after a crash.
        """

        def __init__(self):
            super().__init__("ros-pool-scan", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            args = arg.strip().split()
            if len(args) < 1:
                print("Usage: ros-pool-scan <start_page> [num_pages]")
                return
            start = int(args[0], 0) & ~0xFFF
            num = int(args[1], 0) if len(args) >= 2 else 256

            print(f"Scanning {num} pages from 0x{start:x}...")
            corrupt_count = 0
            ok_count = 0
            skip_count = 0
            for i in range(num):
                page = start + i * 0x1000
                # Quick check: first byte must be PreviousSize=0
                if not is_pool_page(page):
                    skip_count += 1
                    continue

                blocks, errors = walk_pool_page(page)
                if errors:
                    corrupt_count += 1
                    print(f"\n  CORRUPT: 0x{page:x} ({len(blocks)} blocks)")
                    for e in errors:
                        print(f"    {e}")
                    # Print the blocks around the error
                    for b in blocks:
                        status = "FREE" if b['free'] else f"pt={b['pool_type']}"
                        print(f"    #{b['index']:3d}  +0x{b['offset']:03x}  [{b['tag']:4s}]  "
                              f"bs={b['block_size']:3d}  ps={b['prev_size']:3d}  {status}")
                else:
                    ok_count += 1

            print(f"\nDone: {ok_count} OK, {corrupt_count} CORRUPT, {skip_count} skipped")

    ReactosPoolScan()


    class ReactosPoolFind(gdb.Command):
        """Find pool blocks by tag across a range of pages.
        Usage: ros-pool-find <tag> <start_page> [num_pages]
        Example: ros-pool-find NtfF 0xFFFFFA8000200000 512

        Scans pool pages and lists all blocks whose tag matches (case-sensitive).
        """

        def __init__(self):
            super().__init__("ros-pool-find", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            args = arg.strip().split()
            if len(args) < 2:
                print("Usage: ros-pool-find <tag> <start_page> [num_pages]")
                return
            tag = args[0][:4].ljust(4)
            start = int(args[1], 0) & ~0xFFF
            num = int(args[2], 0) if len(args) >= 2 else 256

            print(f"Searching for tag '{tag}' in {num} pages from 0x{start:x}...")
            found = 0
            for i in range(num):
                page = start + i * 0x1000
                if not is_pool_page(page):
                    continue
                blocks, _ = walk_pool_page(page)
                for b in blocks:
                    if b['tag'] == tag:
                        found += 1
                        status = "FREE" if b['free'] else "alloc"
                        data_addr = page + b['offset'] + POOL_BLOCK_SIZE
                        print(f"  0x{data_addr:x}  bs={b['block_size']:3d} (0x{b['actual_size']:03x})  {status}")
            print(f"\n{found} block(s) found.")

    ReactosPoolFind()


    class ReactosPoolCrash(gdb.Command):
        """Analyze pool corruption after a #GP/BSOD in ExpCheckPoolBlocks.
        Usage: ros-pool-crash

        Automatically finds the pool page from the crash stack and
        validates it, showing which block is corrupted.
        """

        def __init__(self):
            super().__init__("ros-pool-crash", gdb.COMMAND_USER)

        def invoke(self, arg, from_tty):
            # Search the stack for pool addresses (FFFFFA80...)
            try:
                rsp = int(gdb.parse_and_eval("$rsp"))
            except:
                print("Cannot read RSP")
                return

            print("=== Pool crash analysis ===\n")

            # Find pool addresses on stack
            pool_addrs = set()
            for off in range(0, 0x800, 8):
                val = read_u64(rsp + off)
                if val is None:
                    continue
                if val >= 0xFFFFFA8000000000 and val < 0xFFFFFA8100000000:
                    pool_addrs.add(val)

            if not pool_addrs:
                print("No pool addresses found on stack.")
                return

            # Find unique pages
            pages = sorted(set(a & ~0xFFF for a in pool_addrs))
            print(f"Pool pages referenced from stack: {len(pages)}")

            for page in pages:
                print(f"\n--- Page 0x{page:x} ---")
                blocks, errors = walk_pool_page(page)

                # Find which stack addresses fall in this page
                page_refs = [a for a in pool_addrs if a & ~0xFFF == page]

                for b in blocks:
                    status = "FREE" if b['free'] else f"pt={b['pool_type']}"
                    marker = ""
                    blk_start = page + b['offset']
                    blk_end = blk_start + b['actual_size']
                    refs = [a for a in page_refs if blk_start <= a < blk_end]
                    if refs:
                        marker = f"  <<< stack refs: {', '.join(f'0x{a:x}' for a in refs)}"
                    print(f"  #{b['index']:3d}  +0x{b['offset']:03x}  [{b['tag']:4s}]  "
                          f"bs={b['block_size']:3d} (0x{b['actual_size']:03x})  "
                          f"ps={b['prev_size']:3d}  {status}{marker}")

                if errors:
                    print(f"\n  *** CORRUPTION DETECTED:")
                    for e in errors:
                        print(f"    {e}")
                else:
                    print(f"  ({len(blocks)} blocks, no header corruption)")

            # Also dump the call chain for context
            print(f"\n--- Call chain (return addresses on stack) ---")
            for off in range(0, 0x600, 8):
                val = read_u64(rsp + off)
                if val is None:
                    continue
                if 0xFFFFF80000400000 <= val <= 0xFFFFF880FFFFFFFF:
                    sym = addr_to_sym(val)
                    if sym:
                        print(f"  [RSP+0x{off:03x}] {sym}")

    ReactosPoolCrash()


    # ============================================================
    # Print available commands (updated)
    # ============================================================
    print("=== ReactOS GDB helpers loaded ===")
    print("Commands:")
    print("  ros-load-symbols      Auto-find ntoskrnl and load DWARF symbols")
    print("  ros-load-at <addr>    Load ntoskrnl at a known base address")
    print("  ros-load-module <addr> <path>  Load symbols for any module")
    print("  ros-lsmod [--load]    List loaded modules (walk PsLoadedModuleList)")
    print("  ros-addr2mod <addr>   Find which module owns an address")
    print("  ros-thread <addr>     Inspect thread: stack, APC state, wait info")
    print("  ros-waiters <addr>    Find threads waiting on a dispatcher object")
    print("  ros-irp <addr>        Inspect an IRP structure")
    print("  ros-trapframes [lo] [hi]  Scan stack for KTRAP_FRAMEs")
    print("  ros-frame-regs <rsp> [pat]  Recover pushed regs from function frame")
    print("  ros-callchain [rsp] [n]  Heuristic stack scan for return addresses")
    print("  ros-findmod <addr>    Find module base by scanning for MZ/PE")
    print("  ros-verify <base> <path>  Compare loaded binary vs build (SHA256)")
    print("Pool debugging:")
    print("  ros-pool-page <addr>  Walk & validate all blocks on a pool page")
    print("  ros-pool-block <addr> Inspect a single pool allocation")
    print("  ros-pool-scan <start> [n]  Scan N pages for corruption")
    print("  ros-pool-find <tag> <start> [n]  Find blocks by pool tag")
    print("  ros-pool-crash        Auto-analyze pool corruption from crash stack")
    print("")

    # Auto-load symbols
    gdb.execute("ros-load-symbols")

end

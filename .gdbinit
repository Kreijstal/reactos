# ReactOS GDB debug helper for QEMU
# Usage: gdb -x .gdbinit
#
# Auto-detects real mode (bootloader) vs long mode (kernel).
# Connects to QEMU gdbstub and loads symbols for ntoskrnl + drivers.

set architecture i386:x86-64
set disassembly-flavor intel
set pagination off
set confirm off

# QEMU gdbstub default
target remote :1234

python
import gdb
import struct
import os

KSEG0_BASE = 0xFFFFF80000000000
BUILD_DIR = os.path.join(os.getcwd(), 'build')

def detect_cpu_mode():
    """Detect CPU mode by reading CR0 and CS.
    Returns 'real', 'protected', or 'long'."""
    try:
        cr0 = int(gdb.parse_and_eval("$cr0"))
        cs = int(gdb.parse_and_eval("$cs"))
        efer = int(gdb.parse_and_eval("$efer"))
    except:
        return "unknown"

    pe = cr0 & 1          # Protected mode enable
    lme = efer & 0x100    # Long mode enable
    lma = efer & 0x400    # Long mode active

    if not pe:
        return "real"
    elif lma:
        return "long"
    else:
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


    # Print available commands
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
    print("")

    # Auto-load symbols
    gdb.execute("ros-load-symbols")

end

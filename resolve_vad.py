#!/usr/bin/env python3
"""Resolve VADASSERT frame addresses to ntkrnlmp symbols.
Uses the 'self=' anchor (runtime VA of MiDbgAssertIsLockedForRead) to compute
the load slide, then maps each frame to the nearest preceding symbol."""
import re, subprocess, sys

KRNL = '/home/kreijstal/git/reactos/build_nt10/ntoskrnl/ntkrnlmp/ntkrnlmp.exe'
CAP  = sys.argv[1] if len(sys.argv) > 1 else '/home/kreijstal/git/reactos/vad_capture.log'

# nm symbol table (file VAs, ImageBase 0x400000)
syms = []
out = subprocess.run(['x86_64-w64-mingw32-nm', KRNL], capture_output=True, text=True).stdout
for line in out.splitlines():
    m = re.match(r'^([0-9a-fA-F]+)\s+[TtWw]\s+(\S+)', line)
    if m:
        syms.append((int(m.group(1), 16), m.group(2)))
syms.sort()
SELF_FILE_VA = next(a for a, n in syms if n == 'MiDbgAssertIsLockedForRead')

def nearest(file_va):
    lo, hi, best = 0, len(syms) - 1, None
    for a, n in syms:
        if a <= file_va:
            best = (a, n)
        else:
            break
    if best:
        return f"{best[1]}+0x{file_va-best[0]:x}"
    return "?"

text = open(CAP).read()
for block in text.split('=== capture'):
    if 'VADASSERT' not in block:
        continue
    m = re.search(r'self=([0-9a-fA-F]+)', block)
    if not m:
        print("(no self anchor in block)")
        continue
    self_rt = int(m.group(1), 16)
    slide = self_rt - SELF_FILE_VA
    print(f"\n--- block (slide=0x{slide:x}) ---")
    for line in block.splitlines():
        if 'self=' in line or 'plCount' in line:
            print(line.strip())
        fm = re.search(r'frame\[(\d+)\]=([0-9a-fA-F]+)', line)
        if fm:
            rt = int(fm.group(2), 16)
            file_va = rt - slide
            print(f"  frame[{fm.group(1)}]={rt:016x} -> {nearest(file_va)}")

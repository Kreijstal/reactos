# findmod.py <mon.sock> <rip-hex> : scan back from RIP for an MZ/PE header via the QEMU HMP 'x' command
import socket, sys, re, time
sock, rip = sys.argv[1], int(sys.argv[2], 16)
s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(3)
def cmd(c):
    s.sendall((c + '\n').encode()); buf = b''
    t = time.time()
    while time.time() - t < 3:
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        buf += d
        if buf.rstrip().endswith(b'(qemu)'): break
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', buf.decode('latin1'))
cmd('')
def rd32(a):
    o = cmd('x /1wx 0x%x' % a)
    m = re.findall(r'^[0-9a-f]+:\s+0x([0-9a-f]+)', o, re.M)
    return int(m[-1], 16) if m else None
page = rip & ~0xfff
for i in range(8192):
    a = page - i * 0x1000
    v = rd32(a)
    if v is not None and (v & 0xffff) == 0x5a4d:
        lfanew = rd32(a + 0x3c)
        if lfanew and lfanew < 0x1000 and rd32(a + lfanew) == 0x4550:
            ts = rd32(a + lfanew + 8); soi = rd32(a + lfanew + 0x18 + 0x38)
            print('base=0x%x off=0x%x timestamp=0x%x sizeofimage=0x%x' % (a, rip - a, ts, soi))
            break
else:
    print('no MZ found')

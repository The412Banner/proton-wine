import hashlib, os, struct, sys
new, old = sys.argv[1], sys.argv[2]
def files(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            if not os.path.islink(p):
                out[os.path.relpath(p, root)] = p
    return out
def norm(b):
    # zero build-time fields of a PE image: COFF TimeDateStamp, optional-header CheckSum, debug-directory TimeDateStamps
    if len(b) < 0x40 or b[:2] != b'MZ':
        return None
    b = bytearray(b)
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    if b[pe:pe+4] != b'PE\0\0':
        return None
    b[pe+8:pe+12] = b'\0' * 4
    opt = pe + 24
    magic = struct.unpack_from('<H', b, opt)[0]
    b[opt+64:opt+68] = b'\0' * 4
    ddir = opt + (96 if magic == 0x10b else 112)
    nsec = struct.unpack_from('<H', b, pe+6)[0]
    sec = opt + struct.unpack_from('<H', b, pe+20)[0]
    rva, size = struct.unpack_from('<II', b, ddir + 6*8)
    if rva and size:
        for i in range(nsec):
            s = sec + 40*i
            vsz, va, rsz, raw = struct.unpack_from('<IIII', b, s+8)
            if va <= rva < va + max(vsz, rsz):
                off = raw + (rva - va)
                for e in range(size // 28):
                    o = off + 28*e
                    if o + 28 <= len(b):
                        b[o+4:o+8] = b'\0' * 4
                break
    return bytes(b)
a, o = files(new), files(old)
print(f"files new={len(a)} tested={len(o)} only-new={sorted(set(a)-set(o))[:10]} only-tested={sorted(set(o)-set(a))[:10]}")
same = stamp = 0; real = []
for k in sorted(set(a) & set(o)):
    x, y = open(a[k], 'rb').read(), open(o[k], 'rb').read()
    if x == y:
        same += 1; continue
    nx, ny = norm(x), norm(y)
    if nx is not None and nx == ny:
        stamp += 1
    else:
        real.append(f"{k} ({'PE' if nx is not None else 'non-PE'}, {len(x)} vs {len(y)} B)")
print(f"identical={same} differ-only-in-PE-build-stamps={stamp} other-differences={len(real)}")
for r in real[:60]:
    print("  DIFF", r)

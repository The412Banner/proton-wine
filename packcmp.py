import hashlib, io, re, sys, tarfile
def load(p):
    out = {}
    with tarfile.open(p, 'r:xz') as t:
        for m in t.getmembers():
            if m.isfile():
                out[m.name] = t.extractfile(m).read()
            elif m.issym() or m.islnk():
                out[m.name] = ('link:' + m.linkname).encode()
    return out
def norm(name, b):
    if name.endswith('.reg'):
        b = re.sub(rb'^(\[[^\n]*\]) \d+\r?$', rb'\1', b, flags=re.M)          # key write times
        b = re.sub(rb'^#time=[0-9a-f]+\r?$', b'#time=', b, flags=re.M)
    return b
a, o = load(sys.argv[1]), load(sys.argv[2])
print(f"entries new={len(a)} tested={len(o)} only-new={sorted(set(a)-set(o))[:10]} only-tested={sorted(set(o)-set(a))[:10]}")
same = ts = 0; real = []
for k in sorted(set(a) & set(o)):
    if a[k] == o[k]: same += 1
    elif norm(k, a[k]) == norm(k, o[k]): ts += 1
    else: real.append(k)
print(f"identical={same} differ-only-in-reg-timestamps={ts} other={len(real)} {real[:20]}")

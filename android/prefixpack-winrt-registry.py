#!/usr/bin/env python3
"""Register every Windows Runtime class the layer's own DLLs provide in the prefix pack.

The prefix packs (GameNative/bionic-prefix-files, 2025) were generated with an older Wine.
Classes added since (Windows.Foundation.Collections.PropertySet, Storage.Streams Buffer /
DataWriter / InMemoryRandomAccessStream, DispatcherQueueController, MediaPlayer, ...) are only
registered when wineboot updates the prefix on the first launch, and on arm64ec that update
writes them to the 32-bit view (Wow6432Node) only: a 64-bit game's RoGetActivationFactory then
fails with REGDB_E_CLASSNOTREG (Xbox sign-in through PropertySet, 2026-10-22).

Each Wine DLL carries its registrations as a WINE_REGISTRY script ("NoRemove ActivatableClassId
{ ForceRemove <class> { val 'DllPath' = s '%MODULE%' } ... }"). This reads the class names from
the layer's 64-bit PE DLLs (64-bit view) and i386 PE DLLs (Wow6432Node), and adds every class a
view lacks, with DllPath C:\\windows\\system32\\<dll> as wineboot writes it for both views.
Existing entries are left as they are; the pack is written back in the same layout.

    prefixpack-winrt-registry.py PACK.txz PE64_DIR PE32_DIR OUT.txz
    prefixpack-winrt-registry.py --reg SYSTEM.reg PE64_DIR PE32_DIR OUT.reg
"""
import io
import lzma
import os
import re
import sys
import tarfile
import time

KEY64 = "Software\\\\Microsoft\\\\WindowsRuntime\\\\ActivatableClassId\\\\"
KEY32 = "Software\\\\Wow6432Node\\\\Microsoft\\\\WindowsRuntime\\\\ActivatableClassId\\\\"
SECTION_RE = re.compile(r"^\[(Software\\\\(?:Wow6432Node\\\\)?Microsoft\\\\WindowsRuntime\\\\ActivatableClassId\\\\[^\]\\]+)\]", re.M)


def dll_classes(pe_dir):
    """{class name: dll name} from the WINE_REGISTRY scripts of every DLL in pe_dir."""
    out = {}
    if not pe_dir or not os.path.isdir(pe_dir):
        return out
    for name in sorted(os.listdir(pe_dir)):
        if not name.lower().endswith(".dll"):
            continue
        data = open(os.path.join(pe_dir, name), "rb").read()
        start = 0
        while True:
            i = data.find(b"NoRemove ActivatableClassId", start)
            if i < 0:
                break
            j = data.find(b"{", i)
            depth, k = 0, j
            while k < len(data):
                c = data[k:k + 1]
                if c == b"{":
                    depth += 1
                elif c == b"}":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            block = data[j:k].decode("latin-1")
            for m in re.finditer(r"ForceRemove\s+([A-Za-z0-9_.]+)", block):
                out.setdefault(m.group(1), name.lower())
            start = k
    return out


def merge(text, classes64, classes32, now=None):
    present = {s.lower() for s in SECTION_RE.findall(text)}
    now = int(now if now is not None else time.time())
    filetime = (now + 11644473600) * 10000000
    add = []
    for prefix, classes in ((KEY64, classes64), (KEY32, classes32)):
        for cls, dll in sorted(classes.items()):
            key = prefix + cls
            if key.lower() in present:
                continue
            add.append((key, dll))
    body = text.rstrip("\n") + "\n\n"
    for key, dll in add:
        body += (f"[{key}] {now}\n#time={filetime:x}\n"
                 f"\"DllPath\"=\"C:\\\\windows\\\\system32\\\\{dll}\"\n\n")
    return body, add


def report(add, where):
    n64 = sum(1 for k, _ in add if "Wow6432Node" not in k)
    print(f"{where}: added {n64} classes to the 64-bit view, {len(add) - n64} to Wow6432Node")
    for key, dll in add:
        print(f"  {key.replace(chr(92) * 2, chr(92))} -> {dll}")


def main_pack(pack, pe64, pe32, out):
    with tarfile.open(pack, "r:xz") as tf:
        reg = next(m for m in tf.getmembers() if m.name.endswith("system.reg"))
        text = tf.extractfile(reg).read().decode("utf-8", "replace")
    new_text, add = merge(text, dll_classes(pe64), dll_classes(pe32))
    data = io.BytesIO()
    with tarfile.open(pack, "r:xz") as tin, tarfile.open(fileobj=data, mode="w") as tout:
        for m in tin.getmembers():
            if m.name == reg.name:
                payload = new_text.encode("utf-8")
                m.size = len(payload)
                tout.addfile(m, io.BytesIO(payload))
            elif m.isfile():
                tout.addfile(m, tin.extractfile(m))
            else:
                tout.addfile(m)
    with lzma.open(out, "wb", preset=9) as f:
        f.write(data.getvalue())
    report(add, reg.name)


def main_reg(reg, pe64, pe32, out):
    new_text, add = merge(open(reg, encoding="utf-8", errors="replace").read(),
                          dll_classes(pe64), dll_classes(pe32))
    open(out, "w", encoding="utf-8").write(new_text)
    report(add, out)


if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "--reg":
        main_reg(*sys.argv[2:])
    elif len(sys.argv) == 5:
        main_pack(*sys.argv[1:])
    else:
        raise SystemExit(__doc__)

"""Grep the config.bin of every PBO under a folder for given byte strings."""
import os, struct, sys

def read_cstr(f):
    b = bytearray()
    while (c := f.read(1)) not in (b"", b"\0"):
        b += c
    return b.decode("latin-1")

def config_blobs(path):
    with open(path, "rb") as f:
        ents = []
        while True:
            name = read_cstr(f)
            hdr = f.read(20)
            if len(hdr) < 20:
                return
            method, orig, _r, _t, size = struct.unpack("<5I", hdr)
            if not name and method == 0x56657273:
                while read_cstr(f):
                    pass
                continue
            if not name:
                break
            ents.append((name, size))
        off = f.tell()
        for name, size in ents:
            if name.lower().endswith("config.bin"):
                f.seek(off)
                yield name, f.read(size)
            off += size

root, needles = sys.argv[1], [n.encode() for n in sys.argv[2:]]
for dp, _dn, fn in os.walk(root):
    for n in fn:
        if not n.lower().endswith(".pbo"):
            continue
        p = os.path.join(dp, n)
        try:
            for cname, blob in config_blobs(p):
                hits = [k.decode() for k in needles if k in blob]
                if hits:
                    print(f"{os.path.relpath(p, root)} :: {cname} :: {hits}")
        except Exception as e:
            print(f"ERR {p}: {e}")

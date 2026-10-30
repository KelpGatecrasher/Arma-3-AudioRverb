"""Minimal PBO reader: list entries or extract one file by name."""
import struct, sys

def read_cstr(f):
    b = bytearray()
    while (c := f.read(1)) not in (b"", b"\0"):
        b += c
    return b.decode("latin-1")

def entries(path):
    with open(path, "rb") as f:
        out = []
        while True:
            name = read_cstr(f)
            method, orig, _res, _ts, size = struct.unpack("<5I", f.read(20))
            if not name and method == 0x56657273:  # 'Vers' header extension
                while read_cstr(f):
                    pass
                continue
            if not name:
                break
            out.append((name, method, orig, size))
        data_start = f.tell()
    return out, data_start

def main():
    path, cmd = sys.argv[1], sys.argv[2]
    ents, off = entries(path)
    if cmd == "list":
        for n, m, o, s in ents:
            print(f"{s:>10} {m:#x} {n}")
        return
    want, dest = sys.argv[3], sys.argv[4]
    for n, m, o, s in ents:
        if n.lower() == want.lower():
            if m not in (0,):
                sys.exit(f"{n} is packed (method {m:#x}); not supported")
            with open(path, "rb") as f:
                f.seek(off)
                data = f.read(s)
            open(dest, "wb").write(data)
            print(f"wrote {dest} ({s} bytes)")
            return
        off += s
    sys.exit("not found")

main()

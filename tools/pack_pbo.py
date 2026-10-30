"""Pack a folder into an uncompressed Arma 3 PBO.

usage: pack_pbo.py <source_dir> <out.pbo> <prefix>

Files are stored as-is (config.cpp is accepted unbinarized by the engine). Writes the
'Vers' header extension with the prefix and the trailing SHA1 checksum.
"""
import hashlib, os, struct, sys

def main():
    src, out, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    files = []
    for dp, _dn, fn in os.walk(src):
        for n in fn:
            full = os.path.join(dp, n)
            rel = os.path.relpath(full, src).replace("/", "\\")
            if n.startswith("$") or n.startswith("."):
                continue
            files.append((rel, full))
    files.sort(key=lambda f: f[0].lower())

    hdr = bytearray()
    hdr += b"\0" + struct.pack("<5I", 0x56657273, 0, 0, 0, 0)
    hdr += b"prefix\0" + prefix.encode() + b"\0" + b"\0"
    blobs = []
    for rel, full in files:
        data = open(full, "rb").read()
        ts = int(os.path.getmtime(full))
        hdr += rel.encode() + b"\0" + struct.pack("<5I", 0, len(data), 0, ts, len(data))
        blobs.append(data)
    hdr += b"\0" + struct.pack("<5I", 0, 0, 0, 0, 0)

    body = bytes(hdr) + b"".join(blobs)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(body + b"\0" + hashlib.sha1(body).digest())
    print(f"packed {len(files)} files -> {out}")

main()

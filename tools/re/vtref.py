"""Find 8-byte absolute pointers to a VA (vtable slots), then code that references the vtable base.
Also resolves MSVC RTTI class name for the vtable when present.
usage: vtref.py exe 0xVA [0xVA...]"""
import sys, struct
import numpy as np
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
tb = np.frombuffer(img[t0:t1], dtype=np.uint8)
n = len(tb) - 4
disp = (tb[0:n].astype(np.int64) | (tb[1:n+1].astype(np.int64) << 8) |
        (tb[2:n+2].astype(np.int64) << 16) | (tb[3:n+3].astype(np.int64) << 24))
disp = np.where(disp >= 2**31, disp - 2**32, disp)
targets = np.arange(n, dtype=np.int64) + t0 + 4 + disp

def rtti_name(vt_rva):
    # vtable[-1] -> CompleteObjectLocator; COL+0xC = TypeDescriptor rva; TD+0x10 = name
    col_va = struct.unpack_from("<Q", img, vt_rva - 8)[0]
    col = col_va - base
    if not (0 < col < len(img) - 0x18):
        return None
    td = struct.unpack_from("<I", img, col + 0xC)[0]
    if not (0 < td < len(img)):
        return None
    end = img.find(b"\0", td + 0x10, td + 0x200)
    return img[td + 0x10:end].decode("latin-1", "replace")

for a in sys.argv[2:]:
    va = int(a, 16)
    key = struct.pack("<Q", va)
    pos = 0
    print(f"=== pointers to {va:#x}")
    while (pos := img.find(key, pos)) != -1:
        if pos % 8 == 0:
            # walk back to vtable start (previous slot not a .text pointer)
            s = pos
            while True:
                prev = struct.unpack_from("<Q", img, s - 8)[0] - base
                if t0 <= prev < t1:
                    s -= 8
                else:
                    break
            name = rtti_name(s)
            refs = [hex(base + int(p) + t0) for p in np.nonzero(targets == s)[0][:6]]
            print(f"  slot rva {pos:#x} (index {(pos - s)//8}) vtable {base+s:#x} rtti={name} code refs={refs}")
        pos += 1

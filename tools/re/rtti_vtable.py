"""Find the vtable(s) of an MSVC RTTI class name in an x64 PE and list their slots.
Also lists rip-relative code references to each vtable (constructors) and absolute
pointers to it in data.

usage: rtti_vtable.py exe ".?AVSoundSystemXA2@@" [max_slots]"""
import struct, sys
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
name = sys.argv[2].encode()
max_slots = int(sys.argv[3]) if len(sys.argv) > 3 else 60
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize

td_name = img.find(name + b"\0")
td = td_name - 0x10
print(f"TypeDescriptor rva {td:#x}")
# CompleteObjectLocator: sig(4) offset(4) cdOffset(4) pTypeDescriptor(4 rva) pClassDescriptor(4) pSelf(4)
cols = []
pos = 0
key = struct.pack("<I", td)
while (pos := img.find(key, pos)) != -1:
    col = pos - 0xC
    sig, off, cd, tdr, chd, selfr = struct.unpack_from("<6I", img, col)
    if sig == 1 and selfr == col:
        cols.append((col, off))
    pos += 1
for col, off in cols:
    p = img.find(struct.pack("<Q", base + col))
    while p != -1:
        vt = p + 8
        print(f"COL {col:#x} (this-offset {off:#x}) -> vtable rva {vt:#x} va {base+vt:#x}")
        for i in range(max_slots):
            f = struct.unpack_from("<Q", img, vt + 8 * i)[0] - base
            if not (t0 <= f < t1):
                break
            print(f"   [{i:2}] {base+f:#x}")
        p = img.find(struct.pack("<Q", base + col), p + 1)

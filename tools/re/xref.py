"""Find code xrefs to ASCII strings in an x64 PE and summarise the enclosing functions.

usage: xref.py exe "str1" "str2" ... [--dis]
For each string: lists functions referencing it (via .pdata bounds) and, for each
function, every string literal that function references, in order. --dis prints
full disassembly of each function too.
"""
import bisect, struct, sys
import numpy as np
import pefile, capstone

exe = sys.argv[1]
args = [a for a in sys.argv[2:] if not a.startswith("--")]
dis = "--dis" in sys.argv

pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize

funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]

def func_of(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0 and funcs[i][0] <= rva < funcs[i][1]:
        return funcs[i]
    return None

# rip-relative disp32 candidates: target = p + 4 + disp (disp being last 4 bytes of insn)
tb = np.frombuffer(img[t0:t1], dtype=np.uint8)
n = len(tb) - 4
disp = (tb[0:n].astype(np.int64) | (tb[1:n+1].astype(np.int64) << 8) |
        (tb[2:n+2].astype(np.int64) << 16) | (tb[3:n+3].astype(np.int64) << 24))
disp = np.where(disp >= 2**31, disp - 2**32, disp)
targets = np.arange(n, dtype=np.int64) + t0 + 4 + disp

def cstr_at(rva):
    end = img.find(b"\0", rva, rva + 300)
    if end <= rva:
        return None
    s = img[rva:end]
    if len(s) >= 3 and all(32 <= c < 127 for c in s):
        return s.decode()
    return None

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = False

def summarise(f):
    code = img[f[0]:f[1]]
    strs = []
    lines = []
    for ins in md.disasm(code, base + f[0]):
        extra = ""
        if "rip" in ins.op_str and "[" in ins.op_str:
            try:
                d = ins.op_str.split("rip")[1].split("]")[0].replace(" ", "")
                tgt = ins.address + ins.size + int(d, 16) - base
                s = cstr_at(tgt)
                if s:
                    strs.append(s)
                    extra = f'   ; "{s}"'
                else:
                    extra = f"   ; rva {tgt:#x}"
            except ValueError:
                pass
        lines.append(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}{extra}")
    return strs, lines

for s in args:
    if s.startswith("@"):  # disassemble function containing address
        f = func_of(int(s[1:], 16) - base)
        print(f"=== func {base+f[0]:#x}-{base+f[1]:#x}")
        print("\n".join("    " + l for l in summarise(f)[1]))
        continue
    if s.startswith("callers:"):  # direct E8/E9 rel32 calls to address
        tgt = int(s[8:], 16) - base
        cands = np.nonzero(targets == tgt)[0]
        out = set()
        for p in cands:
            if p >= 1 and tb[p - 1] in (0xE8, 0xE9):
                f = func_of(int(p) + t0)
                out.add((hex(base + int(p) + t0 - 1), hex(base + f[0]) if f else None))
        print(f"=== callers of {s[8:]}: {sorted(out)}")
        continue
    key = b"\0" + s.encode() + b"\0"
    hits = []
    pos = 0
    while (pos := img.find(key, pos)) != -1:
        hits.append(pos + 1)
        pos += 1
    print(f"=== '{s}' string rvas: {[hex(h) for h in hits]}")
    seen = set()
    for h in hits:
        for p in np.nonzero(targets == h)[0]:
            f = func_of(int(p) + t0)
            if not f or f in seen:
                continue
            seen.add(f)
            strs, lines = summarise(f)
            print(f"  func {base+f[0]:#x}-{base+f[1]:#x} ({f[1]-f[0]} bytes) strings: {strs}")
            if dis:
                print("\n".join("    " + l for l in lines))

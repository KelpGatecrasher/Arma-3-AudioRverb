"""Locate XAPOFX / XAudio2 reverb CLSIDs in an exe and the functions referencing them,
plus callers of the CreateFX IAT slot."""
import sys, uuid, bisect
import numpy as np, pefile

exe, iat_va = sys.argv[1], int(sys.argv[2], 16)
guids = {
    "FXEQ": "F5E01117-D6C4-485A-A3F5-695196F3DBFA",
    "FXMasteringLimiter": "C4137916-2BE1-46FD-8599-441536F49856",
    "FXReverb": "7D9ACA56-CB68-4807-B632-B137352E8596",
    "FXEcho": "5039D740-F736-449A-84D3-A57202557B87",
    "AudioReverb(2.7 COM)": "6A93130E-1D53-41D1-A9CF-E758800BB179",
    "AudioReverb(2.9 COM)": "C2633B16-471B-4498-B8C5-4F0959E2EC09",
}
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
def func_of(r):
    i = bisect.bisect_right(starts, r) - 1
    return hex(base + funcs[i][0]) if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None
tb = np.frombuffer(img[t0:t1], dtype=np.uint8)
n = len(tb) - 4
d = (tb[0:n].astype(np.int64) | (tb[1:n+1].astype(np.int64) << 8) |
     (tb[2:n+2].astype(np.int64) << 16) | (tb[3:n+3].astype(np.int64) << 24))
d = np.where(d >= 2**31, d - 2**32, d)
targets = np.arange(n, dtype=np.int64) + t0 + 4 + d

def refs(rva):
    return sorted({func_of(int(p) + t0) for p in np.nonzero(targets == rva)[0]} - {None})

for name, g in guids.items():
    b = uuid.UUID(g).bytes_le
    pos, hits = 0, []
    while (pos := img.find(b, pos)) != -1:
        hits.append(pos); pos += 1
    print(f"{name}: {[hex(h) for h in hits]} refs={[refs(h) for h in hits]}")
print("CreateFX IAT callers:", refs(iat_va - base))

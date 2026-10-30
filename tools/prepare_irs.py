"""Turn OpenAIR impulse responses into AudioReverb convolution IRs.

usage: prepare_irs.py <dir with the OpenAIR zips> <output dir>

For each selected recording:
  1. Stereo: B-format -> virtual cardioids at +/-90 deg (FuMa vs AmbiX detected from which
     channel carries the frontal direct sound); mid/side -> M +/- S; ORTF stereo as-is;
     mono -> two decorrelated channels.
  2. Align the direct sound to t = 0 and remove it (first 2.5 ms zeroed, 1 ms fade-in): the
     game's dry signal already plays, so leaving it in would double every sound.
  3. 40 Hz high-pass, resample to 44.1 kHz (the game's 3D mix rate), trim to 4.2 s with a fade-out.
  4. Peak-normalise to 0.9 and write 32-bit float stereo WAV. Prints the measured T60.
Only reads the zips; nothing in them is executed.
"""
import io, struct, sys, zipfile
from pathlib import Path
from math import gcd

import numpy as np
from scipy.signal import butter, resample_poly, sosfiltfilt

OUT_RATE = 44100
MAX_SEC = 4.2
DIRECT_CUT_MS = 2.5

# name -> (zip, member, kind)   kind: bformat | ms | stereo
SOURCES = {
    "open_koli":          ("koli-national-park-summer.zip", "koli-national-park-summer/b-format/koli_summer_site1_1way_bformat.wav", "bformat"),
    "forest_wheldrake":   ("Forest-in-WheldrakeWood.zip", "Forest-in-WheldrakeWood/IR_SF/S1R1_MS.wav", "ms"),
    "urban_alcuin":       ("alcuin-college-university-york.zip", "alcuin-college-university-york/b-format/alcuin_s1r1_bformat.wav", "bformat"),
    "urbandense_trollers":("trollers-gill.zip", "trollers-gill/b-format/dales_site2_1way_bformat.wav", "bformat"),
    "indoorsmall_stairway":("stairway-university-york.zip", "stairway-university-york/stereo/stairwell_ortf.wav", "stereo"),
    "indoormedium_typing":("terrys-typing-room.zip", "terrys-typing-room/stereo/terrys_typing_ortf.wav", "stereo"),
    "indoorlarge_warehouse":("terrys-factory-warehouse.zip", "terrys-factory-warehouse/stereo/terrys_warehouse_stereo.wav", "stereo"),
    # The hangar's "B-format" files carry the same signal on all four channels, so it is treated
    # as mono and decorrelated into stereo.
    "hangar_t2":          ("air-museum.zip", "b-format/AR_bformat_S1R1_1.wav", "mono"),
}


def decorrelate(x, rate, seed):
    """Convolve with a short decaying velvet-noise filter (unit energy): same spectrum and decay,
    independent fine structure per seed."""
    rng = np.random.default_rng(seed)
    n = int(0.03 * rate)
    v = np.zeros(n)
    period = rate / 1500.0
    t = 0.0
    while t < n:
        i = int(t + rng.uniform(0, period))
        if i < n:
            v[i] = rng.choice([-1.0, 1.0]) * np.exp(-i / (0.01 * rate))
        t += period
    v /= np.sqrt(np.sum(v ** 2))
    return np.convolve(x, v)[: len(x)]


def read_wav(data):
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV")
    p, fmt, pcm = 12, None, None
    while p + 8 <= len(data):
        cid, ln = data[p:p + 4], struct.unpack("<I", data[p + 4:p + 8])[0]
        body = data[p + 8:p + 8 + ln]
        if cid == b"fmt ":
            tag, ch, rate = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            pcm = body
        p += 8 + ln + (ln & 1)
    tag, ch, rate, bits = fmt
    if tag == 3:
        x = np.frombuffer(pcm, dtype="<f4").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(pcm, dtype="<i2") / 32768.0
    elif bits == 24:
        b = np.frombuffer(pcm[: len(pcm) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v / 8388608.0
    elif bits == 32:
        x = np.frombuffer(pcm, dtype="<i4") / 2147483648.0
    else:
        raise ValueError(f"unsupported bits {bits}")
    x = x[: len(x) // ch * ch].reshape(-1, ch)
    return x, rate


def to_stereo(x, kind, rate):
    if kind == "mono":
        m = x[:, 0]
        return decorrelate(m, rate, 1), decorrelate(m, rate, 2), "mono -> decorrelated stereo"
    if kind == "stereo":
        return x[:, 0], x[:, 1], "ORTF stereo"
    if kind == "ms":
        m, s = x[:, 0], x[:, 1]
        return m + s, m - s, "mid/side"
    # B-format: the microphone faced the source, so the frontal channel (X) carries the biggest
    # direct-sound peak among the three directional channels. FuMa = W X Y Z, AmbiX = W Y Z X.
    w = x[:, 0]
    peak = int(np.argmax(np.abs(w[: int(0.3 * len(w))])))
    win = slice(max(0, peak - 48), peak + 48)
    energy = [np.sum(x[win, c] ** 2) for c in (1, 2, 3)]
    if int(np.argmax(energy)) == 2:   # X in the last channel -> AmbiX (SN3D, W unscaled)
        y, wn, layout = x[:, 1], w, "AmbiX"
    else:                             # FuMa (W carries -3 dB)
        y, wn, layout = x[:, 2], w * np.sqrt(2.0), "FuMa"
    return 0.5 * (wn + y), 0.5 * (wn - y), f"B-format ({layout}) -> cardioids +/-90"


def t60(l, r, rate):
    """T30-based T60 from Schroeder backward integration of the per-sample energy of both channels."""
    e = np.cumsum((l ** 2 + r ** 2)[::-1])[::-1]
    e = 10 * np.log10(np.maximum(e / e[0], 1e-12))
    try:
        i5 = np.argmax(e <= -5)
        i35 = np.argmax(e <= -35)
        if i35 <= i5:
            return float("nan")
        return 2.0 * (i35 - i5) / rate
    except ValueError:
        return float("nan")


def write_float_wav(path, l, r, rate):
    inter = np.empty(len(l) * 2, dtype="<f4")
    inter[0::2], inter[1::2] = l, r
    data = inter.tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 3, 2, rate, rate * 8, 8, 32) + b"data" + struct.pack("<I", len(data))
    path.write_bytes(hdr + data)


def main():
    src_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, (zname, member, kind) in SOURCES.items():
        with zipfile.ZipFile(src_dir / zname) as z:
            x, rate = read_wav(z.read(member))
        l, r, how = to_stereo(x, kind, rate)

        # Direct sound: earliest strong peak of the summed signal.
        mono = np.abs(l) + np.abs(r)
        search = mono[: int(0.3 * rate)]
        peak = int(np.argmax(search))
        l, r = l[peak:], r[peak:]
        cut = int(DIRECT_CUT_MS * 1e-3 * rate)
        fade = int(1e-3 * rate)
        ramp = np.concatenate([np.zeros(cut), np.linspace(0, 1, fade), np.ones(max(0, len(l) - cut - fade))])[: len(l)]
        l, r = l * ramp, r * ramp

        sos = butter(2, 40, btype="highpass", fs=rate, output="sos")
        l, r = sosfiltfilt(sos, l), sosfiltfilt(sos, r)
        g = gcd(OUT_RATE, rate)
        l = resample_poly(l, OUT_RATE // g, rate // g)
        r = resample_poly(r, OUT_RATE // g, rate // g)

        n = min(len(l), int(MAX_SEC * OUT_RATE))
        l, r = l[:n], r[:n]
        tail = int(0.15 * n)
        fade_out = np.ones(n)
        fade_out[n - tail:] = np.linspace(1, 0, tail)
        l, r = l * fade_out, r * fade_out
        peak_amp = max(np.max(np.abs(l)), np.max(np.abs(r)), 1e-9)
        l, r = 0.9 * l / peak_amp, 0.9 * r / peak_amp

        corr = float(np.corrcoef(l, r)[0, 1])
        out = out_dir / f"{name}.wav"
        write_float_wav(out, l.astype(np.float32), r.astype(np.float32), OUT_RATE)
        print(f"{name:24} {how:38} src {rate} Hz  {n / OUT_RATE:4.2f} s  T60 {t60(l, r, OUT_RATE):4.2f} s"
              f"  L/R corr {corr:+.2f}  -> {out.name}")


main()

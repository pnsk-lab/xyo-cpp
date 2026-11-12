#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Generate a tiny, original CC0 SF2 fallback for xyo music.

All PCM is synthesized mathematically (no samples from third parties).
Not a replacement for a high-quality GM SoundFont; it is a portable default.
"""
from pathlib import Path
import math
import random
import struct

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/soundfonts/xyo-basic.sf2"
RATE = 44100
FRAME_COUNT = 16856  # ~100 complete cycles at middle C
GM_PROGRAMS = (0, 4, 19, 24, 26, 32, 45, 48, 56, 71, 65,
               73, 74, 68, 52, 11, 9, 13, 47, 80, 88)
DRUM_KEYS = (38, 36, 42, 46, 49, 47, 51, 39, 75,
             60, 54, 56, 76, 77, 81, 80, 37, 35)


def chunk(kind, data):
    data = bytes(data)
    return kind + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def group(kind, *chunks):
    return chunk(b"LIST", kind + b"".join(chunks))


def name_bytes(value):
    return value.encode("ascii")[:20].ljust(20, b"\0")


def generator(op, amount):
    return struct.pack("<HH", op, amount & 0xffff)


def melodic_sample(index):
    # Repeating harmonic oscillator: different spectra per instrument.
    # Fractional harmonics are intentionally excluded to avoid loop pops.
    rng = random.Random(100 + index)
    harmonics = [0.0] + [
        (rng.uniform(0.6, 1.0) / h) * (
            1 if index % 5 in (0, 2, 4) else (1 if h % 2 else 0.18))
        for h in range(1, 11)
    ]
    if index in (0, 1, 15, 16):
        harmonics = [0] + [1 if h == 1 else 0.12/(h*h)
                           for h in range(1, 11)]
    samples = []
    for t in range(FRAME_COUNT):
        phase = math.tau * 100 * t / FRAME_COUNT
        v = sum(harmonics[h] * math.sin(phase*h) for h in range(1, 11))
        samples.append(v)
    norm = 12000 / max(1.0, max(abs(x) for x in samples))
    return [int(max(-32768, min(32767, x*norm))) for x in samples]


def drum_sample(index):
    rng = random.Random(300 + index)
    n = 6000
    decay = 5 + (index % 6) * 1.6
    phase = 0.0
    out = []
    for i in range(n):
        env = math.exp(-decay * i/n)
        phase += math.tau * (50 + 18*index) / RATE
        harmonic = math.sin(phase) * (0.75 if index%4==0 else 0.2)
        noise = rng.uniform(-1, 1)*(0.6 if index%4 else 0.13)
        out.append(int(max(-32768,min(32767,(harmonic+noise)*env*12500))))
    return out


def main():
    sample_data = bytearray()
    headers = []
    voices = []
    for idx in range(21+18):
        pcm = melodic_sample(idx) if idx < 21 else drum_sample(idx - 21)
        start = len(sample_data)//2
        sample_data += struct.pack("<"+"h"*len(pcm), *pcm)
        end = len(sample_data)//2
        sample_data += b"\0"*92  # 46 padding samples required by SF2 spec
        is_drum = idx >= 21
        headers.append((f"voice{idx:02d}", start, end,
                        start, end if is_drum else end,
                        RATE, 60 if not is_drum else DRUM_KEYS[idx-21]))
        voices.append(idx)

    info = group(b"INFO",
        chunk(b"ifil", struct.pack("<HH", 2, 1)),
        chunk(b"INAM", b"xyo-basic CC0\0"),
        chunk(b"ISFT", b"xyo generator\0"),
        chunk(b"ICOP", b"CC0 1.0; original synthesized samples\0"))
    sdta = group(b"sdta", chunk(b"smpl", sample_data))

    phdr, pbag, pgen, inst, ibag, igen = [], [], [], [], [], []
    # 21 melodic GM presets, each with a dedicated melodic instrument zone.
    for i, program in enumerate(GM_PROGRAMS):
        phdr.append((f"Preset{i+1}", program, 0, len(pbag)))
        pbag.append((len(pgen), 0))
        pgen.append((41, i))
    # Drum bank 128 / preset zero points at a multi-zone percussion instrument.
    phdr.append(("Drums", 0, 128, len(pbag)))
    pbag.append((len(pgen), 0)); pgen.append((41, 21))
    phdr.append(("EOP", 0, 0, len(pbag)))
    pbag.append((len(pgen), 0))

    for i in range(21):
        inst.append((f"Inst{i+1}",len(ibag)))
        ibag.append((len(igen), 0))
        igen.extend([(54,1),(53,i)])
    inst.append(("Percussion",len(ibag)))
    for j, key in enumerate(DRUM_KEYS):
        ibag.append((len(igen),0))
        igen.extend([(43, key | (key << 8)),(54,0),(53,21+j)])
    inst.append(("EOI",len(ibag)))
    ibag.append((len(igen),0))

    phdr_data = b"".join(name_bytes(n) + struct.pack("<HHHIII",p,b,bag,0,0,0)
                          for n,p,b,bag in phdr)
    pbag_data = b"".join(struct.pack("<HH",*v) for v in pbag)
    pgen_data = b"".join(generator(*v) for v in pgen)
    inst_data = b"".join(name_bytes(n)+struct.pack("<H",bag)
                         for n,bag in inst)
    ibag_data = b"".join(struct.pack("<HH",*v) for v in ibag)
    igen_data = b"".join(generator(*v) for v in igen)
    # 46-byte shdr sentinel: all-zero offsets and sound parameters.
    shdr = b"".join(name_bytes(n) +
                   struct.pack("<IIIII BbHH",start,end,ls,le,rate,key,0,0,1)
                   for n,start,end,ls,le,rate,key in headers)
    shdr += name_bytes("EOS") + struct.pack("<IIIII BbHH",0,0,0,0,RATE,0,0,0,1)
    pdta = group(b"pdta",chunk(b"phdr",phdr_data),chunk(b"pbag",pbag_data),
                 chunk(b"pmod",b"\0"*10),chunk(b"pgen",pgen_data),
                 chunk(b"inst",inst_data),chunk(b"ibag",ibag_data),
                 chunk(b"imod",b"\0"*10),chunk(b"igen",igen_data),
                 chunk(b"shdr",shdr))
    data = b"".join((info,sdta,pdta))
    OUT.parent.mkdir(parents=True,exist_ok=True)
    OUT.write_bytes(b"RIFF"+struct.pack("<I",4+len(data))+b"sfbk"+data)
    print(OUT,OUT.stat().st_size,"bytes",len(headers),"samples")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Skate 3 DLC bank (.vlt/.bin/.opt) decoder — current state.

STATUS (2026-09-23, UTC): the full decode is NOT implemented yet. This script
implements what IS verified about the encoded-bank format and provides the
diagnostic/crib machinery that the decode pass builds on:

  1. Granule view: bank = N*16 bytes; granule = [8B payload][8B trailer].
  2. Tag discovery: bytes [0..1] of each granule decompose EXACTLY as
     tag = (m << 5) | s   ->   byte0 = m >> 3,  byte1 = ((m & 7) << 5) | s
     with m an 11-bit field saturating at 2047 across the corpus, s 5 bits.
     Verified on 40 sample banks (see findings.md).
  3. Trailer families per bank: ff ff 00 00 00 00 00 00 / fe fe 00 00 ... / 00...
  4. Known-plaintext anchor: plaintext content flows continuously across
     granule boundaries in some banks (ots_dwmc_01_ol.vlt from 0x300 on) —
     the granule structure exists only in the encoded regions.

Next step for a full decode: find the game function that computes/uses the
(m,s) tag (search disasm for the 16-bit tag writer: sthu with (m<<5)|s, or
stores of byte pairs from an 11-bit index) and its inverse loop; then place
the 6 payload bytes into the plaintext stream per the index.
"""
import sys, struct, glob, collections
from pathlib import Path

def granules(d):
    return [d[i:i+16] for i in range(0, len(d) - len(d) % 16, 16)]

def tag_ms(g):
    v = (g[0] << 8) | g[1]
    return v >> 5, v & 31

def analyze(path):
    d = open(path, 'rb').read()
    n = len(d) // 16
    ms = []
    tails = collections.Counter()
    tags = collections.Counter()
    for i in range(n):
        g = d[i*16:(i+1)*16]
        m, s = tag_ms(g)
        ms.append((m, s))
        tags[(g[0], g[1])] += 1
        tails[g[8:16]] += 1
    return dict(path=path, size=len(d), granules=n, ms=ms, tags=tags, tails=tails)

def ascii_runs(d, minlen=6):
    import re
    return [(hex(m.start()), m.group()) for m in
            __import__('re').finditer(rb'[ -~]{%d,}' % minlen, d)]

def chunk_walk(d):
    """Oracle: walk [u32BE type][u32BE size][payload]; returns list of chunks
    or raises. Types must be the 7 Forge magics; must end at EndC."""
    MAG = {0x56657273:'Vers',0x5374724e:'StrN',0x4465704e:'DepN',0x4461744e:'DatN',
           0x5074724e:'PtrN',0x4578704e:'ExpN',0x456e6443:'EndC',0x5369676e:'Sign'}
    o, out = 0, []
    while o + 8 <= len(d):
        t, sz = struct.unpack_from('>II', d, o)
        if t not in MAG:
            return out, f"bad magic {t:08x} at {o:#x}"
        out.append((MAG[t], sz, o))
        o += 8 + sz
    if out and out[-1][0] == 'EndC' and o == len(d):
        return out, 'OK'
    return out, f'ended at {o:#x} of {len(d):#x}'

def main():
    pat = sys.argv[1] if len(sys.argv) > 1 else \
        str(Path(__file__).resolve().parents[2] / "analysis/dlc/dbs/maloof/db/challenge_local_data/otl_mmcn_01.vlt")
    for f in glob.glob(pat):
        r = analyze(f)
        print(f"== {r['path'].split('/')[-1]}: {r['size']}B, {r['granules']} granules")
        print("   top tags:", [(f'{k[0]:02x}{k[1]:02x}', c) for k, c in r['tags'].most_common(8)])
        print("   tails:", [(t.hex(), c) for t, c in r['tails'].most_common(5)])
        m_max = max(m for m, _ in r['ms'])
        print(f"   m range: {min(m for m,_ in r['ms'])}..{m_max}  (11-bit max=2047)")

if __name__ == '__main__':
    main()


# ---------------------------------------------------------------------------
# PASS-3 additions: the verified codec (game's RefPack variant at 0x82A5E038)
# PASS-6 re-transcription from RAW instruction words (field-accurate):
#   2-byte   (b0<0x80):       proc=b0&3;  len=((b0>>2)&7)+3;
#                             dis=(((b0>>5)&3)<<8)+b1+1;  order [b0][b1][lits][copy]
#   3-byte   (b0&0xC0==0x80): proc=b1>>6; len=(b0&0x3F)+4;
#                             dis=((b1&0x3F)<<8)+b2+1;    order [b0][b1][b2][lits][copy]
#   4-byte   (b0&0xE0==0xC0): proc=b0&3; len=(((b0>>2)&3)<<8)+b3+5;
#                             dis=(((b0>>4)&1)<<16)+(b1<<8)+b2+1; [b0][b1][b2][b3][lits]
#   literal/STOP (b0>=0xE0):  count=((b0&0x1F)+1)*4; >0x70 -> STOP with (b0&3) tail.
# Header: sig16; L=sig&0x8000 (usize 4|3 bytes); C=sig&0x100 (skip csize 4|3);
#         usize next 4|3 bytes BE; commands follow.
# ---------------------------------------------------------------------------

def game_refpack_decode(src, prime=b'', max_out=1<<20):
    """Exact transcription of rw::core::codec::RefpackInflate::Decompress
    (default.xex 0x82A5E038). src = RefPack-variant byte stream.
    prime = bytes pre-seeding the LZ window (the previously decoded bank/pool)."""
    src = src + b'\x00' * 8   # guard tail reads; n is real length
    out = bytearray(prime)
    base = len(prime)
    i = 0
    n = len(src) - 8
    status = 'eof'
    usize = 0
    if n >= 2:
        sig = (src[0] << 8) | src[1]
        i = 2
        if sig & 0x100:
            i += 4 if (sig & 0x8000) else 3   # skip compressed-size field
        us_len = (4 if (sig & 0x8000) else 3)
        usize = int.from_bytes(src[i:i+us_len], 'big')
        i += us_len
    while i < n:
        b0 = src[i]; i += 1
        if b0 < 0x80:
            # 2-byte copy
            b1 = src[i]; i += 1
            proc = b0 & 3
            out += src[i:i+proc]; i += proc
            ref_len = ((b0 >> 2) & 7) + 3
            ref_dis = (((b0 >> 5) & 3) << 8) + b1 + 1
            p = len(out) - ref_dis
            if p < 0:
                status = f'bad dis {ref_dis} (out={len(out)-base}) at {i-2}'
                return bytes(out), status
            for k in range(ref_len):
                out.append(out[p]); p += 1
        elif b0 < 0xC0:
            # 3-byte copy
            b1 = src[i]; b2 = src[i+1]; i += 2
            proc = b1 >> 6
            out += src[i:i+proc]; i += proc
            ref_len = (b0 & 0x3F) + 4
            ref_dis = ((b1 & 0x3F) << 8) + b2 + 1
            p = len(out) - ref_dis
            if p < 0:
                status = f'bad dis {ref_dis} at {i-2}'
                return bytes(out), status
            for k in range(ref_len):
                out.append(out[p]); p += 1
        elif b0 < 0xE0:
            # 4-byte copy
            b1 = src[i]; b2 = src[i+1]; b3 = src[i+2]; i += 3
            proc = b0 & 3
            out += src[i:i+proc]; i += proc
            ref_len = (((b0 >> 2) & 3) << 8) + b3 + 5
            ref_dis = (((b0 >> 4) & 1) << 16) + (b1 << 8) + b2 + 1
            p = len(out) - ref_dis
            if p < 0:
                status = f'bad dis {ref_dis}'
                return bytes(out), status
            for k in range(ref_len):
                out.append(out[p]); p += 1
        else:
            # literal run / STOP
            lit = ((b0 & 0x1F) + 1) * 4
            if lit <= 0x70:
                out += src[i:i+lit]; i += lit
            else:
                tail = b0 & 3
                out += src[i:i+tail]; i += tail
                status = 'STOP'
                break
    return bytes(out), status

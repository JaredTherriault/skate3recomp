"""Hashes used by Skate 3 data files.

vlt64     : EA Attrib 64-bit key hash = Bob Jenkins lookup8 (hash2 variant),
            a = b = 0xABCDEF0011223344, c = 0x9E3779B97F4A7C13 (golden ratio).
            Verified against stock keys: 'ots_univ_02' -> 6a1e2626030ca22e,
            'challenge_local_data' -> 46a56addf284c381, 'own_the_spots' ->
            bf65ab4c87f0a118, 'KilledItScore' -> 32e098540a7efcad,
            'Attrib::RefSpec' -> 97c5113de9bd1ad6, DepN 'ots_univ_02.vlt'/.bin.
            Export ids = vlt64('<class>/<collection>'), export type =
            vlt64('Attrib::CollectionLoadData').
djb2      : .big TOC hash1 = djb2(full path), seed 5381.
loc_hash  : language .BIN row key = djb2 variant seeded 0xFFFFFFFF.
"""

M64 = (1 << 64) - 1


def _mix64(a, b, c):
    a = (a - b - c) & M64; a ^= c >> 43
    b = (b - c - a) & M64; b ^= (a << 9) & M64
    c = (c - a - b) & M64; c ^= b >> 8
    a = (a - b - c) & M64; a ^= c >> 38
    b = (b - c - a) & M64; b ^= (a << 23) & M64
    c = (c - a - b) & M64; c ^= b >> 5
    a = (a - b - c) & M64; a ^= c >> 35
    b = (b - c - a) & M64; b ^= (a << 49) & M64
    c = (c - a - b) & M64; c ^= b >> 11
    a = (a - b - c) & M64; a ^= c >> 12
    b = (b - c - a) & M64; b ^= (a << 18) & M64
    c = (c - a - b) & M64; c ^= b >> 22
    return a, b, c


def vlt64(s):
    k = s.encode('latin1') if isinstance(s, str) else bytes(s)
    n = len(k)
    a = b = 0xABCDEF0011223344
    c = 0x9E3779B97F4A7C13
    o = 0
    while n - o >= 24:
        a = (a + int.from_bytes(k[o:o + 8], 'little')) & M64
        b = (b + int.from_bytes(k[o + 8:o + 16], 'little')) & M64
        c = (c + int.from_bytes(k[o + 16:o + 24], 'little')) & M64
        a, b, c = _mix64(a, b, c)
        o += 24
    c = (c + n) & M64
    for i, byte in enumerate(k[o:]):
        if i < 8:
            a = (a + (byte << (8 * i))) & M64
        elif i < 16:
            b = (b + (byte << (8 * (i - 8)))) & M64
        else:
            c = (c + (byte << (8 * (i - 15)))) & M64  # c's low byte = length
    return _mix64(a, b, c)[2]


def djb2(s, seed=5381):
    h = seed
    for ch in s.encode('latin1'):
        h = (h * 33 + ch) & 0xFFFFFFFF
    return h


def loc_hash(s):
    return djb2(s, 0xFFFFFFFF)


def _selftest():
    assert vlt64('ots_univ_02') == 0x6a1e2626030ca22e
    assert vlt64('challenge_local_data') == 0x46a56addf284c381
    assert vlt64('own_the_spots') == 0xbf65ab4c87f0a118
    assert vlt64('KilledItScore') == 0x32e098540a7efcad
    assert vlt64('Attrib::RefSpec') == 0x97c5113de9bd1ad6
    assert vlt64('ots_univ_02.vlt') == 0xe91f9ff74a37be14
    assert vlt64('challenges/ots_univ_02') == 0x95d560ab4c347d64
    assert vlt64('Attrib::CollectionLoadData') == 0xad303b8f42b3307e
    assert loc_hash('ID_CHALLENGE_OTS_UNIV_15_TITLE') == 0xAD1C2A6A


_selftest()

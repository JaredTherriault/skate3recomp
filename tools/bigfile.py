"""Read / rewrite Skate 3 EB v3 .big archives (the base-game layout: fat = 0).

Layout (verified against stock db.big / missions.big / miscboot.big):
  0x00 header (0x30): >HHIHBBIIBBHQI6x
       sig 'EB', ver 3, file_count, flags 0x10, align 6, res, names_off,
       names_size, fn_len, fo_len, folder_count, archive_size, fat(=0)
  0x30 TOC: file_count x [u32 off>>align][u32 csize][u32 size][u32 hash1]
       ** sorted ascending by hash1 = djb2(full path) — the game binary-searches **
  then file_count ctype bytes (1 = RefPack-compressed, csize != 0; 0 = raw, csize = 0)
  names_off = align16(0x30 + 17*file_count)
  names region: file_count x fn_len slots [u16 BE folder index][name NUL-padded],
                then folder_count x fo_len slots (EA tool quirk: fixed prefix of zero
                bytes, long strings spill into the next slot)
  names_size = align16(raw names bytes) + 16
  payload starts at align64(names_off + names_size); each payload 64-aligned.
"""
import struct
from pathlib import Path
from s3hash import djb2

HDR = ">HHIHBBIIBBHQI6x"


DELTA_MAGIC = 0x5333444C  # 'S3DL'
DELTA_VERSION = 1
DELTA_HDR = '<IIQQII64s'
DELTA_CHUNK_HDR = '<QII'


class Entry:
    __slots__ = ('path', 'folder', 'name', 'data', 'csize', 'size', 'ctype', 'order',
                 'fidx', 'hash1', 'is_stock', 'modified')

    def __init__(self, folder, name, data, csize, size, ctype, order, is_stock=True, modified=False):
        self.folder, self.name, self.data = folder, name, data
        self.csize, self.size, self.ctype, self.order = csize, size, ctype, order
        self.is_stock, self.modified = is_stock, modified
        self.path = name if folder in ('', '.') else folder + '/' + name


class Big:
    pass


def read(path, known_folders=()):
    b = Big.__new__(Big)
    b.path = Path(path)
    b.known_folders = list(known_folders)
    src = Path(path).read_bytes()
    b.raw_header = bytes(src[:0x30])
    h = struct.unpack_from(HDR, src, 0)
    (b.sig, b.ver, fa, b.flags, b.align, b.res, names_off, names_size,
     b.fn_len, b.fo_len, nfold, asize, fat) = h
    assert b.sig == 0x4542 and b.ver == 3, h
    b.fat = fat
    if fat > 0:
        b.fat_region = bytes(src[names_off + names_size: names_off + names_size + fat])
    else:
        b.fat_region = b''
    ctypes = src[0x30 + fa * 16: 0x30 + fa * 17]
    fstart = names_off + fa * b.fn_len
    b.folder_region = bytearray(src[fstart: fstart + nfold * b.fo_len])
    tail = src[fstart + nfold * b.fo_len: names_off + names_size]
    b.folder_spill = tail[:tail.find(b'\0')] if b'\0' in tail else b''
    slot0 = b.folder_region[:b.fo_len]
    b.prefix = 0
    while b.prefix < len(slot0) and slot0[b.prefix] == 0:
        b.prefix += 1
    b.folders = []
    for i in range(nfold):
        s = bytes(b.folder_region[i * b.fo_len + b.prefix: (i + 1) * b.fo_len])
        b.folders.append(s.split(b'\0')[0].decode('latin1'))
    b.entries = []
    for i in range(fa):
        off, csize, size, h1 = struct.unpack_from('>IIII', src, 0x30 + 16 * i)
        slot = src[names_off + i * b.fn_len: names_off + (i + 1) * b.fn_len]
        fidx = struct.unpack_from('>H', slot, 0)[0]
        name = slot[2:].split(b'\0')[0].decode('latin1')
        off <<= b.align
        data = src[off: off + (csize or size)]
        folder = b.folders[fidx] if fidx < nfold else ''
        if djb2(folder + '/' + name) != h1:
            ok = False
            for full in b.known_folders:
                if full.startswith(folder) and djb2(full + '/' + name) == h1:
                    folder = full; ok = True; break
            if not ok and djb2(name) == h1:
                folder = '.'; ok = True
            if not ok:
                folder = '?' + folder
        e = Entry(folder, name, data, csize, size, ctypes[i], off)
        e.fidx = fidx
        e.hash1 = h1
        b.entries.append(e)
    return b


def entry_bytes(e):
    """Decoded payload."""
    if e.csize:
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'xex'))
        from decrypt import game_refpack_decode as gdec
        dec, st = gdec(e.data)
        assert st == 'STOP' and len(dec) == e.size, (e.path, st, len(dec), e.size)
        return dec
    return e.data


def find(b, path):
    for e in b.entries:
        if e.path == path:
            return e
    return None


def replace_raw(b, path, data):
    e = find(b, path)
    assert e is not None, path
    e.data, e.csize, e.size, e.ctype = bytes(data), 0, len(data), 0
    e.modified = True


def add_raw(b, folder, name, data):
    assert find(b, folder + '/' + name) is None, folder + '/' + name
    if folder in b.folders:
        fidx = b.folders.index(folder)
    else:
        fidx = None
        for i, f in enumerate(b.folders):
            if folder.startswith(f) and len(f) >= b.fo_len - b.prefix - 1 and \
                    any(x.folder == folder for x in b.entries if x.fidx == i):
                fidx = i
        if fidx is None:
            fidx = len(b.folders)
            b.folders.append(folder)
            slot = bytearray(b.fo_len)
            s = folder.encode('latin1')[: b.fo_len - b.prefix]
            slot[b.prefix: b.prefix + len(s)] = s
            b.folder_region += slot
            b.folder_spill = b''
    e = Entry(folder, name, bytes(data), 0, len(data), 0, 1 << 62, is_stock=False, modified=True)
    e.fidx = fidx
    e.hash1 = djb2(folder + '/' + name)
    b.entries.append(e)
    return e


def write(b, path):
    ents = sorted(b.entries, key=lambda e: djb2(e.path) if not e.folder.startswith('?') else e.hash1)
    for e in ents:
        h = djb2(e.path) if not e.folder.startswith('?') else e.hash1
        e.hash1 = h
    hs = [e.hash1 for e in ents]
    assert len(set(hs)) == len(hs), 'hash1 collision'
    fa = len(ents)
    fn_len = max(b.fn_len, max(len(e.name.encode('latin1')) + 3 for e in ents))
    names = bytearray()
    for e in ents:
        slot = bytearray(fn_len)
        struct.pack_into('>H', slot, 0, e.fidx)
        s = e.name.encode('latin1')
        slot[2:2 + len(s)] = s
        names += slot
    raw_names = names + b.folder_region + b.folder_spill
    names_off = (0x30 + 17 * fa + 0xF) & ~0xF
    names_size = ((len(names) + len(b.folder_region) + 0xF) & ~0xF) + 16
    fat_size = getattr(b, 'fat', 0)
    fat_region = getattr(b, 'fat_region', b'')
    payload_off = (names_off + names_size + fat_size + 0x3F) & ~0x3F
    body = bytearray()
    offs = {}
    cur = payload_off
    for e in sorted(ents, key=lambda e: e.order):
        offs[id(e)] = cur
        body += e.data
        pad = (-len(e.data)) % 64
        body += b'\0' * pad
        cur += len(e.data) + pad
    out = bytearray(b.raw_header)
    struct.pack_into('>I', out, 0x04, fa)
    struct.pack_into('>I', out, 0x0C, names_off)
    struct.pack_into('>I', out, 0x10, names_size)
    struct.pack_into('>B', out, 0x14, fn_len)
    struct.pack_into('>B', out, 0x15, b.fo_len)
    struct.pack_into('>H', out, 0x16, len(b.folders))
    struct.pack_into('>Q', out, 0x18, cur)
    struct.pack_into('>I', out, 0x20, fat_size)
    for e in ents:
        out += struct.pack('>IIII', offs[id(e)] >> b.align, e.csize, e.size, e.hash1)
    out += bytes(e.ctype for e in ents)
    out += b'\0' * (names_off - len(out))
    out += raw_names
    out += b'\0' * (names_off + names_size - len(out))
    if fat_size > 0:
        out += fat_region
    out += b'\0' * (payload_off - len(out))
    assert len(out) == payload_off
    out += body
    assert len(out) == cur
    Path(path).write_bytes(out)
    return len(out)


def write_delta(b, delta_path, stock_path=None):
    if stock_path is None:
        stock_path = getattr(b, 'path', None)
    assert stock_path is not None, "Stock path required for write_delta"
    stock_path = Path(stock_path)
    stock_size = stock_path.stat().st_size
    base_name = stock_path.name.encode('latin1')[:63]

    ents = sorted(b.entries, key=lambda e: djb2(e.path) if not e.folder.startswith('?') else e.hash1)
    for e in ents:
        e.hash1 = djb2(e.path) if not e.folder.startswith('?') else e.hash1
    hs = [e.hash1 for e in ents]
    assert len(set(hs)) == len(hs), 'hash1 collision'
    fa = len(ents)
    fn_len = max(b.fn_len, max(len(e.name.encode('latin1')) + 3 for e in ents))
    names = bytearray()
    for e in ents:
        slot = bytearray(fn_len)
        struct.pack_into('>H', slot, 0, e.fidx)
        s = e.name.encode('latin1')
        slot[2:2 + len(s)] = s
        names += slot
    raw_names = names + b.folder_region + b.folder_spill
    names_off = (0x30 + 17 * fa + 0xF) & ~0xF
    names_size = ((len(names) + len(b.folder_region) + 0xF) & ~0xF) + 16
    fat_size = getattr(b, 'fat', 0)
    fat_region = getattr(b, 'fat_region', b'')
    new_names_end = (names_off + names_size + fat_size + 0x3F) & ~0x3F

    offs = {}
    appended_body = bytearray()
    cur = (stock_size + 0x3F) & ~0x3F
    if cur > stock_size:
        appended_body += b'\0' * (cur - stock_size)

    for e in ents:
        needs_append = (not getattr(e, 'is_stock', True)) or getattr(e, 'modified', False) or (e.order < new_names_end)
        if needs_append:
            offs[id(e)] = cur
            appended_body += e.data
            pad = (-len(e.data)) % 64
            appended_body += b'\0' * pad
            cur += len(e.data) + pad
        else:
            offs[id(e)] = e.order

    virtual_size = cur

    # Chunk 0: Header, TOC, ctypes, names, fat
    chunk0 = bytearray(b.raw_header)
    struct.pack_into('>I', chunk0, 0x04, fa)
    struct.pack_into('>I', chunk0, 0x0C, names_off)
    struct.pack_into('>I', chunk0, 0x10, names_size)
    struct.pack_into('>B', chunk0, 0x14, fn_len)
    struct.pack_into('>B', chunk0, 0x15, b.fo_len)
    struct.pack_into('>H', chunk0, 0x16, len(b.folders))
    struct.pack_into('>Q', chunk0, 0x18, virtual_size)
    struct.pack_into('>I', chunk0, 0x20, fat_size)
    for e in ents:
        chunk0 += struct.pack('>IIII', offs[id(e)] >> b.align, e.csize, e.size, e.hash1)
    chunk0 += bytes(e.ctype for e in ents)
    chunk0 += b'\0' * (names_off - len(chunk0))
    chunk0 += raw_names
    chunk0 += b'\0' * (names_off + names_size - len(chunk0))
    if fat_size > 0:
        chunk0 += fat_region
    chunk0 += b'\0' * (new_names_end - len(chunk0))
    assert len(chunk0) == new_names_end

    chunks = [(0, chunk0)]
    if len(appended_body) > 0:
        chunks.append((stock_size, bytes(appended_body)))

    out = bytearray()
    hdr = struct.pack(DELTA_HDR, DELTA_MAGIC, DELTA_VERSION, virtual_size, stock_size, len(chunks), 0, base_name)
    out += hdr
    for c_off, c_data in chunks:
        out += struct.pack(DELTA_CHUNK_HDR, c_off, len(c_data), 0)
        out += c_data

    Path(delta_path).write_bytes(out)
    return len(out)

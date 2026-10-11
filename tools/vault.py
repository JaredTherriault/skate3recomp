"""Skate 3 Attrib vault (.vlt + .bin) parsing and collection cloning.

.vlt chunks (BE): Vers[id8] StrN DepN DatN ExpN PtrN EndC, every chunk 16-aligned.
  DepN : u64 count, count x u64 vlt64(filename), count x u32 name offset, names
  DatN : 8 pad bytes, then the collection blocks back to back (each 16*k bytes,
         starting at file offset = 8 mod 16), then 8 pad bytes.
  collection block: u64 key, u64 class, u64 parent, u32 reserve, u32 shift(?),
         u32 entry_count, u16 numTypes, u16 tlen, u64 layout ptr (relocated),
         tlen x u64 type hashes, entry_count x [u64 attr key][u32 data][u16 type
         index][u8 nodeflags][u8 entryflags]
  ExpN : u32 0, u32 count, count x [u64 id = vlt64(class/name)][u64 type =
         vlt64('Attrib::CollectionLoadData')][u32 bytes][u32 file offset]
  PtrN : rows [u32 fixup][u16 type][u16 idx][u64 dest]
         type 2 -> switch: following fixups are offsets inside dep idx
                   (1 = the .bin, 0 = the .vlt)
         type 3 -> pointer: write address of (dep idx file + dest) at fixup
         type 0 -> end
.bin : StrE chunk (strings) + EndC, then a raw data region to EOF.
"""
import struct
from s3hash import vlt64

EXPORT_TYPE = vlt64('Attrib::CollectionLoadData')


def chunks(buf):
    out = []
    off = 0
    while off + 8 <= len(buf):
        nm = buf[off:off + 4].decode('latin1')
        size = struct.unpack_from('>I', buf, off + 4)[0]
        out.append((nm, off, size))
        if nm == 'EndC' or size == 0:
            break
        off += size
    return out


def cstr(b, off):
    e = b.find(b'\0', off)
    return bytes(b[off:e])


class Vault:
    def __init__(self, vlt, binb):
        self.vlt = bytearray(vlt)
        self.bin = bytearray(binb)
        self.ch = {nm: (off, size) for nm, off, size in chunks(self.vlt)}
        bch = {nm: (off, size) for nm, off, size in chunks(self.bin)}
        self.str_end = bch['StrE'][0] + bch['StrE'][1]
        self.data_start = bch['EndC'][0] + bch['EndC'][1]
        off, size = self.ch['ExpN']
        cnt = struct.unpack_from('>I', self.vlt, off + 12)[0]
        self.exports = []
        for i in range(cnt):
            self.exports.append(list(struct.unpack_from('>QQII', self.vlt, off + 16 + 24 * i)))
        off, size = self.ch['PtrN']
        self.ptr_rows = []
        p = off + 8
        while True:
            row = struct.unpack_from('>IHHQ', self.vlt, p)
            self.ptr_rows.append(row)
            p += 16
            if row[1] == 0:
                break
        self.ptrn_tail = bytes(self.vlt[p: off + size])  # pad after the end row
        # split pointer rows by base
        self.ptrs = {0: [], 1: []}
        base = None
        for fix, ty, idx, dest in self.ptr_rows:
            if ty == 2:
                base = idx
            elif ty == 3:
                self.ptrs[base].append((fix, idx, dest))
        assert [r[1:3] for r in self.ptr_rows if r[1] == 2] == [(2, 1), (2, 0)], 'unexpected ptr block order'

    # ---------------------------------------------------------------- queries
    def export_by_id(self, eid):
        for e in self.exports:
            if e[0] == eid:
                return e
        return None

    def collection(self, off):
        key, cls, parent = struct.unpack_from('>QQQ', self.vlt, off)
        return key, cls, parent

    def block_starts(self):
        """All data-region pointer targets (block boundaries) in the .bin."""
        s = set()
        for base in (0, 1):
            for fix, idx, dest in self.ptrs[base]:
                if idx == 1 and dest >= self.data_start:
                    s.add(dest)
        s.add(len(self.bin))
        return sorted(s)

    # ---------------------------------------------------------------- serialize
    def serialize(self):
        """Rebuild the .vlt from self.vlt's head (Vers/StrN/DepN), the DatN payload
        (self.datn_payload if set), self.exports and self.ptrs."""
        v = self.vlt
        dat_off, dat_size = self.ch['DatN']
        head = bytes(v[:dat_off])
        payload = self.datn_payload if hasattr(self, 'datn_payload') else \
            bytes(v[dat_off + 8: dat_off + dat_size - 8])
        datn = b'DatN' + struct.pack('>I', 16 + len(payload)) + b'\0' * 4 + b'\0' * 0
        # DatN: 'DatN' + size + (pad to 8 mod 16 start) ; stock = header(8) payload pad(8)
        datn = b'DatN' + struct.pack('>I', 8 + len(payload) + 8) + payload + b'\0' * 8
        assert (len(head) + len(datn)) % 16 == 0
        expn = bytearray(b'ExpN' + b'\0\0\0\0' + struct.pack('>II', 0, len(self.exports)))
        for e in self.exports:
            expn += struct.pack('>QQII', *e)
        expn += b'\0' * ((-len(expn)) % 16)
        struct.pack_into('>I', expn, 4, len(expn))
        ptrn = bytearray(b'PtrN\0\0\0\0')
        ptrn += struct.pack('>IHHQ', 0, 2, 1, 0)
        for fix, idx, dest in self.ptrs[1]:
            ptrn += struct.pack('>IHHQ', fix, 3, idx, dest)
        ptrn += struct.pack('>IHHQ', 0, 2, 0, 0)
        for fix, idx, dest in self.ptrs[0]:
            ptrn += struct.pack('>IHHQ', fix, 3, idx, dest)
        ptrn += struct.pack('>IHHQ', 0, 0, 0, 0)
        ptrn += self.ptrn_tail
        struct.pack_into('>I', ptrn, 4, len(ptrn))
        endc = b'EndC' + struct.pack('>I', 16) + b'\0' * 8
        return head + datn + bytes(expn) + bytes(ptrn) + endc


def replace_u64(buf, mapping, start=0, end=None, step=4):
    """Replace 8-byte BE values (aligned to `step`) in buf[start:end]."""
    end = len(buf) if end is None else end
    n = 0
    for o in range(start - (start % step), end - 7, step):
        if o < start:
            continue
        v = struct.unpack_from('>Q', buf, o)[0]
        if v in mapping:
            struct.pack_into('>Q', buf, o, mapping[v])
            n += 1
    return n


def entry_data_addr(vault, eoff, attr_key):
    """File offset (in the .vlt) of the data word of attribute attr_key inside
    the collection block at eoff, or None."""
    cnt = struct.unpack_from('>I', vault.vlt, eoff + 0x20)[0]
    tlen = struct.unpack_from('>H', vault.vlt, eoff + 0x26)[0]
    base = eoff + 0x30 + 8 * tlen
    for i in range(cnt):
        k = struct.unpack_from('>Q', vault.vlt, base + 16 * i)[0]
        if k == attr_key:
            return base + 16 * i + 8
    return None


def append_array(vault, cls, name, attr_key, new_elems):
    """Grow an array attribute ([u16 count][u16 cap][u16 stride][u16 0] + elems)
    by copying it to the end of the .bin with extra elements and re-pointing the
    collection's pointer row. Returns (old_dest, new_dest, new_count)."""
    v = vault
    exp = v.export_by_id(vlt64(cls + '/' + name))
    assert exp is not None, (cls, name)
    addr = entry_data_addr(v, exp[3], attr_key)
    assert addr is not None, 'attr not in collection'
    rows = [i for i, (fix, idx, dest) in enumerate(v.ptrs[0]) if fix == addr]
    assert len(rows) == 1, rows
    ri = rows[0]
    fix, idx, old = v.ptrs[0][ri]
    assert idx == 1
    cnt, cap, stride, z = struct.unpack_from('>HHHH', v.bin, old)
    end = old + 8 + cap * stride
    for f, i, d in v.ptrs[1]:
        assert not (old <= f < end), 'array has internal pointers'
    for e in new_elems:
        assert len(e) == stride
    elems = bytes(v.bin[old + 8: old + 8 + cnt * stride]) + b''.join(new_elems)
    n = cnt + len(new_elems)
    blk = struct.pack('>HHHH', n, n, stride, z) + elems
    pad = (old % 16 - len(v.bin)) % 16
    v.bin.extend(b'\0' * pad)
    new = len(v.bin)
    v.bin.extend(blk)
    v.bin.extend(b'\0' * ((-len(v.bin)) % 16))
    v.ptrs[0][ri] = (fix, idx, new)
    return old, new, n


def clone_collections(vault, specs, rename, key_map, str_fn):
    """Clone collections inside `vault` (in place: appends DatN blocks, exports,
    pointer rows and .bin data).

    specs   : list of (class_name, src_name, dst_name)
    rename  : {src_collection_name: dst_collection_name} for key/parent remap
    key_map : {old u64: new u64} applied to copied .bin data blocks (refspec keys,
              node ids)
    str_fn  : f(old_string_bytes) -> new bytes or None (keep the original string)
    Returns a report list.
    """
    v = vault
    dat_off, dat_size = v.ch['DatN']
    payload = bytearray(v.datn_payload if hasattr(v, 'datn_payload') else
                        v.vlt[dat_off + 8: dat_off + dat_size - 8])
    payload_base = dat_off + 8           # file offset of payload[0]
    starts = v.block_starts()
    all_rows0 = list(v.ptrs[0])
    new_bin = bytearray()
    bin_base = len(v.bin)
    bin_base += (-bin_base) % 16
    pad0 = bin_base - len(v.bin)
    str_cache = {}
    report = []
    new_rows0, new_rows1 = [], []

    def add_string(s):
        if s in str_cache:
            return str_cache[s]
        off = bin_base + len(new_bin)
        new_bin.extend(s + b'\0')
        str_cache[s] = off
        return off

    span_map = []   # (old_lo, old_hi, new_lo)

    def map_dest(dest):
        for lo, hi, nlo in span_map:
            if lo <= dest < hi:
                return nlo + (dest - lo)
        if dest < v.str_end:
            s = cstr(v.bin, dest)
            ns = str_fn(s)
            if ns is not None and ns != s:
                return add_string(ns)
        return dest

    for cls, src, dst in specs:
        eid = vlt64(cls + '/' + src)
        exp = v.export_by_id(eid)
        assert exp is not None, (cls, src)
        _, etype, ebytes, eoff = exp
        blk = bytearray(v.vlt[eoff: eoff + ebytes])
        key, ccls, parent = struct.unpack_from('>QQQ', blk, 0)
        assert key == vlt64(src) and ccls == vlt64(cls), (cls, src)
        struct.pack_into('>Q', blk, 0, vlt64(dst))
        for pname, nname in rename.items():
            if parent == vlt64(pname):
                struct.pack_into('>Q', blk, 16, vlt64(nname))
        # base-0 pointer rows inside this collection
        rows = [(fix, idx, dest) for fix, idx, dest in v.ptrs[0] if eoff <= fix < eoff + ebytes]
        # copy the .bin data span this collection owns
        dests = sorted({d for _, idx, d in rows if idx == 1 and d >= v.data_start})
        if dests:
            lo = dests[0]
            hi = next(s for s in starts if s > dests[-1])
            # closure: nested blocks reached only through bin-internal pointers
            changed = True
            while changed:
                changed = False
                for fix, idx, dest in v.ptrs[1]:
                    if lo <= fix < hi and idx == 1 and dest >= hi and dest < hi + 0x200:
                        hi = next(s for s in starts if s > dest); changed = True
            # ownership check: no other collection's vlt pointer may land in the span
            for fix, idx, dest in all_rows0:
                if idx == 1 and lo <= dest < hi and not (eoff <= fix < eoff + ebytes):
                    raise AssertionError('span %x..%x of %s/%s shared with vlt fixup %x' % (lo, hi, cls, src, fix))
            align = lo % 16
            cur = bin_base + len(new_bin)
            padn = (align - cur) % 16
            new_bin.extend(b'\0' * padn)
            nlo = bin_base + len(new_bin)
            chunk = bytearray(v.bin[lo:hi])
            nrep = replace_u64(chunk, key_map)
            new_bin.extend(chunk)
            span_map.append((lo, hi, nlo))
            # bin-internal pointers inside the span
            for fix, idx, dest in v.ptrs[1]:
                if lo <= fix < hi:
                    new_rows1.append((nlo + (fix - lo), idx, ('MAP', dest)))
            report.append('%s/%s -> %s : vlt %d B, bin span %x..%x -> %x (%d u64 remapped)'
                          % (cls, src, dst, ebytes, lo, hi, nlo, nrep))
        else:
            report.append('%s/%s -> %s : vlt %d B, no bin span' % (cls, src, dst, ebytes))
        new_off = payload_base + len(payload)
        assert new_off % 16 == 8
        payload.extend(blk)
        for fix, idx, dest in rows:
            new_rows0.append((new_off + (fix - eoff), idx, ('MAP', dest)))
        v.exports.append([vlt64(cls + '/' + dst), etype, ebytes, new_off])

    # resolve destinations now that all spans are known
    def resolve(rows):
        out = []
        for fix, idx, d in rows:
            dest = d[1]
            out.append((fix, idx, map_dest(dest) if idx == 1 else dest))
        return out
    r1 = resolve(new_rows1)
    r0 = resolve(new_rows0)
    v.ptrs[1].extend(sorted(r1))
    v.ptrs[0].extend(sorted(r0))
    v.datn_payload = bytes(payload)
    v.bin.extend(b'\0' * pad0)
    v.bin.extend(new_bin)
    v.bin.extend(b'\0' * ((-len(v.bin)) % 16))
    return report

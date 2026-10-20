"""NSO loader: decompress segments, build the memory image, parse MOD0/dynamic info."""
import hashlib
import struct
import sys
import lz4.block

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_JMPREL, DT_RELACOUNT = 10, 23, 0x6FFFFFF9
DT_SYMENT, DT_RELRSZ, DT_RELR, DT_RELRENT = 11, 35, 36, 37
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027


class Nso:
    def __init__(self, path):
        with open(path, 'rb') as f:
            if f.seek(0, 2) > 512 << 20:
                raise ValueError('NSO file exceeds 512 MiB')
            f.seek(0)
            raw = f.read()
        if len(raw) < 0x100 or raw[:4] != b'NSO0':
            raise ValueError('invalid NSO header')
        flags = struct.unpack_from('<I', raw, 0xC)[0]
        if flags & ~0x7f:
            raise ValueError('unsupported NSO flags or compression')
        self.build_id = raw[0x40:0x60].rstrip(b'\0').hex()
        segs = []
        for i in range(3):
            file_off, mem_off, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
            comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
            stored = comp_size if flags & (1 << i) else dec_size
            if (file_off < 0x100 or file_off + stored > len(raw)
                    or mem_off + dec_size > 512 << 20):
                raise ValueError('invalid NSO segment bounds')
            if segs and mem_off < segs[-1][0] + len(segs[-1][1]):
                raise ValueError('overlapping NSO segments')
            data = raw[file_off:file_off + stored]
            if flags & (1 << i):
                data = lz4.block.decompress(data, uncompressed_size=dec_size)
            if len(data) != dec_size:
                raise ValueError('NSO segment size mismatch')
            if flags & (8 << i) and hashlib.sha256(data).digest() != raw[0xa0 + i * 32:0xc0 + i * 32]:
                raise ValueError('NSO segment hash mismatch')
            segs.append((mem_off, data))
        self.bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
        self.text = (segs[0][0], len(segs[0][1]))
        self.rodata = (segs[1][0], len(segs[1][1]))
        self.data = (segs[2][0], len(segs[2][1]))
        end = segs[2][0] + len(segs[2][1]) + self.bss_size
        if end > 512 << 20:
            raise ValueError('NSO image exceeds 512 MiB')
        self.size = (end + 0xFFF) & ~0xFFF
        img = bytearray(self.size)
        for off, d in segs:
            img[off:off + len(d)] = d
        self.image = img
        self.dynsym_offset, self.dynsym_size = struct.unpack_from('<II', raw, 0x98)
        self._parse_mod0()

    def check_range(self, off, size):
        if off < 0 or size < 0 or off + size > len(self.image):
            raise ValueError('NSO metadata outside image')

    def u32(self, off):
        self.check_range(off, 4)
        return struct.unpack_from('<I', self.image, off)[0]

    def u64(self, off):
        self.check_range(off, 8)
        return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_mod0(self):
        mod0 = self.u32(4)
        self.check_range(mod0, 28)
        if self.image[mod0:mod0 + 4] != b'MOD0':
            raise ValueError('invalid MOD0 header')
        rel = lambda i: mod0 + struct.unpack_from('<i', self.image, mod0 + 4 + i * 4)[0]
        self.dynamic, self.bss_start, self.bss_end = rel(0), rel(1), rel(2)
        self.eh_start, self.eh_end, self.module_obj = rel(3), rel(4), rel(5)
        dyn = {}
        needed = []
        off = self.dynamic
        dynamic_end = next((start + size for start, size in (self.text, self.rodata, self.data)
                            if start <= off < start + size), 0)
        while True:
            if off + 16 > dynamic_end:
                raise ValueError('unterminated dynamic table')
            tag, val = self.u64(off), self.u64(off + 8)
            off += 16
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        strtab = dyn.get(DT_STRTAB, 0)
        self.strtab = strtab
        self.needed = [self.cstr(strtab + n) for n in needed]
        # Newer modules may only have GNU hash; NSO also records the dynsym extent.
        self.syms = []
        if DT_SYMTAB in dyn:
            if dyn.get(DT_SYMENT, 24) != 24:
                raise ValueError('invalid dynamic symbol entry size')
            if self.dynsym_size:
                if (self.dynsym_size % 24 or self.dynsym_offset + self.dynsym_size > self.rodata[1]
                        or self.rodata[0] + self.dynsym_offset != dyn[DT_SYMTAB]):
                    raise ValueError('invalid NSO dynamic symbol extent')
                nchain = self.dynsym_size // 24
                if DT_HASH in dyn and self.u32(dyn[DT_HASH] + 4) != nchain:
                    raise ValueError('inconsistent dynamic symbol count')
            elif DT_HASH in dyn:
                nchain = self.u32(dyn[DT_HASH] + 4)
            else:
                raise ValueError('missing dynamic symbol count')
            st = dyn[DT_SYMTAB]
            self.check_range(st, nchain * 24)
            for i in range(nchain):
                o = st + i * 24
                name, info, other, shndx = struct.unpack_from('<IBBH', self.image, o)
                value, size = struct.unpack_from('<QQ', self.image, o + 8)
                self.syms.append((self.cstr(strtab + name), info, shndx, value, size))
        self.relocs = []
        for base_tag, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if base_tag in dyn:
                if size_tag not in dyn or dyn[size_tag] % 24:
                    raise ValueError('invalid RELA table size')
                b, n = dyn[base_tag], dyn[size_tag] // 24
                self.check_range(b, n * 24)
                for i in range(n):
                    r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, b + i * 24)
                    self.relocs.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))
        self.relr_count = 0
        if any(tag in dyn for tag in (DT_RELR, DT_RELRSZ, DT_RELRENT)):
            if (not all(tag in dyn for tag in (DT_RELR, DT_RELRSZ, DT_RELRENT))
                    or dyn[DT_RELRENT] != 8 or dyn[DT_RELRSZ] % 8):
                raise ValueError('invalid RELR table')
            self.check_range(dyn[DT_RELR], dyn[DT_RELRSZ])
            cursor = None
            for pos in range(dyn[DT_RELR], dyn[DT_RELR] + dyn[DT_RELRSZ], 8):
                entry = self.u64(pos)
                if entry & 1:
                    if cursor is None:
                        raise ValueError('RELR bitmap before address')
                    offsets = (cursor + bit * 8 for bit in range(63) if entry & (2 << bit))
                    next_cursor = cursor + 63 * 8
                else:
                    offsets = (entry,)
                    next_cursor = entry + 8
                for offset in offsets:
                    # Normalize implicit addends so all function-discovery callers see them.
                    self.relocs.append((offset, R_AARCH64_RELATIVE, 0, self.u64(offset)))
                    self.relr_count += 1
                    if self.relr_count > len(self.image) // 8:
                        raise ValueError('too many RELR relocations')
                cursor = next_cursor

    def cstr(self, off):
        self.check_range(off, 1)
        end = self.image.index(b'\0', off)
        return self.image[off:end].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]


if __name__ == '__main__':
    from collections import Counter
    for p in sys.argv[1:]:
        n = Nso(p)
        print(f'== {p}  build {n.build_id[:16]}  image 0x{n.size:x}')
        print(f'  text 0x{n.text[0]:x}+0x{n.text[1]:x}  ro 0x{n.rodata[0]:x}+0x{n.rodata[1]:x}  '
              f'data 0x{n.data[0]:x}+0x{n.data[1]:x}  bss 0x{n.bss_size:x}')
        print(f'  needed {n.needed}  syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
        print(f'  relocs {dict(Counter(r[1] for r in n.relocs))}')

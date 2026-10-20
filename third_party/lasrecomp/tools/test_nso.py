"""NSO parsing regression tests, using only synthetic bytes."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

import lz4.block
from nso import Nso, R_AARCH64_RELATIVE


def fixture(*, compressed=False, hashed=False, legacy=False):
    text, ro, data = bytearray(0x100), bytearray(0x400), bytearray(0x400)
    struct.pack_into('<I', text, 4, 0x40)
    struct.pack_into('<4s6i', text, 0x40, b'MOD0', 0xc0, 0x8c0, 0x8c0, 0, 0, 0)
    # One address, a bitmap with its first/last bits, then a consecutive bitmap.
    tags = [(6, 0x200), (11, 24), (5, 0x230), (10, 8), (36, 0x280), (35, 24), (37, 8)]
    if legacy:
        tags.append((4, 0x250))
    else:
        tags.append((0x6ffffef5, 0x250))
    for i, pair in enumerate(tags):
        struct.pack_into('<QQ', ro, i * 16, *pair)
    struct.pack_into('<IBBHQQ', ro, 0x118, 1, 2, 0, 1, 0x80, 4)
    ro[0x130:0x138] = b'\0entry\0\0'
    struct.pack_into('<II', ro, 0x150, 1, 2)
    struct.pack_into('<3Q', ro, 0x180, 0x500, 3 | (1 << 63), 3)
    for off, value in ((0x500, 0x80), (0x508, 0x84), (0x6f8, 0x88), (0x700, 0x8c)):
        struct.pack_into('<Q', data, off - 0x500, value)
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    struct.pack_into('<I', header, 0xc, (7 if compressed else 0) | (0x38 if hashed else 0))
    if not legacy:
        struct.pack_into('<II', header, 0x98, 0x100, 48)
    payload = bytearray()
    for i, (off, segment) in enumerate(((0, text), (0x100, ro), (0x500, data))):
        stored = lz4.block.compress(segment, store_size=False) if compressed else segment
        struct.pack_into('<III', header, 0x10 + i * 16, 0x100 + len(payload), off, len(segment))
        struct.pack_into('<I', header, 0x60 + i * 4, len(stored) if compressed else 0)
        header[0xa0 + i * 32:0xc0 + i * 32] = hashlib.sha256(segment).digest()
        payload.extend(stored)
    return header + payload


class NsoTests(unittest.TestCase):
    def parse(self, raw):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.nso'
            path.write_bytes(raw)
            return Nso(path)

    def test_relr_and_gnu_only_symbols(self):
        n = self.parse(fixture())
        self.assertEqual(n.relocs, [(off, R_AARCH64_RELATIVE, 0, value) for off, value in
                                  ((0x500, 0x80), (0x508, 0x84), (0x6f8, 0x88), (0x700, 0x8c))])
        self.assertEqual(n.relr_count, 4)
        self.assertEqual(n.syms[1], ('entry', 2, 1, 0x80, 4))
        self.assertEqual(n.u64(0x500), 0x80)  # parsing must not relocate the image

    def test_compressed_and_hashed(self):
        self.assertEqual(self.parse(fixture(compressed=True, hashed=True)).relr_count, 4)

    def test_legacy_hash(self):
        self.assertEqual(len(self.parse(fixture(legacy=True)).syms), 2)

    def test_bad_inputs(self):
        mutations = [
            (0, '<4s', b'BAD!'), (0xc, '<I', 0x80), (0x18, '<I', 0x40000000),
            (0x24, '<I', 0), (0x3c, '<I', 0xffffffff),
            (0x9c, '<I', 47), (0x98, '<I', 0x101),
            (0x208, '<Q', 0xfffffff0),  # DT_SYMTAB outside image
            (0x218, '<Q', 16),          # DT_SYMENT
            (0x258, '<Q', 23),          # DT_RELRSZ
            (0x268, '<Q', 4),           # DT_RELRENT
            (0x144, '<i', -0x80),       # dynamic table before image
            (0x380, '<Q', 3),           # bitmap without anchor
            (0x380, '<Q', 0x1000),      # relocation outside image
        ]
        for off, fmt, value in mutations:
            with self.subTest(offset=off, value=value):
                raw = fixture()
                struct.pack_into(fmt, raw, off, value)
                with self.assertRaises(ValueError):
                    self.parse(raw)
        with self.assertRaises(ValueError):
            self.parse(fixture()[:100])
        with self.assertRaises(ValueError):
            self.parse(fixture()[:-1])

    def test_hash_mismatch(self):
        raw = fixture(hashed=True)
        raw[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.parse(raw)

    def test_unterminated_dynamic_table(self):
        raw = fixture()
        for off in range(0x200, len(raw), 16):
            struct.pack_into('<QQ', raw, off, 1, 1)
        with self.assertRaises(ValueError):
            self.parse(raw)

    def test_empty_bitmap_advances_cursor(self):
        raw = fixture()
        struct.pack_into('<Q', raw, 0x388, 1)
        n = self.parse(raw)
        self.assertEqual([r[0] for r in n.relocs], [0x500, 0x700])


if __name__ == '__main__':
    unittest.main()

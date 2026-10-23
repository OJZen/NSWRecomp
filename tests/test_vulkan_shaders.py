"""Synthetic shader containers and rejection boundaries; no captured game data."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from vulkan_shaders import decode


def shader(stage=5, version=15):
    offset = (0 if version == 15 else 0x100) if stage == 5 else 0x30
    control, instructions, constants = 0x800, b'\x00' * 16, b'\x12\x34\x56\x78'
    data = bytearray(control + offset + len(instructions) + len(constants))
    fields = {0: 0x98761234, 4: 1, 8: version, 0x20: control, 0x714: stage,
              0x708: offset, 0x700: offset + len(instructions), 0x6fc: len(constants),
              0x704: offset + len(instructions) + len(constants), 0x6f8: len(instructions),
              0x748: 4, 0x74c: 2, 0x750: 1, 0x754: 256}
    for at, value in fields.items(): struct.pack_into('<I', data, at, value)
    if offset: struct.pack_into('<I', data, control, 0x12345679 if stage == 5 else 0x12345678)
    data[control + offset:] = instructions + constants
    return data


class VulkanShaderTests(unittest.TestCase):
    def test_compute_headers_and_graphics_constant_extents(self):
        for stage in (0, 1, 5):
            for version in (9, 15, 16):
                with self.subTest(stage=stage, version=version):
                    kind, job, instructions = decode(shader(stage, version))
                    self.assertEqual((kind, job['c1'], instructions), (stage, '12345678', b'\x00' * 16))
                    if stage == 5:
                        self.assertEqual((job['local'], job['shared']), ([4, 2, 1], 256))

    def test_rejects_truncation_trailing_bytes_and_unsupported_header(self):
        valid = shader()
        for data in (valid[:0x20], valid[:0x748], valid[:-1], valid + b'\x00' * 4):
            with self.subTest(length=len(data)), self.assertRaises(ValueError): decode(data)
        for offset, value in ((0, 0), (4, 2), (8, 17), (0x714, 4), (0x708, 8), (0x20, 0x747),
                              (0x700, 32), (0x6fc, 32), (0x6f8, 24), (0x748, 0), (0x748, 2048), (0x754, 65536)):
            data = shader(); struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError): decode(data)

    def test_rejects_compute_launch_metadata_outside_control_record(self):
        data = shader()
        code = data[0x800:]
        data = data[:0x748] + code
        struct.pack_into('<I', data, 0x20, 0x748)
        with self.assertRaises(ValueError): decode(data)


if __name__ == '__main__': unittest.main()

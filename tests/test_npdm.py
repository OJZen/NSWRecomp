import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from npdm import access_policy, read_policy, service_names


def sac(names, server=False):
    return b''.join(bytes([(len(n) - 1) | (0x80 if server else 0)]) + n.encode() for n in names)


def fixture(clients=('lm', 'fsp-srv'), permitted=('*',), servers=()):
    requested = sac(clients) + sac(servers, True)
    allowed = sac(permitted) + sac(servers, True)
    aci, aci_size = 0x80, 0x40 + len(requested)
    acid = (aci + aci_size + 15) & ~15
    acid_size = 0x240 + len(allowed)
    data = bytearray(acid + acid_size)
    data[:4] = b'META'; data[0xc] = 7
    struct.pack_into('<4I', data, 0x70, aci, aci_size, acid, acid_size)
    data[aci:aci + 4] = b'ACI0'
    struct.pack_into('<II', data, aci + 0x28, 0x40, len(requested))
    data[aci + 0x40:aci + aci_size] = requested
    data[acid + 0x200:acid + 0x204] = b'ACID'
    struct.pack_into('<I', data, acid + 0x204, acid_size - 0x100)
    struct.pack_into('<II', data, acid + 0x228, 0x240, len(allowed))
    data[acid + 0x240:] = allowed
    return data


class NpdmTests(unittest.TestCase):
    def test_client_permissions_and_wildcards(self):
        self.assertEqual(access_policy(fixture(('lm', 'fsp-srv'), ('l*', 'fsp-*'), ('secret',))), ['lm', 'fsp-srv'])
        self.assertEqual(access_policy(fixture((), ())), [])
        self.assertEqual(service_names(sac(('lm', 'lm', '12345678'))), ['lm', '12345678'])
        with self.assertRaisesRegex(ValueError, 'exceeds'):
            access_policy(fixture(('secret',), ('lm',), ('secret',)))

    def test_truncation_and_region_bounds(self):
        data = fixture()
        for size in range(len(data)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                access_policy(data[:size])
        for offset, value in ((0x70, 0), (0x74, 0xffffffff), (0x78, 0x80),
                              (0x80 + 0x28, 0), (0x80 + 0x2c, 0xffffffff)):
            broken = data.copy(); struct.pack_into('<I', broken, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError): access_policy(broken)

    def test_unsupported_models_and_service_descriptors(self):
        data = fixture(); data[0xc] = 1
        with self.assertRaisesRegex(ValueError, 'address-space'): access_policy(data)
        for raw in (b'\x08x', b'\x07abc', b'\x00\0', sac(('a*b',)), sac(('a,b',)), sac(('x',)) * 129):
            with self.subTest(raw=raw[:8]), self.assertRaises(ValueError): service_names(raw)

    def test_explicit_fingerprint_and_file_size(self):
        with tempfile.TemporaryDirectory(prefix='npdm-test-') as directory:
            path = Path(directory) / 'main.npdm'
            data = fixture(); path.write_bytes(data)
            sha = hashlib.sha256(data).hexdigest()
            self.assertEqual(read_policy(path, sha.upper())['client_services'], ['lm', 'fsp-srv'])
            self.assertFalse(read_policy(path, sha)['signature_verified'])
            for wrong in (None, '', 'invalid', '0' * 64):
                with self.assertRaises(ValueError): read_policy(path, wrong)
            path.write_bytes(data + b'\0')
            with self.assertRaisesRegex(ValueError, 'fingerprint'): read_policy(path, sha)
            path.write_bytes(b'x' * (65536 + 1))
            with self.assertRaisesRegex(ValueError, '64 KiB'): read_policy(path, sha)

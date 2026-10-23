"""Read-only storage bounds, 64-bit offsets and verified Windows handle lifetime."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import resources
from host import executable


def fixture(path, size=0x20300):
    header = [0x50, 0x50, 4, 0x54, 24, 0x6c, 4, 0x70, 32, 0x200]
    with path.open('wb') as stream:
        if os.name == 'nt' and size > 1 << 32:
            import msvcrt
            from ctypes import wintypes as W
            control = ctypes.WinDLL('kernel32', use_last_error=True).DeviceIoControl
            control.argtypes = [W.HANDLE, W.DWORD, ctypes.c_void_p, W.DWORD,
                                ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD), ctypes.c_void_p]
            control.restype = W.BOOL
            returned = W.DWORD()
            if not control(msvcrt.get_osfhandle(stream.fileno()), 0x900c4, None, 0, None, 0, ctypes.byref(returned), None):
                raise ctypes.WinError(ctypes.get_last_error())
        stream.write(struct.pack('<10Q', *header))
        stream.seek(0x200)
        stream.write(bytes(range(256)) * 512)
        stream.seek(size - 64)
        stream.write(bytes(i & 255 for i in range(size - 64, size)))


class RomfsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='romfs-native-')
        cls.root = Path(cls.temporary.name)
        cls.binary = cls.root / executable('romfs')
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                        str(ROOT / 'runtime/romfs.c'), str(ROOT / 'tests/native_romfs.c'),
                        *(['-municode'] if os.name == 'nt' else []), '-o', str(cls.binary)], check=True, timeout=30)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_utf8_path_chunked_reads_and_invalid_tables(self):
        path = self.root / '资源 with spaces.bin'; fixture(path)
        env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1')
        env.pop('NSW_ROMFS_HANDLE', None); env.pop('NSW_TEST_HOLD', None)
        result = subprocess.run([str(self.binary), str(path), str(path.stat().st_size)], env=env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        for index, value in ((0, 0), (1, 0x40), (2, 1 << 63), (3, 0x50), (4, 23), (9, 1 << 63)):
            fixture(path)
            with path.open('r+b') as stream:
                stream.seek(index * 8); stream.write(struct.pack('<Q', value))
            result = subprocess.run([str(self.binary), str(path), 'reject'], env=env,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(self.binary), str(self.root), 'reject'], env=env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_sparse_file_offsets_above_four_gib(self):
        path = self.root / 'large.bin'; size = (1 << 32) + 0x301; fixture(path, size)
        result = subprocess.run([str(self.binary), str(path), str(size)], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        path.unlink()

    @unittest.skipUnless(os.name == 'nt', 'Windows share mode and explicit handle inheritance')
    def test_verified_handle_survives_path_changes_and_rejects_writes(self):
        path = self.root / 'locked.bin'; fixture(path)
        profile = self.root / 'profile.json'
        profile.write_text(json.dumps(dict(size=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest())))
        environment = dict(os.environ, NSW_TEST_HOLD='1', UBSAN_OPTIONS='halt_on_error=1')
        with patch.object(resources, 'ROOT', self.root), resources.verified_romfs(path, profile) as (record, stream):
            with resources.resource_child_options(stream, environment) as options:
                with subprocess.Popen([str(self.binary), 'deliberately-missing.bin', str(record['size'])],
                                      env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True, **options) as child:
                    try:
                        self.assertEqual(child.stdout.readline().strip(), 'ready')
                        with self.assertRaises(PermissionError): path.write_bytes(b'changed')
                        with self.assertRaises(PermissionError): path.rename(self.root / 'renamed.bin')
                    finally:
                        stdout, stderr = child.communicate('\n', timeout=15)
                    self.assertEqual(child.returncode, 0, stderr)
                    self.assertIn('RomFS storage checks passed', stdout)
        path.write_bytes(b'released')

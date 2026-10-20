import contextlib
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from extract_exefs import pfs_entries, LimitedReader
import extract_romfs


def package(name=b'main\0', offset=0, size=4):
    return struct.pack('<4sIIIQQII', b'PFS0', 1, len(name), 0, offset, size, 0, 0) + name + b'data'


class ImportTests(unittest.TestCase):
    def test_container_paths_bounds_and_truncation(self):
        blob = package()
        self.assertEqual(pfs_entries(io.BytesIO(blob), 0, len(blob)), {'main': (45, 4)})
        for blob in (package(b'../bad\0'), package(b'..\0'), package(b'a\\b\0'), package(b'not-terminated'),
                     package(offset=1), package(size=5), b'PFS0'):
            with self.assertRaises(ValueError): pfs_entries(io.BytesIO(blob), 0, len(blob))
        stream = LimitedReader(io.BytesIO(b'allowed-unrelated'), 7)
        self.assertEqual(stream.read(99), b'allowed'); self.assertEqual(stream.read(), b'')

    def test_keys_are_bounded_regular_inputs(self):
        with tempfile.TemporaryDirectory(prefix='import-keys-test-') as temp:
            path = Path(temp) / 'keys'
            path.write_text('header_key = ' + '00' * 32 + '\n')
            self.assertEqual(len(extract_romfs.read_keys(path)['header_key']), 32)
            for content in ('invalid', 'header_key = xyz', 'header_key = 00'):
                path.write_text(content)
                with self.assertRaises(ValueError): extract_romfs.read_keys(path)
            path.unlink(); os.mkfifo(path)
            with self.assertRaisesRegex(ValueError, 'regular'): extract_romfs.read_keys(path)

    def test_hactool_failure_withholds_raw_output_and_removes_private_temporaries(self):
        with tempfile.TemporaryDirectory(prefix='import-failure-test-') as temp:
            root = Path(temp); (root / 'work/tmp').mkdir(parents=True)
            plan = dict(nca_size=10, romfs_size=8, romfs_index=1, metadata_hash=False,
                        rights=bytes(16), encrypted_key=bytes(16), generation=0)
            captured = io.StringIO()
            with patch.object(extract_romfs, 'ROOT', root), \
                 patch.object(extract_romfs, 'read_keys', return_value={'header_key': bytes(32), 'titlekek_00': bytes(16)}), \
                 patch.object(extract_romfs, 'inspect_package', return_value=plan), \
                 patch.object(extract_romfs, 'restore_ncz', return_value='synthetic'), \
                 patch.object(extract_romfs.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stdout='private diagnostic marker')), \
                 patch.object(extract_romfs.shutil, 'disk_usage', return_value=SimpleNamespace(free=100 << 30)), \
                 contextlib.redirect_stdout(captured):
                with self.assertRaisesRegex(ValueError, 'raw output withheld'):
                    extract_romfs.extract(root / 'base', root / 'update', root / 'key', root / 'hactool', root / 'output.bin', {})
            self.assertNotIn('private diagnostic marker', captured.getvalue())
            self.assertEqual(list((root / 'work/tmp').iterdir()), [])
            self.assertFalse((root / 'output.bin').exists())

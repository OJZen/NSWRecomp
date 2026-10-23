import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import resources


class ResourceTests(unittest.TestCase):
    def test_explicit_profile_validation_and_verification_lifetime(self):
        with tempfile.TemporaryDirectory(prefix='resource-profile-') as temp:
            root = Path(temp); path = root / 'romfs.bin'; profile = root / 'profile.json'
            data = b'synthetic' * 100; path.write_bytes(data)
            good = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            with patch.object(resources, 'ROOT', root):
                for bad in ({}, dict(good, size=True), dict(good, size=1 << 40), dict(good, sha256='invalid')):
                    profile.write_text(json.dumps(bad))
                    with self.assertRaisesRegex(ValueError, 'invalid RomFS profile'):
                        resources.verify_romfs(path, profile)
                profile.write_text(json.dumps(good))
                with resources.verified_romfs(path, profile) as (record, stream):
                    self.assertEqual(record['sha256'], good['sha256'])
                    self.assertFalse(stream.closed)
                    if os.name == 'nt':
                        with self.assertRaises(PermissionError): path.write_bytes(b'changed')
                self.assertTrue(stream.closed)

    def test_fingerprint_cache_and_changed_input_rejection(self):
        with tempfile.TemporaryDirectory(prefix='resource-test-') as temp:
            root = Path(temp)
            data = b'synthetic resource only' * 100
            path, profile = root / 'romfs.bin', root / 'profile.json'
            path.write_bytes(data)
            profile.write_text(json.dumps({'version': 'test', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))
            with patch.object(resources, 'ROOT', root), patch.object(resources, 'PROFILE', profile):
                first = resources.verify_romfs(path)
                self.assertFalse(first['cached'])
                before = path.read_bytes()
                with patch.object(resources, 'stream_hash', wraps=resources.stream_hash) as hashed:
                    self.assertEqual(resources.verify_romfs(path)['cached'], os.name != 'nt')
                    self.assertEqual(hashed.call_count, int(os.name == 'nt'))
                self.assertEqual(path.read_bytes(), before)
                receipt = root / 'work/resources/verified.json'
                saved = receipt.read_bytes()
                path.write_bytes(b'x' * len(data))
                with self.assertRaisesRegex(ValueError, 'fingerprint'):
                    resources.verify_romfs(path)
                self.assertEqual(receipt.read_bytes(), saved)
                path.write_bytes(data)
                def change(stream):
                    result = hashlib.file_digest(stream, 'sha256').hexdigest()
                    path.write_bytes(b'y' * len(data))
                    return result
                with patch.object(resources, 'stream_hash', side_effect=change):
                    if os.name == 'nt':
                        with self.assertRaises(PermissionError):
                            resources.verify_romfs(path)
                        self.assertEqual(path.read_bytes(), data)
                    else:
                        with self.assertRaisesRegex(ValueError, 'changed during verification'):
                            resources.verify_romfs(path)
                self.assertEqual(receipt.read_bytes(), saved)

    @unittest.skipUnless(os.name == 'nt', 'Windows share mode and timestamp coalescing')
    def test_windows_rehashes_same_size_change_with_restored_timestamp(self):
        with tempfile.TemporaryDirectory(prefix='resource-metadata-') as temp:
            root = Path(temp); path = root / 'romfs.bin'; profile = root / 'profile.json'
            data = b'synthetic resource' * 100; path.write_bytes(data)
            profile.write_text(json.dumps({'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))
            with patch.object(resources, 'ROOT', root), patch.object(resources, 'PROFILE', profile):
                resources.verify_romfs(path)
                original = path.stat()
                path.write_bytes(b'x' * len(data))
                os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
                self.assertEqual(resources.stamp(original), resources.stamp(path.stat()))
                with self.assertRaisesRegex(ValueError, 'fingerprint'):
                    resources.verify_romfs(path)

    @unittest.skipUnless(os.name == 'nt', 'Windows share-mode enforcement')
    def test_windows_reader_rejects_open_writer_and_file_replacement(self):
        with tempfile.TemporaryDirectory(prefix='resource-lock-') as temp:
            root = Path(temp); path = root / 'input.bin'; path.write_bytes(b'synthetic')
            with path.open('r+b'):
                with self.assertRaises(PermissionError):
                    resources.resource_reader(path)
            with resources.resource_reader(path) as reader:
                self.assertEqual(reader.read(), b'synthetic')
                with self.assertRaises(PermissionError): path.write_bytes(b'changed')
                with self.assertRaises(PermissionError): path.rename(root / 'renamed.bin')
            path.write_bytes(b'released')
            self.assertEqual(path.read_bytes(), b'released')


if __name__ == '__main__':
    unittest.main()

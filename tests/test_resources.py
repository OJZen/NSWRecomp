import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import resources


class ResourceTests(unittest.TestCase):
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
                with patch.object(resources, 'stream_hash', side_effect=AssertionError('unchanged input rehashed')):
                    self.assertTrue(resources.verify_romfs(path)['cached'])
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
                    with self.assertRaisesRegex(ValueError, 'changed during verification'):
                        resources.verify_romfs(path)
                self.assertEqual(receipt.read_bytes(), saved)


if __name__ == '__main__':
    unittest.main()

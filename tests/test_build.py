import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build
from recompile import digest


class BuildTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='build-test-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'directory with spaces'
        self.output = self.root / 'work/build'
        self.source = self.root / 'work/generated/sample'
        self.output.mkdir(parents=True)
        self.source.mkdir(parents=True)
        self.enterContext(patch.object(build, 'ROOT', self.root))
        self.enterContext(patch.object(build, 'BUILD', self.output))
        self.context = build.context()
        build.write_json(self.output / 'context.json', self.context)
        files = {}
        for i in range(2):
            path = self.source / f'sample_{i:03}.c'
            path.write_text(f'int example_{i}(void) {{ return {i + 1}; }}\n')
            files[path.name] = digest(path)
        self.generation = {'files': files}
        build.write_json(self.source / 'receipt.json', self.generation)

    def test_resume_and_corruption_detection(self):
        first = build.batch('sample', limit=1)
        self.assertEqual((first['compiled'], first['remaining']), (1, 1))
        obj = self.output / 'sample/sample_000.o'
        before = obj.stat().st_mtime_ns
        second = build.batch('sample', limit=1)
        self.assertEqual((second['compiled'], second['reused'], second['remaining']), (1, 1, 0))
        self.assertEqual(obj.stat().st_mtime_ns, before)
        self.assertEqual(len(build.object_list({'sample': self.generation}, self.context)), 2)
        obj.write_bytes(b'corrupt')
        self.assertFalse(build.ready(obj, self.generation['files']['sample_000.c'], build.signature(self.context)))
        self.assertEqual(build.batch('sample')['compiled'], 1)

    def test_failed_compile_preserves_old_object_and_receipt(self):
        build.batch('sample')
        obj = self.output / 'sample/sample_000.o'
        receipt = obj.with_suffix('.json')
        old = obj.read_bytes(), receipt.read_bytes()
        with patch.object(build.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['clang'])):
            with self.assertRaises(subprocess.CalledProcessError):
                build.compile_unit(self.source / 'sample_000.c', obj,
                                   self.generation['files']['sample_000.c'], 'changed', build.FLAGS)
        self.assertEqual((obj.read_bytes(), receipt.read_bytes()), old)
        self.assertFalse(obj.with_suffix('.o.partial').exists())

    def test_changed_source_rejected(self):
        path = self.source / 'sample_000.c'
        sha = digest(path)
        path.write_text('changed')
        with self.assertRaisesRegex(ValueError, 'source changed'):
            build.compile_unit(path, self.output / 'unit.o', sha, 'unused', build.FLAGS)

    def test_stale_outputs_removed_and_foreign_files_preserved(self):
        build.batch('sample')
        directory = self.output / 'sample'
        stale = directory / 'sample_1000.o'
        stale.write_bytes(b'obsolete')
        build.object_list({'sample': self.generation}, self.context)
        self.assertFalse(stale.exists())
        foreign = directory / 'keep.txt'
        foreign.write_text('user data')
        with self.assertRaisesRegex(ValueError, 'refusing cleanup'):
            build.object_list({'sample': self.generation}, self.context)
        self.assertEqual(foreign.read_text(), 'user data')


if __name__ == '__main__':
    unittest.main()

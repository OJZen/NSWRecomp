from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'third_party/lasrecomp/tools'))
from bounded import group_rss, run
from recompile import diagnostics, digest, generate, prepare_work, verify
from test_nso import fixture


class PipelineTests(unittest.TestCase):
    def test_failed_generation_preserves_previous_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            with patch('recompile.ROOT', root), patch('tempfile.tempdir', tempfile.tempdir):
                prepare_work()
                output = root / 'work/generated/rtld'
                output.mkdir()
                (output / 'old.c').write_text('previous generated output')
                receipt = {'inputs': 'old', 'files': {'old.c': digest(output / 'old.c')}}
                (output / 'receipt.json').write_text(json.dumps(receipt))
                with patch('recompile.verify', return_value={'rtld': {'base': '0x100000'}}), \
                     patch('recompile.generation_inputs', return_value='new'), \
                     patch('recompile.run', side_effect=RuntimeError('generation stopped')):
                    with self.assertRaisesRegex(RuntimeError, 'generation stopped'):
                        generate(root / 'input', 'rtld')
                self.assertEqual((output / 'old.c').read_text(), 'previous generated output')
                self.assertEqual(json.loads((output / 'receipt.json').read_text()), receipt)

    def test_work_symlink_rejected_before_writing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            target = root / 'elsewhere'
            target.mkdir()
            (root / 'work').symlink_to(target, target_is_directory=True)
            with patch('recompile.ROOT', root):
                with self.assertRaisesRegex(ValueError, 'symlink'):
                    prepare_work()
            self.assertEqual(list(target.iterdir()), [])

    def test_vendor_matches_recorded_source(self):
        vendor = ROOT / 'third_party/lasrecomp'
        record = json.loads((vendor / 'SOURCE.json').read_text())
        for file, sha in record['files'].items():
            with self.subTest(file=file):
                self.assertEqual(digest(vendor / file), sha)

    def test_unsupported_instructions_stay_visible(self):
        with tempfile.TemporaryDirectory() as td:
            directory = Path(td)
            (directory / 'sample_000.c').write_text(
                'rt_udf(c, 0x1000ull); /* unsupported synthetic opcode */\n'
                'rt_udf(c, 0x1004ull);\n')
            (directory / 'sample_table.c').write_text('const uint64_t sample_fn_count = 1;\n')
            result = diagnostics(directory, 'sample')
            self.assertEqual(result['unsupported_instructions'], {'0x1000': 'unsupported synthetic opcode'})
            self.assertEqual(result['status'], 'needs_instruction_support')
            self.assertEqual(result['functions'], 1)

    def test_profile_rejects_wrong_or_missing_modules(self):
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaisesRegex(ValueError, 'unmodified v1.4.3'):
                verify(Path(td))
            for name in ('rtld', 'main', 'subsdk0', 'sdk'):
                (Path(td) / name).write_bytes(fixture())
            with self.assertRaisesRegex(ValueError, 'unmodified v1.4.3'):
                verify(Path(td))

    def test_group_rss_only_counts_our_children(self):
        self.assertEqual(group_rss(' 10 100\n11 99999\n10 200\n', 10), 300 * 1024)
        with self.assertRaises(ValueError):
            group_rss('unavailable', 10)

    def test_monitor_failure_does_not_launch(self):
        with patch('bounded.sample', side_effect=RuntimeError('unavailable')):
            with patch('bounded.subprocess.Popen') as spawn:
                with self.assertRaises(RuntimeError):
                    run(['unused'], cwd=ROOT, log=ROOT / 'work/tmp/unused.log')
                spawn.assert_not_called()

    def test_timeout_reaps_child(self):
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / 'timeout.log'
            with patch('bounded.sample', return_value=1024):
                with self.assertRaisesRegex(RuntimeError, 'time budget'):
                    run([sys.executable, '-c', 'import time; time.sleep(5)'], cwd=ROOT,
                        log=log, seconds=0.05)

    def test_memory_limit_reaps_child(self):
        with tempfile.TemporaryDirectory() as td:
            with patch('bounded.sample', return_value=2048):
                with self.assertRaisesRegex(RuntimeError, 'RSS'):
                    run([sys.executable, '-c', 'import time; time.sleep(5)'], cwd=ROOT,
                        log=Path(td) / 'memory.log', rss_limit=1024)


if __name__ == '__main__':
    unittest.main()

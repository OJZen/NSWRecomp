"""Generic mode must never consult a game's patch tables."""
from pathlib import Path
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from gen import Module, main
from test_nso import fixture


class GenericTests(unittest.TestCase):
    def test_exclusive_fusion_respects_interior_entries(self):
        from test_cpu_semantics import CONFIGS, pair_words
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.nso'
            raw = fixture()
            load, store = pair_words(3, 1, 1, CONFIGS[0])
            struct.pack_into('<3I', raw, 0x180, load, store, 0xd65f03c0)
            path.write_bytes(raw)
            start = 0x100080
            m = Module('synthetic', path, 0x100000, 'c', game_patches=False)
            emitted = '\n'.join(m.emit_function(start, start + 12, {start}, {}, {}))
            self.assertIn('__atomic_compare_exchange_n', emitted)
            self.assertNotIn('rt_udf(c, 0x100084ull)', emitted)
            for labels, tables in (({start, start + 4}, {}), ({start}, {start + 8: 'all'})):
                emitted = '\n'.join(m.emit_function(start, start + 12, labels, tables, {}))
                self.assertNotIn('__atomic_compare_exchange_n', emitted)
                self.assertIn('exclusive pair / CAS', emitted)
            m.patches[start + 4] = lambda *_: '/* synthetic patch */'
            emitted = '\n'.join(m.emit_function(start, start + 12, {start}, {}, {}))
            self.assertNotIn('__atomic_compare_exchange_n', emitted)

    def test_cleanup_above_999_chunks(self):
        with tempfile.TemporaryDirectory() as td:
            directory = Path(td)
            path = directory / 'synthetic.nso'
            raw = fixture()
            struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)  # no unwind header
            path.write_bytes(raw)
            output = directory / 'out'
            output.mkdir()
            for name in ('synthetic_099.c', 'synthetic_1000.c', 'other_1000.c'):
                (output / name).write_text('stale or unrelated')
            args = ['gen.py', 'synthetic', str(path), '0x100000', str(output),
                    '--simd', 'c', '--no-game-patches', '--cache-limit', '1']
            with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
                main()
            self.assertFalse((output / 'synthetic_099.c').exists())
            self.assertFalse((output / 'synthetic_1000.c').exists())
            self.assertTrue((output / 'other_1000.c').exists())
            self.assertTrue((output / 'synthetic_000.c').exists())

    def test_patch_isolation_and_bounded_cache(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.nso'
            raw = fixture()
            struct.pack_into('<3I', raw, 0x180, 0x91000400, 0x91000800, 0xd65f03c0)
            path.write_bytes(raw)
            with patch('gen.load_game_patches', side_effect=AssertionError('patch table was read')):
                m = Module('synthetic', path, 0x100000, 'c', game_patches=False, cache_limit=1)
                first = m.lift(0x100080)
                m.lift(0x100084)
                self.assertEqual(len(m.cache), 1)
                again = m.lift(0x100080)
                self.assertEqual((again.kind, again.code), (first.kind, first.code))
                lines = m.emit_function(0x100080, 0x10008c, {0x100080}, {}, {})
                self.assertIn('return;', '\n'.join(lines))
                self.assertEqual((m.patches, m.hooks, m.rando_code), ({}, {}, {}))
                self.assertLessEqual(len(m.cache), 1)
            tables = ({1: 'patch'}, {2: 'hook'}, {3: ['variant']})
            with patch('gen.load_game_patches', return_value=tables) as load:
                m = Module('synthetic', path, 0x100000)
                load.assert_called_once_with()
                self.assertEqual((m.patches, m.hooks, m.rando_code), tables)


if __name__ == '__main__':
    unittest.main()

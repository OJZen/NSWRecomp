"""Generic mode must never consult a game's patch tables."""
from pathlib import Path
import contextlib
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from gen import Module, main
from test_nso import fixture


class GenericTests(unittest.TestCase):
    def test_configured_hooks_validate_words_targets_and_patch_isolation(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.nso'
            raw = fixture()
            struct.pack_into('<3I', raw, 0x180, 0x91000400, 0x91000800, 0xd65f03c0)
            struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)  # no unwind header
            path.write_bytes(raw)
            config = Path(td) / 'hooks.json'
            record = {'offset': '0x80', 'expected': '0x91000400', 'targets': ['0x88']}
            def module(records):
                config.write_text(json.dumps({'schema': 1, 'hooks': records}))
                m = Module('synthetic', path, 0x100000, 'c', game_patches=False)
                m.load_hooks(config)
                return m
            with patch('gen.load_game_patches', side_effect=AssertionError('game patches loaded')):
                m = module([record])
            emitted = '\n'.join(m.emit_function(0x100080, 0x10008c, {0x100080}, {}, {}))
            self.assertIn('rt_hook(c, 0x100080ull)', emitted)
            self.assertIn('h_ == 0', emitted)
            self.assertIn('h_ == 2) goto L_100088', emitted)
            self.assertIn('L_100088:;', emitted)
            source = Path(td) / 'check.c'
            source.write_text('#include "cpu.h"\n#include <assert.h>\n' + emitted + '''
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
int rt_hook(Cpu *c, uint64_t site) { assert(site == 0x100080); return (int)c->x[1]; }
int main(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        Cpu c = {0}; c.x[1] = mode; f_100080(&c);
        assert(c.x[0] == (mode == 0 ? 3 : mode == 1 ? 2 : 0));
    }
}
''')
            binary = Path(td) / 'check'
            subprocess.run(['clang', '-std=gnu11', '-I', str(Path(__file__).resolve().parents[1] / 'runtime'),
                            str(source), '-o', str(binary)], check=True, capture_output=True, timeout=30)
            subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
            for records in ([record, record], [{**record, 'expected': '0x0'}],
                            [{**record, 'offset': '0x81'}], [{**record, 'targets': ['0x89']}],
                            [{**record, 'targets': ['0x88', '0x88']}], [{**record, 'expected': 12}]):
                with self.subTest(records=records), self.assertRaises(ValueError): module(records)
            m = module([record])
            with self.assertRaisesRegex(ValueError, 'outside its function'):
                m.emit_function(0x100080, 0x100088, {0x100080}, {}, {})
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                m.load_hooks(config)
            hle = Path(td) / 'hle.txt'; hle.write_text('entry\n')
            args = ['gen.py', 'synthetic', str(path), '0x100000', str(Path(td) / 'out'),
                    '--simd', 'c', '--no-game-patches', '--hooks', str(config), '--hle', str(hle)]
            with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaisesRegex(ValueError, 'overridden by HLE'):
                    main()

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

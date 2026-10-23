"""Five synthetic NSOs through generation, linking, loading and a kernel checkpoint."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from startup import BASIC_SOURCES
from recompile import digest
from test_load import fixture
from test_npdm import fixture as npdm_fixture
from test_romfs import fixture as romfs_fixture


class HostPipelineTests(unittest.TestCase):
    def test_five_modules_with_independent_local_profile(self):
        with tempfile.TemporaryDirectory(prefix='five module build ') as td:
            root = Path(td)
            for directory in ('tools', 'third_party/lasrecomp/tools', 'third_party/lasrecomp/runtime'):
                destination = root / directory
                destination.mkdir(parents=True)
                for path in (ROOT / directory).glob('*'):
                    if path.suffix in ('.py', '.h'):
                        shutil.copyfile(path, destination / path.name)
            (root / 'runtime').mkdir()
            for name in BASIC_SOURCES:
                shutil.copyfile(ROOT / 'runtime' / name, root / 'runtime' / name)
            exefs = root / 'local/exefs'
            exefs.mkdir(parents=True)
            modules = {}
            for i, name in enumerate(('rtld', 'main', 'subsdk0', 'subsdk1', 'sdk')):
                path = exefs / name
                fixture(path, main=name == 'main', exports=('_init_libc0',) if name == 'sdk' else ())
                raw = bytearray(path.read_bytes())
                struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)  # no unwind header
                struct.pack_into('<I', raw, 0x180, 0xd65f03c0)  # ret: synthetic function at text+0x80
                path.write_bytes(raw)
                modules[name] = {'base': hex(0x7200000000 + i * 0x10000), 'sha256': digest(path)}
            profile = root / 'local/profile.json'
            metadata = exefs / 'main.npdm'
            metadata.write_bytes(npdm_fixture())
            profile.write_text(json.dumps({'version': 'synthetic', 'modules': modules}))
            resource = root / 'local/资源.bin'; romfs_fixture(resource)
            resource_profile = root / 'local/romfs.json'
            resource_info = dict(size=resource.stat().st_size, sha256=digest(resource),
                                 exefs_sha256={name: info['sha256'] for name, info in modules.items()})
            resource_profile.write_text(json.dumps(resource_info))
            env = dict(os.environ, NSWRECOMP_PROFILE=str(profile), NSWRECOMP_OPT_LEVEL='1')
            for tool in ('build.py', 'load.py', 'startup.py'):
                command = [sys.executable, str(root / 'tools' / tool), '--exefs', str(exefs)]
                if tool == 'build.py': command += ['--jobs', '2']
                if tool == 'startup.py': command += ['--basic-services', '--npdm-sha256', digest(metadata), '--through', '_init_libc0',
                                                     '--romfs', str(resource), '--romfs-profile', str(resource_profile)]
                result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=120)
                logs = '\n'.join(p.read_text(encoding='utf-8', errors='replace')
                                 for p in (root / 'work/logs').glob('*.log')) if result.returncode else ''
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr + logs)
            record = json.loads((root / 'work/load/result.json').read_text())
            context = json.loads((root / 'work/build/context.json').read_text())
            self.assertIn('-O1', context['flags'])
            self.assertEqual(record['native']['modules'], 5)
            self.assertFalse(record['game_executed'])
            self.assertFalse(record['initialization_executed'])
            self.assertEqual(record['status'], 'native_load_only_verified')
            self.assertEqual(set(record['modules']), set(modules))
            checkpoint = json.loads((root / 'work/startup/result.json').read_text())
            self.assertEqual(checkpoint['status'], 'startup_checkpoint')
            self.assertFalse(checkpoint['kernel_only'])
            self.assertEqual(checkpoint['runtime_mode'], 'basic_services')
            self.assertEqual(checkpoint['service_policy']['sha256'], digest(metadata))
            self.assertEqual(checkpoint['native']['completed_stages'], 1)
            self.assertTrue(checkpoint['native']['initialization_executed'])
            self.assertFalse(checkpoint['native']['entered_nnMain'])
            self.assertFalse(checkpoint['native']['constructors_completed'])
            self.assertEqual(checkpoint['resource']['sha256'], digest(resource))
            resource_info['exefs_sha256']['main'] = '0' * 64
            resource_profile.write_text(json.dumps(resource_info))
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=120)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('RomFS profile does not match the selected ExeFS modules',
                          (root / 'work/logs/startup.log').read_text(encoding='utf-8'))

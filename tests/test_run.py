import json
from pathlib import Path
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import run
import startup
from build import write_json


class RunTests(unittest.TestCase):
    def test_finder_bundle_uses_shared_launcher_with_external_temporaries(self):
        with tempfile.TemporaryDirectory(prefix='finder launch ') as temp:
            root = Path(temp)
            for directory in ('script', 'work/logs', 'work/venv/bin', 'work/tmp'):
                (root / directory).mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / 'script/build_and_run.sh', root / 'script/build_and_run.sh')
            python = root / 'work/venv/bin/python'
            python.write_text('#!/bin/bash\nprintf "%s\\n" "$PWD" "$TMPDIR" "$PATH" "$PYTHONDONTWRITEBYTECODE" "$@"\n')
            python.chmod(0o755)
            executable = root / 'synthetic-native'; executable.write_bytes(b'no game data')
            app = root / 'work/app/TOTK Recompiled.app'
            game = app / 'Contents/Helpers/TOTK Runtime.app'
            with patch.multiple(startup, ROOT=root, APP=app, GAME_APP=game, NATIVE_APP=game / 'Contents/MacOS/totk-recomp'):
                startup.bundle(executable)
            metadata = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
            launcher = app / 'Contents/MacOS' / metadata['CFBundleExecutable']
            self.assertTrue(os.access(launcher, os.X_OK))
            self.assertNotEqual(launcher.read_bytes()[:2], b'#!')
            self.assertIn('NSRemovableVolumesUsageDescription', metadata)
            game_metadata = plistlib.loads((game / 'Contents/Info.plist').read_bytes())
            self.assertEqual(game_metadata['CFBundleIdentifier'], metadata['CFBundleIdentifier'] + '.runtime')
            result = subprocess.run([str(launcher)], cwd='/', env={'PATH': '/usr/bin:/bin'},
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = (root / 'work/logs/app-launch.log').read_text().splitlines()
            self.assertEqual(lines[:2], [str(root), str(root / 'work/tmp') + '/'])
            self.assertIn('/opt/homebrew/bin', lines[2].split(':'))
            self.assertEqual(lines[3:], ['1', 'tools/run.py', '--seconds', '600'])

    def test_stop_only_the_recorded_native_app_identity(self):
        with tempfile.TemporaryDirectory(prefix='run-owner-test-') as temp:
            receipt = Path(temp) / 'guest.json'
            identity = 'Wed Oct 21 00:00:00 2026 ' + str(run.NATIVE_APP)
            with patch.object(run, 'PROCESS', receipt), patch.object(run.os, 'kill') as kill:
                receipt.write_text(json.dumps(dict(pid=123, executable=str(run.NATIVE_APP), identity=identity)))
                with patch.object(run, 'process_identity', return_value='different process'):
                    run.stop_previous()
                kill.assert_not_called()
                receipt.write_text(json.dumps(dict(pid=123, executable=str(run.NATIVE_APP), identity='timestamp /other/app')))
                with self.assertRaisesRegex(ValueError, 'does not match'): run.stop_previous()
                kill.assert_not_called()
                receipt.write_text(json.dumps(dict(pid=123, executable=str(run.NATIVE_APP), identity=identity)))
                with patch.object(run, 'process_identity', side_effect=[identity, None]): run.stop_previous()
                kill.assert_called_once_with(123, run.signal.SIGTERM)

    def test_json_partial_symlink_does_not_replace_user_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); protected = root / 'protected'; protected.write_text('keep')
            output = root / 'result.json'; output.with_suffix('.json.partial').symlink_to(protected)
            with self.assertRaisesRegex(ValueError, 'symlink'): write_json(output, {})
            self.assertEqual(protected.read_text(), 'keep')

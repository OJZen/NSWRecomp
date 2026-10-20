import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import run
from build import write_json


class RunTests(unittest.TestCase):
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

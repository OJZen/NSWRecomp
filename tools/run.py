"""Build and run the native title probe using the user's ignored local input configuration."""
import argparse
import json
import os
from pathlib import Path
import signal
import time

from build import build
from recompile import ROOT, prepare_work
from startup import NATIVE_APP, PROCESS, process_identity, startup


def stop_previous():
    if PROCESS.is_symlink(): raise ValueError('process receipt must not be a symlink')
    if not PROCESS.exists(): return
    old = json.loads(PROCESS.read_text())
    if old.get('executable') not in (str(NATIVE_APP), str(ROOT / 'work/startup/probe')):
        raise ValueError('unrecognized previous app process; refusing to stop it')
    pid = old.get('pid')
    if type(pid) is not int or pid <= 1: raise ValueError('invalid app process receipt')
    identity = old.get('identity')
    if identity and not identity.endswith(' ' + old['executable']):
        raise ValueError('previous app identity does not match its executable; refusing to stop it')
    if identity and process_identity(pid) == identity:
        os.kill(pid, signal.SIGTERM)
        for _ in range(40):
            if process_identity(pid) != old['identity']: break
            time.sleep(.1)
        else: raise ValueError('previous app did not stop')
    if PROCESS.exists() and json.loads(PROCESS.read_text()).get('pid') == pid: PROCESS.unlink()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path)
    parser.add_argument('--romfs', type=Path)
    parser.add_argument('--seconds', type=int, choices=range(1, 601), metavar='1..600', default=180)
    parser.add_argument('--fps', type=int, choices=(30, 60, 90, 120), help='override the saved frame rate for this run')
    parser.add_argument('--jobs', type=int, choices=range(1, 33), metavar='1..32', help='compiler concurrency; default up to 8 CPU cores')
    args = parser.parse_args()
    try:
        prepare_work()
        if args.fps is not None: os.environ['TOTK_TARGET_FPS'] = str(args.fps)
        settings = ROOT / 'local/inputs.json'
        inputs = json.loads(settings.read_text()) if settings.is_file() else {}
        exefs = args.exefs or (Path(inputs['exefs']) if inputs.get('exefs') else None)
        romfs = args.romfs or (Path(inputs['romfs']) if inputs.get('romfs') else None)
        if not exefs or not romfs: raise ValueError('Import your NSZ packages first, or provide --exefs and --romfs')
        stop_previous()
        build(exefs.resolve(), jobs=args.jobs)
        result = startup(exefs.resolve(), 'nnMain', romfs.resolve(), graphics=True, seconds=args.seconds)
        print(json.dumps({k: result[k] for k in ('status', 'exit_code', 'native', 'title_verified')}, indent=2))
        if result['exit_code']: parser.exit(2, 'native startup stopped; see work/logs/startup.log\n')
    except (ValueError, KeyError, OSError, RuntimeError) as error:
        parser.exit(1, f'{error}\n')

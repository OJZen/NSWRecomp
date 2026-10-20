"""Run one POSIX process group with polling limits; keep its log on the workspace volume."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def group_rss(rows, group):
    total = 0
    for row in rows.splitlines():
        fields = row.split()
        if len(fields) != 2:
            raise ValueError('invalid process memory sample')
        pgid, kib = map(int, fields)
        if pgid == group:
            total += kib * 1024
    return total


def sample(group):
    result = subprocess.run(['ps', '-axo', 'pgid=,rss='], capture_output=True, text=True, timeout=5)
    if result.returncode:
        raise RuntimeError('process memory monitoring unavailable; run in a terminal with process access')
    return group_rss(result.stdout, group)


def run(command, *, cwd, log, seconds=900, rss_limit=1536 << 20, reserve=20 << 30):
    sample(os.getpgrp())  # Fail before launching if the monitor cannot run.
    if shutil.disk_usage(cwd).free < reserve:
        raise RuntimeError('less than 20 GiB free on the workspace volume')
    log = Path(log)
    env = dict(os.environ, TMPDIR=str(Path(cwd) / 'work/tmp'), PYTHONDONTWRITEBYTECODE='1')
    start, peak = time.monotonic(), 0
    with log.open('w') as output:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            while process.poll() is None:
                peak = max(peak, sample(process.pid))
                if peak > rss_limit:
                    raise RuntimeError('process group exceeded 1536 MiB RSS')
                if time.monotonic() - start > seconds:
                    raise RuntimeError('process group exceeded its time budget')
                if shutil.disk_usage(cwd).free < reserve:
                    raise RuntimeError('free space fell below the workspace reserve')
                time.sleep(0.25)
            if process.returncode:
                raise RuntimeError(f'child failed with exit code {process.returncode}; see its local log')
        finally:
            # Also stop descendants if the direct child exited early.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    return {'seconds': round(time.monotonic() - start, 3), 'peak_group_rss_bytes': peak,
            'rss_limit_bytes': rss_limit, 'free_space_reserve_bytes': reserve}

"""Translate indexed shaders while preserving the original control flow."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

from build_shaders import ROOT, TOOLS, XLATE, shader_entries


def valid(entry, output):
    try:
        data = (output / (entry['key'] + '.spv')).read_bytes()
        metadata = json.loads((output / (entry['key'] + '.res.json')).read_text())
        return len(data) >= 20 and data[:4] == b'\x03\x02\x23\x07' and metadata['key'] == entry['key'] and metadata['stage'] == entry['stage']
    except (OSError, ValueError, KeyError):
        return False


def translate():
    output = ROOT / 'work/shaders/spv'; output.mkdir(parents=True, exist_ok=True)
    entries = shader_entries(output.parent)
    version = hashlib.sha256(Path(__file__).read_bytes() + b''.join(p.read_bytes() for p in sorted(XLATE.glob('*.dll')))).hexdigest()
    stamp = output / 'translator.sha256'
    current = stamp.exists() and stamp.read_text().strip() == version
    # The upstream AST optimizer silently drops BRX cases used for glyph coordinates and UI panels.
    jobs = [dict(e, code=str(output.parent / 'code' / (e.get('base', e['key']) + '.bin')), debug=True)
            for e in entries if not current or not valid(e, output)]
    for job in jobs:
        if 'next' in job:
            job['next'] = dict(job['next'], code=str(output.parent / 'code' / (job['next']['key'] + '.bin')))
    for e in jobs:
        for suffix in ('.spv', '.res.json'):
            (output / (e['key'] + suffix)).unlink(missing_ok=True)
    environment = dict(os.environ, DOTNET_ROOT=str(TOOLS / 'dotnet'), DOTNET_CLI_HOME=str(TOOLS / 'dotnet-home'),
                       DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1')
    job_file = output.parent / 'jobs.json'
    if jobs:
        job_file.write_text(json.dumps(jobs) + '\n')
        result = subprocess.run([str(TOOLS / 'dotnet/dotnet'), str(XLATE / 'dmsh_xlate.dll'), str(job_file), str(output)], env=environment)
        if result.returncode not in (0, 1): raise RuntimeError('shader translator could not run')
        jobs = [e for e in jobs if not valid(e, output)]
    if jobs: raise RuntimeError(f'{len(jobs)} shaders could not be translated')
    assert all(valid(e, output) for e in entries), 'SPIR-V output validation failed'
    stamp.write_text(version + '\n')
    job_file.unlink(missing_ok=True)
    print('Validated SPIR-V shaders:', len(entries))


if __name__ == '__main__':
    translate()

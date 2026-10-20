"""Compile one immutable snapshot of the shaders requested by the current draw."""
import hashlib
import json
import os
from pathlib import Path
import sys

import build_shaders
from build_shaders import ROOT, shader_entries
from index_shaders import archive, index
import translate_shaders
from translate_shaders import translate

SHADERS = ROOT / 'work/shaders'
LABELS = {'vs': 'vertex', 'ps': 'pixel', 'cs': 'compute', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry'}


def write_atomic(path, text):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(text)
    temporary.replace(path)


def index_captures():
    # Equivalent to index_shaders.index() for an unchanged archive set: append records of new runtime captures.
    records = json.loads((SHADERS / 'index.json').read_text())
    known = {entry['key']: entry for entry in records}
    sources = {entry['source'] for entry in records}
    added = []
    for path in sorted((SHADERS / 'source').glob('runtime-*.nvn')):
        if path.name in sources:
            continue
        for entry, program in archive(path.read_bytes()):
            entry['code_sha256'] = hashlib.sha256(program).hexdigest()
            entry['source'] = path.name
            prior = known.get(entry['key'])
            if prior:
                if any(prior[k] != entry[k] for k in ('stage', 'code_sha256', 'c1')):
                    raise ValueError('shader control hash collision')
                continue
            (SHADERS / 'code' / (entry['key'] + '.bin')).write_bytes(program)
            known[entry['key']] = entry
            records.append(entry)
            added.append(entry['key'])
    if added:
        write_atomic(SHADERS / 'index.json', json.dumps(records, indent=2) + '\n')
        write_atomic(SHADERS / 'shader_map.tsv', 'package\tindex\tstage\tcontrol\n' + ''.join(
            f"{e['source']}\t0\t{LABELS[e['stage']]}\t{e['key']}\n" for e in records))
    print('Runtime records added:', len(added), flush=True)
    return set(added)


def compile_pending(keys):
    if not (SHADERS / 'index.json').exists(): index(SHADERS / 'source', SHADERS / 'code')
    else: index_captures()
    entries = shader_entries(SHADERS, keys)
    print('Requested shaders:', len(entries), flush=True)
    original = build_shaders.shader_entries
    translate_shaders.shader_entries = build_shaders.shader_entries = lambda directory: entries
    failures_path = SHADERS / 'build-failures.json'
    previous = json.loads(failures_path.read_text()) if failures_path.exists() else []
    try:
        translate()
        blocked = build_shaders.build()
    finally:
        translate_shaders.shader_entries = build_shaders.shader_entries = original
    # build() rewrote the failure list for this subset; keep the earlier entries for other shaders.
    current = json.loads(failures_path.read_text())
    processed = {entry['key'] for entry in entries}
    write_atomic(failures_path, json.dumps(current + [f for f in previous if f['key'] not in processed], indent=2) + '\n')
    return blocked


if __name__ == '__main__':
    os.chdir(ROOT)
    sys.exit(1 if compile_pending(sys.argv[1:]) else 0)

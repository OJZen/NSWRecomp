"""Compile shaders captured at run time with the same checked offline pipeline, touching only what is new.

The game waits for this inside a frame. Re-indexing every archive and revalidating every translation took about
10 s per capture, so only new runtime records and variant requests without a library are processed; their
translation and MSL build reuse the unchanged translate/build steps. If the translator itself changed, the full
pipeline runs as before.
"""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

import build_shaders
from build_shaders import ROOT, XLATE, chain, shader_entries
from index_shaders import archive, index
import translate_shaders
from translate_shaders import translate

SHADERS = ROOT / 'work/shaders'
LABELS = {'vs': 'vertex', 'ps': 'pixel', 'cs': 'compute', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry'}


def translator_current():
    # Same identity as translate_shaders.translate() records in spv/translator.sha256.
    version = hashlib.sha256(Path(translate_shaders.__file__).read_bytes() +
                             b''.join(p.read_bytes() for p in sorted(XLATE.glob('*.dll')))).hexdigest()
    stamp = SHADERS / 'spv/translator.sha256'
    return stamp.exists() and stamp.read_text().strip() == version


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


def requested(added):
    library = SHADERS / ('b-' + chain())
    built = {name[:-len('_b.metallib')] for name in os.listdir(library) if name.endswith('_b.metallib')} if library.exists() else set()
    variants = [name[:-5] for name in os.listdir(SHADERS / 'variants') if name.endswith('.json')]
    failures = SHADERS / 'build-failures.json'
    # A request written before its translation ran (e.g. the game stopped first) is pending, not blocked.
    blocked = {failure['key'] for failure in json.loads(failures.read_text())
               if failure['error'] != 'SPIR-V translation missing'} if failures.exists() else set()
    # Known blocked requests are not retried on every capture; drawing one still stops the game explicitly.
    missing = {key for key in variants if key not in built and key not in blocked} | {key for key in added if key not in built}
    with tempfile.TemporaryDirectory(dir=ROOT / 'work/tmp') as temporary:
        # shader_entries validates requests exactly as the full build does; give it only the pending ones.
        (Path(temporary) / 'index.json').symlink_to(SHADERS / 'index.json')
        (Path(temporary) / 'variants').mkdir()
        for key in missing:
            if key not in added:
                (Path(temporary) / 'variants' / (key + '.json')).symlink_to(SHADERS / 'variants' / (key + '.json'))
        return [entry for entry in shader_entries(Path(temporary)) if entry['key'] in missing]


def compile_pending():
    if not translator_current():
        index(SHADERS / 'source', SHADERS / 'code')
        translate()
        return build_shaders.build()
    entries = requested(index_captures())
    print('Pending shaders:', len(entries), flush=True)
    if not entries:
        return False
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
    compile_pending()  # Unused unsupported shaders do not prevent the runtime checking its requested output.
    sys.exit(0)

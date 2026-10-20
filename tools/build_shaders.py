"""Reuse the Maxwell translator/reflection helpers in core/ for Zelda's own shader records."""
import ast
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from spirv_subgroup import lower as lower_subgroups
from build import write_json

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'third_party/zelda-mobile/core'
TOOLS = ROOT / 'work/tools'
XLATE = ROOT / 'work/build/dotnet/bin/dmsh_xlate/release'
# Every programmable gl_PointSize in v1.4.3 (Grass, Agl, agl_technique vertex shaders) is written as
# min(size, c0[0x610].x): the compiler's clamp to a driver-supplied point-size limit. The host fills only that
# component, for the vertex stage (nvn_mtl.m adaptation in prepare_graphics.py).
POINT_SIZE_LIMIT = 0x610


def point_size_limit_misuse(source):
    return bool(re.search(r'_c0\)\._m0\[%du?\](?!\.x\b)' % (POINT_SIZE_LIMIT // 16), source))


def shader_entries(directory, requested=None):
    if requested is not None:
        requested = set(requested)
        if not requested or any(not isinstance(key, str) or not re.fullmatch(r'[0-9a-f]{16}(?:-[0-9a-f]{4}(?:[0-9a-f]{4})?)?', key) for key in requested):
            raise ValueError('invalid requested shader key')
    entries = json.loads((directory / 'index.json').read_text())
    if (not isinstance(entries, list) or any(not isinstance(e, dict) or
            not re.fullmatch(r'[0-9a-f]{16}', str(e.get('key', ''))) or
            e.get('stage') not in ('vs', 'ps', 'cs', 'hs', 'ds', 'gs') for e in entries)):
        raise ValueError('invalid shader index')
    bases = {entry['key']: entry for entry in entries}
    if len(bases) != len(entries): raise ValueError('duplicate shader index key')
    if requested is not None: entries = [entry for entry in entries if entry['key'] in requested]
    for path in sorted((directory / 'variants').glob('*.json')):
        if requested is not None and path.stem not in requested: continue
        request = json.loads(path.read_text())
        base, mask = request['base'], request['mask']
        stage = bases.get(base, {}).get('stage')
        slots = 16 if stage == 'vs' else 8
        next_stage = request.get('next')
        if (stage not in ('vs', 'ps') or type(mask) is not int or
                not (0 if next_stage else 1) <= mask < 1 << (2 * slots) or
                any((mask >> (2 * i)) & 3 == 3 for i in range(slots)) or
                (next_stage is not None and (not isinstance(next_stage, str) or stage != 'vs' or
                                            bases.get(next_stage, {}).get('stage') != 'ps'))):
            raise ValueError('invalid shader interface variant')
        name = f'{base}-{mask:0{slots // 2}x}'
        if next_stage: name = hashlib.sha256(f'{base}-{mask:08x}-{next_stage}'.encode()).hexdigest()[:16]
        if path.stem != name or name in bases:
            raise ValueError('shader variant identity mismatch')
        types = {str(i): ('uint' if (mask >> (2 * i)) & 3 == 1 else 'sint')
                 for i in range(slots) if (mask >> (2 * i)) & 3}
        entry = dict(bases[base], key=path.stem, base=base, **{'attr' if stage == 'vs' else 'fout': types})
        if next_stage: entry['next'] = bases[next_stage]
        entries.append(entry)
    if requested is not None and {entry['key'] for entry in entries} != requested:
        raise ValueError('requested shader capture is missing')
    return entries


def chain():
    paths = [Path(__file__), ROOT / 'tools/index_shaders.py', ROOT / 'tools/translate_shaders.py', ROOT / 'tools/spirv_subgroup.py', CORE / 'gfx/tools/dmsh_build.py',
             TOOLS / 'spirv-cross', *[XLATE / (n + '.dll') for n in ('dmsh_xlate', 'Ryujinx.Graphics.Shader', 'Ryujinx.Common', 'Spv.Generator')]]
    if any(not p.is_file() for p in paths):
        return 'unprepared'
    digest = hashlib.sha256()
    for p in paths:
        digest.update(p.name.encode() + b'\0' + p.read_bytes())
    version = digest.hexdigest()[:12]
    # Reuse caches only for exact source/tool fingerprints verified to emit identical libraries.
    return version


def point_size_uniform(source):
    # The translator's default point size becomes the NVN fixed size; a later programmable write still overrides it.
    assignment = 'out.gl_PointSize = 1.0;'
    entry = re.search(r'vertex\s+\w+\s+\w+\s*\(', source)
    if source.count(assignment) != 1 or not entry or '[[buffer(15)]]' in source:
        raise ValueError('unsupported vertex point-size layout')
    offset = entry.end()
    parameter = 'constant float& zelda_point_size [[buffer(15)]]'
    if source[offset:].lstrip()[0] != ')': parameter += ', '
    source = source[:offset] + parameter + source[offset:]
    return source.replace(assignment, 'out.gl_PointSize = zelda_point_size;')


def pointer_storage(bindings, resources):
    """Read-only global memory whose base address the shader loads from a game uniform buffer (v1.4.3 cave_quadmesh
    vertex shaders: c6[0], NVN uniform slot 3 at byte 0). The upstream reflection only maps driver c0 storage slots;
    these become kind sbuf_cb and the host reads the address from that uniform buffer when binding."""
    ordered = sorted(resources['sbufs'], key=lambda b: b['binding'])
    for binding in bindings:
        if binding['kind'] != 'sbuf' or binding.get('reg') is not None or binding['index'] >= len(ordered):
            continue
        source = ordered[binding['index']]
        offset = source['cb_offset'] * 4
        if 3 <= source['cb_slot'] <= 16 and not source['flags'] & 1 and offset % 8 == 0 and offset + 8 <= 0x10000:
            binding.update(kind='sbuf_cb', reg=source['cb_slot'] - 3, offset=offset, rw=False)
    return bindings


def reflection_helpers():
    source = (CORE / 'gfx/tools/dmsh_build.py').read_text()
    source = source.replace("r'fp_c%d", "r'[a-z]+_c%d")
    if source.count("b['flags'] & 2") != 1:
        raise ValueError('upstream buffer usage flags changed')
    source = source.replace("b['flags'] & 2", "b['flags'] & 1")
    names = {'decode_tex', 'reflect', 'raster_facts', 'drv_reads', 'unfillable'}
    nodes = [n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef) and n.name in names]
    if {n.name for n in nodes} != names:
        raise ValueError('upstream shader reflection helpers changed')
    scope = {'re': re, 'TEX_BASE': 0x168, 'SAMP_BASE': 0x568, 'IMG_BASE': 0x120,
             'DRV_FILL': {0: [(0x110, 0x210), (0x310, 0x410), (0x510, 0x610)],
                          2: [(0x20, 0x120), (0x168, 0x568), (0x568, 0x968)]}}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), '<shared shader reflection>', 'exec'), scope)
    separate = scope['decode_tex']

    def decode(name):
        result = separate(name)
        match = re.fullmatch(r'[a-z]+_t\w*?_(?:cb2|tcb)_([0-9A-F]+)', name)
        if result and result[0] is None and result[2] == 0 and match:
            offset = int(match[1], 16) * 4
            if 0x20 <= offset < 0x120 and not (offset - 0x20) % 8:
                return (offset - 0x20) // 8, None, 0
        return result
    scope['decode_tex'] = decode
    original_unfillable = scope['unfillable']

    def unfillable(reads, stage):
        reads = {bank: dict(info, vec4_offsets=[offset for offset in info['vec4_offsets']
                 if not (stage == 'cs' and bank == '0' and 0x210 <= offset < 0x310) and
                 not (stage == 'vs' and bank == '0' and offset == POINT_SIZE_LIMIT)])
                 for bank, info in reads.items()}
        return original_unfillable(reads)
    scope['unfillable'] = unfillable
    return scope


def build():
    version = chain()
    if version == 'unprepared':
        raise ValueError('shader tools have not been prepared')
    entries = shader_entries(ROOT / 'work/shaders')
    target = ROOT / 'work/shaders' / ('b-' + version)
    if target.is_symlink(): raise ValueError('shader output must not be a symlink')
    target.mkdir(parents=True, exist_ok=True)
    spv = ROOT / 'work/shaders/spv'
    helpers = reflection_helpers()
    failures = []

    def finish(entry):
        key = entry['key']
        metadata = target / (key + '_b.json')
        library = target / (key + '_b.metallib')
        if metadata.exists() and library.exists():
            return None
        result_file = spv / (key + '.res.json')
        if not result_file.exists():
            return {'key': key, 'error': 'SPIR-V translation missing'}
        arguments = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
        result = subprocess.run([str(TOOLS / 'spirv-cross'), str(spv / (key + '.spv')), *arguments], capture_output=True, text=True)
        if result.returncode and entry['stage'] == 'vs' and 'Subgroup builtins are not available' in result.stderr:
            # Metal vertex functions have no lane builtins; ballot-only use lowers to a one-invocation subgroup.
            lowered = lower_subgroups((spv / (key + '.spv')).read_bytes())
            if lowered:
                with tempfile.NamedTemporaryFile(suffix='.spv', dir=ROOT / 'work/tmp') as single:
                    single.write(lowered); single.flush()
                    result = subprocess.run([str(TOOLS / 'spirv-cross'), single.name, *arguments], capture_output=True, text=True)
        if result.returncode:
            return {'key': key, 'error': result.stderr[:500]}
        source = result.stdout
        if entry['stage'] == 'vs':
            try:
                source = point_size_uniform(source)
            except ValueError as error:
                return {'key': key, 'error': str(error)}
        resources = json.loads(result_file.read_text())
        bindings = pointer_storage(helpers['reflect'](key, entry['stage'], source, resources), resources)
        missing = [b['msl_name'] for b in bindings if b['kind'] == '?' or
                   (b['kind'] in ('tex', 'samp', 'samp_tex', 'img', 'tbuf', 'sbuf', 'ubo_ptr') and b.get('reg') is None)]
        if missing:
            return {'key': key, 'error': 'unresolved resource bindings', 'bindings': missing}
        function = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', source)
        if not function:
            return {'key': key, 'error': 'Metal entry point missing'}
        reads = helpers['drv_reads'](source)
        unfillable = helpers['unfillable'](reads, entry['stage'])
        if point_size_limit_misuse(source):
            unfillable.append('c0+0x610 beyond the point-size limit component')
        if unfillable:
            return {'key': key, 'error': 'unsupported driver constant reads', 'reads': unfillable}
        data = dict(key=key + '_b', route='B', chain=version, stage=entry['stage'], source=entry['source'],
            entry=function[2], msl_entry_kind=function[1], bindings=bindings, c1=entry['c1'],
            threadgroup_size=entry.get('local'), notes=resources['notes'], drv_reads=reads, drv_unfillable=[],
            vertex_inputs=[{'location': int(m[2]), 'msl_type': m[1]} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', source)],
            raster=helpers['raster_facts'](entry['stage'], source), chain_metal='macOS runtime source compiler')
        # The shared backend already supports source libraries when its shell branch is enabled.
        partial = library.with_suffix('.metallib.partial')
        if library.is_symlink() or partial.is_symlink(): raise ValueError('shader library must not be a symlink')
        partial.write_text(source); partial.replace(library)
        write_json(metadata, data)
        return None

    with ThreadPoolExecutor(max_workers=1) as executor:
        for i, failure in enumerate(executor.map(finish, entries), 1):
            if failure: failures.append(failure)
            if i % 500 == 0: print('MSL processed', i, 'blocked', len(failures), flush=True)
    (ROOT / 'work/shaders/build-failures.json').write_text(json.dumps(failures, indent=2) + '\n')
    print('MSL libraries:', len(entries) - len(failures), 'blocked:', len(failures), 'chain:', version)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(build())

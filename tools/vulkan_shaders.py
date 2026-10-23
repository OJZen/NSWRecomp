"""Prepare checked, private NVN shader captures for the bounded Vulkan backend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

from bounded import run
from recompile import ROOT


def index(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f'{value:016x}'


def decode(data):
    def word(offset):
        if offset + 4 > len(data): raise ValueError('truncated shader record')
        return struct.unpack_from('<I', data, offset)[0]
    length = word(0x20)
    if word(0) != 0x98761234 or word(4) != 1 or word(8) not in (9, 15, 16) or not 0x748 <= length <= 65536:
        raise ValueError('unsupported NVN control header')
    kind, offset, c1, constants, code_size = (word(i) for i in (0x714, 0x708, 0x700, 0x6fc, 0x704))
    if kind not in (0, 1, 5) or code_size > 8 << 20 or len(data) != length + code_size or c1 + constants > code_size:
        raise ValueError('invalid shader source extent or stage')
    if length % 4 or len(data) % 4 or (kind == 5 and length < 0x758):
        raise ValueError('invalid shader control extent or alignment')
    if offset != (0 if word(8) == 15 else 0x100) and kind == 5 or kind != 5 and offset != 0x30:
        raise ValueError('unsupported shader code offset')
    if offset + word(0x6f8) > c1 or c1 < offset or (c1 - offset) % 8:
        raise ValueError('invalid shader instruction span')
    code = data[length:]
    if offset and int.from_bytes(code[:4], 'little') != (0x12345679 if kind == 5 else 0x12345678):
        raise ValueError('invalid shader code header')
    job = dict(key=hashlib.sha256(data).hexdigest(), stage={0: 'vs', 1: 'ps', 5: 'cs'}[kind],
               c1=code[c1:c1 + constants].hex(), debug=False, reduced=False)
    if kind == 5:
        local = [word(0x748 + i * 4) for i in range(3)]
        shared = word(0x754)
        if min(local) < 1 or local[0] * local[1] * local[2] > 1024 or shared > 49152:
            raise ValueError('invalid compute workgroup')
        job.update(local=local, shared=shared)
    return kind, job, code[offset:c1]


def prepare(capture, dotnet, translator):
    directory = capture.resolve().parent
    if not directory.is_relative_to((ROOT / 'work').resolve()):
        raise ValueError('derived shader outputs must stay under ignored work/')
    state = json.loads(capture.read_text(encoding='utf-8'))
    if state.get('version') != 1 or len(state['stages']) not in (1, 2): raise ValueError('unsupported command capture')
    if state.get('origin') not in (0, 1) or state.get('depth_mode') not in (0, 1):
        raise ValueError('invalid captured coordinate convention')
    jobs, sources = {}, {}
    for stage in state['stages']:
        path = (directory / stage['source']).resolve()
        if not path.is_relative_to(directory) or path.stat().st_size > (9 << 20): raise ValueError('invalid captured shader path')
        data = path.read_bytes()
        kind, job, code = decode(data)
        if kind != stage['stage'] or kind in jobs: raise ValueError('captured stage mismatch or duplicate')
        code_path = directory / f'{job["key"]}.code'
        code_path.write_bytes(code)
        job['code'] = str(code_path)
        if kind != 5:
            job.update(origin_upper_left=state['origin'] == 1, depth_minus_one=state['depth_mode'] == 0,
                       attr={}, fout={'0': 'float'}, tex={})
            for i, attribute in enumerate(state['vertex']):
                if attribute['format']:
                    if attribute['format'] not in (10, 22, 34, 46): raise ValueError('draw translator currently accepts float vertex inputs')
                    job['attr'][str(i)] = 'float'
            descriptors = {d['id']: d for d in state['descriptors']}
            for slot, handle in enumerate(stage['textures']):
                if handle:
                    texture = descriptors.get(handle & 0xfffff)
                    if not texture or texture['target'] != 1 or texture['levels'] != 1: raise ValueError('draw translator currently accepts one-level 2D textures')
                    job['tex'][str(slot)] = '2d'
        jobs[kind], sources[kind] = job, data
    if set(jobs) == {0, 1}: jobs[0]['next'] = dict(jobs[1])
    elif set(jobs) != {5}: raise ValueError('expected compute or paired vertex/fragment stages')
    job_path = directory / 'translate-jobs.json'
    job_path.write_text(json.dumps(list(jobs.values()), indent=2), encoding='utf-8')
    output = directory / 'translated'
    record = run([str(dotnet), str(translator), str(job_path), str(output)], cwd=ROOT,
                 log=ROOT / 'work/logs/vulkan-shader-translate.log', seconds=120)
    resources, binaries = {}, {}
    for kind, job in jobs.items():
        res = json.loads((output / (job['key'] + '.res.json')).read_text(encoding='utf-8'))
        binary = (output / (job['key'] + '.spv')).read_bytes()
        if res['notes'] or res['images'] or len(binary) > 4 << 20 or len(binary) % 4 or binary[:4] != b'\x03\x02\x23\x07':
            raise ValueError('invalid translated shader or unsupported image bindings')
        resources[kind], binaries[kind] = res, binary
    bindings = [(2, 0, 0, 0, 0)]
    if 5 in jobs:
        res = resources[5]
        if res['textures']: raise ValueError('compute textures are not implemented')
        bindings += [(0, r['binding'], r['slot'], 0, 0) for r in res['cbufs']]
        bindings += [(1, r['binding'], r['cb_slot'], r['cb_offset'], r['flags']) for r in res['sbufs']]
        source, binary = sources[5], binaries[5]
        header = struct.pack('<8I', 0x4357534e, 1, len(source), len(binary), len(bindings), *jobs[5]['local'])
        payload = source + b''.join(struct.pack('<5I', *r) for r in bindings) + binary
        filename = index(source) + '.vkcs'
    else:
        for kind in (0, 1):
            res = resources[kind]
            if res['sbufs']: raise ValueError('draw storage buffers are not implemented')
            bindings += [(0, kind, r['binding'], r['slot'], 0) for r in res['cbufs']]
            for r in res['textures']:
                offset = r['handle'] * 4
                base = 0x20 if 0x20 <= offset < 0x120 else 0x168
                if r['type'] != 3 or r['array'] != 1 or r['separate'] or r['cbuf_slot'] not in (-1, 2) or not 0 <= offset - base < 256 or (offset - base) % 8:
                    raise ValueError('unsupported draw texture binding')
                bindings.append((1, kind, r['binding'], (offset - base) // 8, 0))
        header = struct.pack('<12I', 0x4457534e, 1, len(sources[0]), len(sources[1]), len(binaries[0]), len(binaries[1]), len(bindings), state['origin'], state['depth_mode'], 0, 0, 0)
        payload = sources[0] + sources[1] + b''.join(struct.pack('<5I', *r) for r in bindings) + binaries[0] + binaries[1]
        filename = f'draw-{index(sources[0])}-{index(sources[1])}-{state["origin"]}-{state["depth_mode"]}.vkdraw'
    if len(bindings) > 32: raise ValueError('shader binding limit')
    cache = directory / 'cache'; cache.mkdir(exist_ok=True)
    packet = cache / filename; packet.write_bytes(header + payload)
    record.update(packet=str(packet), source_sha256={str(k): hashlib.sha256(v).hexdigest() for k, v in sources.items()},
                  translator_sha256=hashlib.sha256(translator.read_bytes()).hexdigest(), packet_sha256=hashlib.sha256(packet.read_bytes()).hexdigest())
    packet.with_suffix(packet.suffix + '.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, default=ROOT / 'work/vulkan-shaders/command.json')
    parser.add_argument('--dotnet', type=Path, default=ROOT / 'work/tools/dotnet-win/dotnet.exe')
    parser.add_argument('--translator', type=Path, default=ROOT / 'work/build/vulkan-xlate/bin/dmsh_xlate/release/dmsh_xlate.dll')
    args = parser.parse_args()
    os.environ['DOTNET_CLI_HOME'] = str(ROOT / 'work/tmp/dotnet-home')
    try: print(json.dumps(prepare(args.capture, args.dotnet, args.translator), indent=2))
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error: parser.exit(1, str(error) + '\n')

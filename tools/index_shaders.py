"""Index the checked shader records in Zelda's BNSH, BFSHA v9 and BAHS v9 files."""
from pathlib import Path
import hashlib
import json
import struct

STAGES = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'hs', 4: 'ds', 5: 'cs'}


def span(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader archive range outside file')
    return data[offset:offset + size]


def read(data, offset, fmt):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def stage(control, code):
    magic, major, minor = read(control, 0, '<3I')
    declared, = read(control, 0x20, '<I')
    if magic != 0x98761234 or major != 1 or minor not in (9, 16) or declared < 0x748 or declared > len(control):
        raise ValueError('unsupported NVN shader control header')
    control = control[:declared]
    program_size, c1_size, c1_offset, total, offset, _, _, kind = read(control, 0x6f8, '<8I')
    if total != len(code) or kind not in STAGES or offset != (0x100 if kind == 5 else 0x30) or c1_offset < offset + program_size:
        raise ValueError('invalid shader bytecode layout')
    if read(code, 0, '<I')[0] != (0x12345679 if kind == 5 else 0x12345678):
        raise ValueError('invalid shader bytecode header')
    c1 = span(code, c1_offset, c1_size)
    program = span(code, offset, c1_offset - offset)
    result = {'key': hashlib.sha256(control + program + c1).hexdigest()[:16],
            'control_sha256': hashlib.sha256(control).hexdigest(), 'stage': STAGES[kind],
            'control_bytes': len(control), 'c1': c1.hex()}
    if kind == 5:
        x, y, z, shared = read(control, 0x748, '<4I')
        if min(x, y, z) < 1 or x * y * z > 1024 or shared > 49152:
            raise ValueError('invalid compute shader workgroup')
        result.update(local=[x, y, z], shared=shared)
    return result, program


def bnsh(data):
    if data[:8] != b'BNSH\0\0\0\0' or read(data, 12, '<H')[0] != 0xfeff or read(data, 28, '<I')[0] != len(data):
        raise ValueError('invalid BNSH header')
    if span(data, 0x60, 4) != b'grsc':
        raise ValueError('BNSH shader block missing')
    count, table = read(data, 0x7c, '<IQ')
    span(data, table, count * 64)
    for i in range(count):
        program, = read(data, table + i * 64 + 16, '<Q')
        if not program:
            raise ValueError('BNSH binary program missing')
        for header in read(data, program + 8, '<6Q'):
            if not header:
                continue
            control, code, code_size, control_size = read(data, header + 8, '<QQII')
            yield stage(span(data, control, control_size), span(data, code, code_size))


def archive(data):
    if data[:4] == b'\x34\x12\x76\x98':
        # Checked runtime records cover shaders absent from the extracted archives.
        length, = read(data, 0x20, '<I')
        if length < 0x748:
            raise ValueError('invalid captured NVN shader')
        yield stage(span(data, 0, length), span(data, length, len(data) - length))
    elif data[:4] == b'BNSH':
        yield from bnsh(data)
    elif data[:4] == b'FSHA':
        if read(data, 10, '<H')[0] != 9 or read(data, 28, '<I')[0] != len(data):
            raise ValueError('expected BFSHA version 9')
        table, = read(data, 0x48, '<Q')
        count, = read(data, 0x78, '<H')
        span(data, table, count * 256)
        for i in range(count):
            offset, = read(data, table + i * 256 + 0xa8, '<Q')
            size, = read(data, offset + 28, '<I')
            yield from bnsh(span(data, offset, size))
    elif data[:4] == b'BAHS':
        _, version, size, _, endian, alignment, code_size, code_base = read(data, 0, '<8I')
        if version != 9 or size != len(data) or endian or alignment < 2048 or alignment & (alignment - 1):
            raise ValueError('expected little-endian BAHS version 9')
        span(data, code_base, code_size)
        section = 0x150 + alignment
        size, count = read(data, section, '<II')
        span(data, section, size)
        end = section + size
        pos = section + 8
        for _ in range(count):
            size, _, _, payload_size = read(data, pos, '<4I')
            if size < 16 or payload_size != size - 16 or pos + size > end:
                raise ValueError('invalid BAHS variation section')
            payload = span(data, pos + 16, payload_size)
            code, code_pad, control_size, control, control_pad = read(payload, 0, '<iIIiI')
            if code_pad or control_pad:
                raise ValueError('invalid BAHS relative offset padding')
            # BAHS deduplicates control blocks by signed backward references.
            ctrl = span(data, pos + 16 + control, control_size)
            shader_size, = read(ctrl, 0x704, '<I')
            if code + shader_size > code_size:
                raise ValueError('BAHS shader outside code region')
            yield stage(ctrl, span(data, code_base + code, shader_size))
            pos += size
        if pos != end:
            raise ValueError('BAHS variation count mismatch')
    else:
        raise ValueError('unsupported shader archive')


def index(source, target):
    target.mkdir(parents=True, exist_ok=True)
    records, programs = {}, {}
    for path in sorted(source.iterdir()):
        if path.suffix not in ('.bnsh', '.bfsha', '.sharcb', '.nvn'):
            continue
        count = 0
        for entry, program in archive(path.read_bytes()):
            key = entry['key']
            entry['code_sha256'] = hashlib.sha256(program).hexdigest()
            entry['source'] = path.name
            prior = records.get(key)
            if prior and any(prior[k] != entry[k] for k in ('stage', 'code_sha256', 'c1')):
                raise ValueError('shader control hash collision')
            if not prior:
                programs[key] = program
                records[key] = entry
            count += 1
        print(path.name, 'shader records=', count, flush=True)
    for key, program in programs.items():
        (target / (key + '.bin')).write_bytes(program)
    (target.parent / 'index.json').write_text(json.dumps(list(records.values()), indent=2) + '\n')
    labels = {'vs': 'vertex', 'ps': 'pixel', 'cs': 'compute', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry'}
    (target.parent / 'shader_map.tsv').write_text('package\tindex\tstage\tcontrol\n' + ''.join(
        f"{e['source']}\t0\t{labels[e['stage']]}\t{e['key']}\n" for e in records.values()))
    for old in target.glob('*.bin'):
        if len(old.stem) == 16 and all(c in '0123456789abcdef' for c in old.stem) and old.stem not in records:
            old.unlink()
    print('Unique shaders:', len(records))


if __name__ == '__main__':
    index(Path('work/shaders/source'), Path('work/shaders/code'))

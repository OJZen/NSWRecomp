"""Generate typed AAPCS64 Cpu-to-native NVN calls from the pinned public API headers."""
import argparse
import json
from pathlib import Path
import re

from recompile import ROOT, digest, prepare_work
from build import write_json

HEADERS = ROOT / 'third_party/zelda-mobile/core/gfx/nvn'


def types(header):
    header = re.sub(r'/\*.*?\*/|//[^\n]*', '', header, flags=re.S)
    kinds = {'void': 'void', 'int': 'word', 'uint32_t': 'word', 'uint8_t': 'word',
             'uint64_t': 'wide', 'int64_t': 'wide', 'size_t': 'wide', 'ptrdiff_t': 'wide', 'float': 'float'}
    for source, name in re.findall(r'typedef\s+(uint(?:8|32|64)_t|int|void\s*\*)\s*(NVN\w+)\s*;', header):
        kinds[name] = 'pointer' if '*' in source else kinds[source]
    for name in re.findall(r'typedef\s+enum\s*\{[^}]*\}\s*(NVN\w+)\s*;', header):
        kinds[name] = 'word'
    for name in re.findall(r'\(\*\s*(PFN\w+)\)', header):
        kinds[name] = 'pointer'
    # These NVN handles are one-register structs, not pointers to opaque objects.
    kinds.update(NVNseparateTextureHandle='small', NVNseparateSamplerHandle='small')
    return kinds


def kind(type_name, kinds):
    type_name = re.sub(r'\bconst\b', '', type_name).strip()
    if not re.fullmatch(r'\w+(?:\s*\*)*', type_name):
        raise ValueError('unsupported API type spelling')
    return 'pointer' if '*' in type_name else kinds.get(type_name, 'unsupported')


def generate(prototypes, header, names=(), extra_header=None):
    kinds = types(header)
    functions = re.findall(r'typedef\s+([^;\n]+?)\s*\(\*NVNT_(\w+)\)\(([^;]*)\);', prototypes)
    if not functions or len({name for _, name, _ in functions}) != len(functions):
        raise ValueError('missing or duplicate NVN API prototypes')
    known = {name for _, name, _ in functions}
    for name in names:
        if not re.fullmatch(r'nvn\w+', name): raise ValueError('invalid NVN name')
        if name not in known:
            functions.append(('unknown', name, 'void'))
            known.add(name)
    code = ['#include "nvn_bridge.h"', '#include "nvn_protos.h"',
            '_Thread_local Cpu *nvn_bridge_cpu;',
            '_Static_assert(sizeof(void *) == 8, "64-bit host required");']
    if extra_header: code.append(f'#include "{extra_header}"')
    unsupported, small_types = [], set()
    for index, (result, name, parameters) in enumerate(functions):
        arguments = [] if parameters.strip() in ('', 'void') else [part.strip() for part in parameters.split(',')]
        result_kind = kind(result, kinds)
        argument_kinds = [kind(part, kinds) for part in arguments]
        code.append(f'static void call_{name}(Cpu *c) {{')
        if result_kind == 'unsupported' or any(k in ('unsupported', 'void') for k in argument_kinds):
            unsupported.append(name)
            code.extend([f'    nvn_bridge_unavailable(c, "{name}");', '}'])
            continue
        code.extend([f'    NVNT_{name} function = (NVNT_{name})nvn_bridge_native({index});',
                     f'    if (!function) nvn_bridge_unavailable(c, "{name}");',
                     '    Cpu *previous = nvn_bridge_cpu; nvn_bridge_cpu = c;'])
        integer = floating = stack = 0
        names = []
        for i, (argument, category) in enumerate(zip(arguments, argument_kinds)):
            variable = f'a{i}'
            names.append(variable)
            if category == 'float' and floating < 8:
                code.extend([f'    float {variable};', f'    memcpy(&{variable}, &c->v[{floating}], sizeof {variable});'])
                floating += 1
                continue
            if category != 'float' and integer < 8:
                value = f'c->x[{integer}]'
                integer += 1
            else:
                value = f'nvn_bridge_stack(c, {stack})'
                stack += 8
            if category in ('small', 'float'):
                if category == 'small': small_types.add(argument)
                code.extend([f'    uint64_t bits{i} = {value};', f'    {argument} {variable};',
                             f'    memcpy(&{variable}, &bits{i}, sizeof {variable});'])
            else:
                cast = '(uint32_t)' if category == 'word' else '(uintptr_t)' if category == 'pointer' else ''
                code.append(f'    {argument} {variable} = ({argument}){cast}{value};')
        call = f'function({", ".join(names)})'
        if result_kind == 'void':
            code.append(f'    {call};')
        else:
            code.append(f'    {result} result = {call};')
            if result_kind == 'float':
                code.extend(['    c->v[0] = 0;', '    memcpy(&c->v[0], &result, sizeof result);'])
            elif result_kind == 'small':
                small_types.add(result)
                code.extend(['    c->x[0] = 0;', '    memcpy(&c->x[0], &result, sizeof result);'])
            else:
                cast = '(uint32_t)' if result_kind == 'word' else '(uintptr_t)' if result_kind == 'pointer' else '(uint64_t)'
                code.append(f'    c->x[0] = {cast}result;')
        code.extend(['    nvn_bridge_cpu = previous;', '}'])
    for name in sorted(small_types):
        code.append(f'_Static_assert(sizeof({name}) <= 8, "NVN one-register handle ABI");')
    code.append('const NvnBridgeEntry nvn_bridge_entries[] = {')
    code += [f'    {{"{name}", call_{name}}},' for _, name, _ in functions]
    code += ['};', f'const size_t nvn_bridge_count = {len(functions)};']
    return '\n'.join(code) + '\n', unsupported


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    prepare_work()
    output = ROOT / 'work/graphics'
    if output.is_symlink(): parser.error('graphics output must not be a symlink')
    output.mkdir(exist_ok=True)
    for name in ('nvn_bridge.c', 'nvn_bridge.json', 'nvn_bridge.json.partial'):
        if (output / name).is_symlink(): parser.error('graphics output file must not be a symlink')
    source, unsupported = generate((HEADERS / 'nvn_protos.h').read_text(), (HEADERS / 'include/nvn.h').read_text())
    (output / 'nvn_bridge.c').write_text(source)
    write_json(output / 'nvn_bridge.json', {'unsupported_abi': unsupported,
               'headers': {name: digest(HEADERS / name) for name in ('nvn_protos.h', 'include/nvn.h')},
               'generator_sha256': digest(Path(__file__)), 'source_sha256': digest(output / 'nvn_bridge.c')})
    print(json.dumps({'source_bytes': len(source), 'unsupported_abi': unsupported}))

"""Bind explicit, fingerprinted completion-callback mutex guards at link time."""
import hashlib
import json
import re

from load import contains, symbol_scope


def generate(path, modules, fingerprints):
    with path.open('rb') as stream:
        raw = stream.read(65537)
    if len(raw) > 65536:
        raise ValueError('callback guard configuration exceeds bound')
    config = json.loads(raw)
    if (not isinstance(config, dict) or set(config) != {'schema', 'guards'} or
            type(config['schema']) is not int or config['schema'] != 1 or
            not isinstance(config['guards'], list) or not 1 <= len(config['guards']) <= 16):
        raise ValueError('invalid callback guard configuration')
    scope = symbol_scope(modules)
    by_name = {m.name: m for m in modules}
    fields = {'module', 'module_sha256', 'function_offset', 'function_size', 'function_sha256',
              'context_pointer_offset', 'mutex_offset', 'mutex_size', 'lock_symbol', 'unlock_symbol'}
    bindings = []
    for item in config['guards']:
        if not isinstance(item, dict) or set(item) != fields:
            raise ValueError('invalid callback guard fields')
        module = by_name.get(item['module']) if isinstance(item['module'], str) else None
        if module is None or item['module_sha256'] != fingerprints.get(module.name):
            raise ValueError('callback guard module fingerprint differs from the selected input')
        offset = item['function_offset']
        if not isinstance(offset, str) or not re.fullmatch(r'0x[0-9a-f]+', offset):
            raise ValueError('invalid callback function offset')
        offset = int(offset, 16)
        size = item['function_size']
        if (offset % 4 or type(size) is not int or not 4 <= size <= 65536 or size % 4 or
                not contains(*module.nso.text, offset, size)):
            raise ValueError('callback function span is outside text')
        if hashlib.sha256(module.nso.image[offset:offset + size]).hexdigest() != item['function_sha256']:
            raise ValueError('callback function fingerprint mismatch')
        for field, maximum, alignment in (('context_pointer_offset', 4096, 8), ('mutex_offset', 4096, 4), ('mutex_size', 256, 4)):
            value = item[field]
            if type(value) is not int or not 0 <= value <= maximum or value % alignment or (field == 'mutex_size' and not value):
                raise ValueError('invalid callback object layout')
        helpers = []
        for field in ('lock_symbol', 'unlock_symbol'):
            name = item[field]
            binding = scope.get(name.encode()) if isinstance(name, str) else None
            if not binding:
                raise ValueError('callback mutex helper symbol is missing')
            owner, symbol = binding
            if symbol[2] != 2 or symbol[4] == 0xfff1 or not contains(*owner.nso.text, symbol[5], 4):
                raise ValueError('callback mutex helper is not executable code')
            helpers.append(owner.defined_value(symbol))
        if helpers[0] == helpers[1]:
            raise ValueError('callback lock and unlock helpers must be distinct')
        bindings.append(dict(address=module.base + offset, context_pointer_offset=item['context_pointer_offset'],
                             mutex_offset=item['mutex_offset'], mutex_size=item['mutex_size'],
                             lock_function=helpers[0], unlock_function=helpers[1]))
    addresses = {b['address'] for b in bindings}
    if len(addresses) != len(bindings) or any(b[k] in addresses for b in bindings for k in ('lock_function', 'unlock_function')):
        raise ValueError('duplicate or recursive callback guard target')
    fingerprint = hashlib.sha256(raw).hexdigest()
    source = ['#include "callback_guard.h"', f'const char callback_guards_sha256[] = "{fingerprint}";']
    flags = []
    for b in bindings:
        name = f"f_{b['address']:x}"
        source += [f'extern void __real_{name}(Cpu *);', f'void __wrap_{name}(Cpu *c) {{',
                   '    static const CallbackMutexGuard guard = {' +
                   ', '.join(f'.{k} = {v:#x}ull' for k, v in b.items() if k != 'address') + '};',
                   f'    callback_mutex_guard(c, __real_{name}, &guard);', '}']
        flags.append(f'-Wl,--wrap={name}')
    source = '\n'.join(source) + '\n'
    receipt = dict(config_sha256=fingerprint, source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                   bindings=bindings, input_sha256=fingerprints)
    return source, flags, receipt

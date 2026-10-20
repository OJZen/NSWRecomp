"""Lower ballot-only subgroup use in vertex SPIR-V to a subgroup of one invocation.

Metal vertex functions have no SIMD-group lane builtins, so SPIRV-Cross rejects vertex shaders that read the
subgroup invocation id (v1.4.3 AglShader 159dd8d89a8d1afd counts drawn vertices with a ballot-aggregated
atomicAdd). A subgroup of size one is a valid Vulkan subgroup: ballot(p) becomes p ? (1,0,0,0) : 0, the
invocation id 0, the size 1 and the masks follow. Aggregations built from these (leader atomics, counts) then give
the same totals. Shaders with any other group operation (shuffles, broadcasts by index, arithmetic) are left
untouched and stay blocked rather than being translated with different cross-lane results.
"""
import struct

OP_CAPABILITY, OP_EXTENSION, OP_ENTRY_POINT, OP_NAME, OP_DECORATE = 17, 10, 15, 5, 71
OP_TYPE_INT, OP_TYPE_VECTOR, OP_CONSTANT, OP_CONSTANT_COMPOSITE = 21, 23, 43, 44
OP_FUNCTION, OP_VARIABLE, OP_LOAD, OP_SELECT, OP_COPY_OBJECT = 54, 59, 61, 169, 83
BALLOT, BALLOT_KHR = 339, 4421
GROUP_OPS = set(range(333, 367)) | set(range(4421, 4433))
SUBGROUP_CAPABILITIES = {61, 64, 4423}  # GroupNonUniform, GroupNonUniformBallot, SubgroupBallotKHR
BUILTIN = 11
# Builtin -> value of a one-invocation subgroup (scalar uint, or uvec4 mask words).
BUILTIN_VALUES = {36: 1, 41: 0, 4416: (1, 0, 0, 0), 4417: (1, 0, 0, 0), 4418: (0, 0, 0, 0), 4419: (1, 0, 0, 0), 4420: (0, 0, 0, 0)}


def instructions(words):
    i = 5
    while i < len(words):
        count = words[i] >> 16
        if not count or i + count > len(words):
            raise ValueError('malformed SPIR-V instruction stream')
        yield i, words[i] & 0xffff, list(words[i + 1:i + count])
        i += count


def encode(opcode, operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def string_words(operands):
    # Literal strings occupy words up to and including the one holding the terminating zero byte.
    for n, word in enumerate(operands):
        if any((word >> shift) & 0xff == 0 for shift in (0, 8, 16, 24)):
            return n + 1
    raise ValueError('unterminated SPIR-V string')


def lower(data):
    """Returns rewritten SPIR-V, or None when the module has nothing to lower or uses other group operations."""
    if len(data) % 4 or len(data) < 20:
        raise ValueError('SPIR-V size')
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if words[0] != 0x07230203:
        raise ValueError('SPIR-V magic')
    listing = list(instructions(words))
    builtins = {ops[0]: ops[2] for _, op, ops in listing if op == OP_DECORATE and len(ops) >= 3 and ops[1] == BUILTIN}
    lane_vars = {var: kind for var, kind in builtins.items() if kind in BUILTIN_VALUES}
    group = [op for _, op, _ in listing if op in GROUP_OPS]
    if not lane_vars and not group:
        return None
    if any(op not in (BALLOT, BALLOT_KHR) for op in group):
        return None
    types = {ops[0]: (op, ops) for _, op, ops in listing if op in (OP_TYPE_INT, OP_TYPE_VECTOR)}
    loads, other_uses = {}, False
    for _, op, ops in listing:
        if op == OP_LOAD and ops[2] in lane_vars:
            loads[ops[1]] = (ops[0], lane_vars[ops[2]])
        elif op not in (OP_DECORATE, OP_NAME, OP_ENTRY_POINT, OP_VARIABLE) and any(v in lane_vars for v in ops[2:] if op != OP_LOAD):
            other_uses = True
    if other_uses:
        return None
    bound = words[3]
    constants, new = {}, []

    def constant(type_id, value):
        nonlocal bound
        key = (type_id, value)
        if key not in constants:
            kind, ops = types.get(type_id, (None, None))
            if kind == OP_TYPE_INT and isinstance(value, int):
                constants[key] = bound; new.extend(encode(OP_CONSTANT, [type_id, bound, value])); bound += 1
            elif kind == OP_TYPE_VECTOR and isinstance(value, tuple) and ops[2] == len(value):
                parts = [constant(ops[1], v) for v in value]
                constants[key] = bound; new.extend(encode(OP_CONSTANT_COMPOSITE, [type_id, bound, *parts])); bound += 1
            else:
                raise ValueError('unsupported subgroup value type')
        return constants[key]

    replaced = {}
    for index, op, ops in listing:
        if op in (BALLOT, BALLOT_KHR):
            result_type, result, predicate = ops[0], ops[1], ops[-1]
            replaced[index] = encode(OP_SELECT, [result_type, result, predicate, constant(result_type, (1, 0, 0, 0)),
                                                 constant(result_type, (0, 0, 0, 0))])
        elif op == OP_LOAD and ops[1] in loads:
            result_type, kind = loads[ops[1]]
            replaced[index] = encode(OP_COPY_OBJECT, [result_type, ops[1], constant(result_type, BUILTIN_VALUES[kind])])
    out = words[:5]
    first_function = next(index for index, op, _ in listing if op == OP_FUNCTION)
    for index, op, ops in listing:
        if index == first_function:
            out.extend(new)
        if index in replaced:
            out.extend(replaced[index]); continue
        if op == OP_CAPABILITY and ops[0] in SUBGROUP_CAPABILITIES:
            continue
        if op == OP_EXTENSION and struct.pack('<%dI' % len(ops), *ops).split(b'\0')[0] == b'SPV_KHR_shader_ballot':
            continue
        if op in (OP_DECORATE, OP_NAME) and ops[0] in lane_vars:
            continue
        if op == OP_VARIABLE and ops[1] in lane_vars:
            continue
        if op == OP_ENTRY_POINT:
            name = string_words(ops[2:])
            ops = ops[:2 + name] + [v for v in ops[2 + name:] if v not in lane_vars]
        out.extend(encode(op, ops))
    out[3] = bound
    return struct.pack('<%dI' % len(out), *out)

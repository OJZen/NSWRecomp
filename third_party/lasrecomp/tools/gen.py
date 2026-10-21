"""Generate C for a whole NSO module.

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--chunk N] [--simd asm|c]

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
"""
import argparse
import bisect
import os
import platform
import re
import struct
import sys
import time
from array import array
from collections import Counter, OrderedDict

import capstone
from capstone import arm64_const as C
import unicorn as uc
from unicorn import arm64_const as A

from analyze import eh_frame_functions
from lifter import Insn, Lifter, Unsupported, bits, emit_asm_batch, exclusive_pair_transaction, hx, sext
from nso import Nso, R_AARCH64_RELATIVE

SCRATCH_MEM = 0x7300000000


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


class Module:
    def __init__(self, name, path, base, simd='asm', game_patches=True, cache_limit=0):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = array('I')
        self.words.frombytes(n.image[n.text[0]:n.text[0] + n.text[1]])
        if sys.byteorder != 'little':
            self.words.byteswap()
        self.lifter = Lifter(base, simd)
        self.cache = OrderedDict() if cache_limit else {}
        self.cache_limit = cache_limit
        self.unsupported = set()
        self.patches, self.hooks, self.rando_code = load_game_patches() if game_patches else ({}, {}, {})
        self.stats = Counter()
        self.mu = None
        self.table_probe = [False, False]
        self.function_spans = None
        self.table_disasm = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
        self.table_disasm.detail = True

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.unsupported.add(a)
                self.stats['unsupported'] = len(self.unsupported)
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
            if self.cache_limit and len(self.cache) >= self.cache_limit:
                self.cache.popitem(last=False)
            self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(b + value)
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(b + add)
        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        if self.mu is None:
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            n = self.nso
            mu.mem_map(self.base, len(n.image))
            mu.mem_write(self.base, bytes(n.image))
            mu.mem_map(SCRATCH_MEM, 0x100000)
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
            probe = self.table_probe
            def read_memory(machine, access, address, size, value, userdata):
                if (probe[0] and SCRATCH_MEM <= address < SCRATCH_MEM + 0x100000 and
                        machine.reg_read(A.UC_ARM64_REG_PC) in probe[0]):
                    probe[1] = True
            mu.hook_add(uc.UC_HOOK_MEM_READ, read_memory)
            self.mu = mu
        return self.mu

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None."""
        targets = self._state_table(br_pc, entry)
        if targets is not None: return targets
        # Preserve a valid local context; earlier memory loads may not be emulatable.
        starts = [entry]
        earlier = max(self.lo, br_pc - 4 * 32)
        if earlier < entry:
            starts.append(earlier)
        for start in starts:
            r = self._jump_table_fallthrough(br_pc, start, end)
            if r is None:
                r = self._jump_table_taken(br_pc, start, end)
            if r is not None:
                return r
        if self.computed_target(br_pc, entry):
            bounds = self._original_bounds(br_pc)
            if bounds and bounds[0] < starts[-1] and br_pc - bounds[0] <= 4 * 4096:
                r = self._jump_table_fallthrough(br_pc, bounds[0], end)
                if r is None:
                    r = self._jump_table_taken(br_pc, bounds[0], end)
                if r is not None:
                    return r
        return None

    def _original_bounds(self, br_pc):
        if self.function_spans is None:
            starts = sorted(self.base + a for a in eh_frame_functions(self.nso) if self.in_text(self.base + a))
            spans = dict(zip(starts, starts[1:]))
            for _, info, section, value, size in self.nso.syms:
                a, end = self.base + value, self.base + value + size
                if section and info & 15 == 2 and size and self.in_text(a) and a < end <= self.hi:
                    spans[a] = min(spans.get(a, end), end)
            self.function_spans = sorted(spans.items())
            self.function_starts = [a for a, _ in self.function_spans]
        index = bisect.bisect_right(self.function_starts, br_pc) - 1
        if index < 0:
            return None
        low, high = self.function_spans[index]
        if not low <= br_pc < high:
            return None
        return low, high

    @staticmethod
    def _register_access(instruction):
        reads, writes = instruction.regs_access()
        if instruction.id in (C.ARM64_INS_CMP, C.ARM64_INS_CMN, C.ARM64_INS_TST):
            # Some Capstone versions retain the removed destination's access on these aliases.
            operands = {o.reg for o in instruction.operands if o.type == C.ARM64_OP_REG}
            return sorted(set(reads) | operands), sorted(set(writes) - operands)
        return reads, writes

    def _table_compare(self, pc, condition):
        """Index/count for an unsigned table guard; CCMP's fallback must skip the table."""
        word = self.word(pc)
        if (word & 0x7F80001F) == 0x7100001F and not bits(word, 22, 1):
            index, limit = bits(word, 5, 5), bits(word, 10, 12)
        else:
            instruction = next(self.table_disasm.disasm(bytes(self.nso.image[pc - self.base:pc - self.base + 4]), pc), None)
            if instruction is None or instruction.id != C.ARM64_INS_CCMP: return None
            args = instruction.operands
            if len(args) != 3 or args[1].type != C.ARM64_OP_IMM or args[2].type != C.ARM64_OP_IMM: return None
            flags = args[2].imm
            carry, zero = bool(flags & 2), bool(flags & 4)
            taken = {2: carry, 3: not carry, 8: carry and not zero, 9: not carry or zero}[condition]
            if taken != (condition in (2, 8)): return None
            index, limit = bits(word, 5, 5), args[1].imm
        count = limit + (condition in (8, 9))
        return (index, count) if index < 31 and 0 < count <= 4096 else None

    def _guard_comparison(self, guard, entry):
        condition = bits(self.word(guard), 0, 4)
        for pc in range(guard - 4, max(entry, guard - 4 * 32) - 1, -4):
            comparison = self._table_compare(pc, condition)
            if comparison is not None: return (pc, *comparison)
            instruction = next(self.table_disasm.disasm(bytes(self.nso.image[pc - self.base:pc - self.base + 4]), pc), None)
            if (instruction is None or instruction.update_flags or
                    self.lift(pc).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap')):
                return None
        return None

    def _state_table(self, br_pc, entry):
        """Resolve a readonly relative table from constants, guard or original function bounds."""
        if not self.computed_target(br_pc, entry):
            return None
        bounds = self._original_bounds(br_pc)
        if bounds is None: return None
        low, high = bounds
        start = max(low, br_pc - 4 * 64)
        instructions = list(self.table_disasm.disasm(bytes(self.nso.image[start - self.base:br_pc - self.base]), start))
        if len(instructions) * 4 != br_pc - start:
            return None
        block_start = max((i + 1 for i, instruction in enumerate(instructions)
                           if self.lift(instruction.address).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap')), default=0)
        block_address = instructions[block_start].address if block_start < len(instructions) else br_pc
        guards = []
        for i, instruction in enumerate(instructions):
            word = self.word(instruction.address)
            if (word & 0xFF000010) == 0x54000000 and bits(word, 0, 4) in (2, 3, 8, 9):
                destination = instruction.address + sext(bits(word, 5, 19), 19) * 4
                if i >= block_start or block_address <= destination <= br_pc: guards.append(i)

        def reg(register):
            name = self.table_disasm.reg_name(register)
            if name in ('fp', 'lr'): return 29 if name == 'fp' else 30
            return int(name[1:]) if re.fullmatch('[xw][0-9]+', name or '') else None

        def writer(register, before):
            if register is None: return None
            for i in range(before - 1, -1, -1):
                instruction = instructions[i]
                if instruction.group(capstone.CS_GRP_JUMP) or instruction.group(capstone.CS_GRP_CALL) or instruction.group(capstone.CS_GRP_RET):
                    return None
                if register in (reg(r) for r in self._register_access(instruction)[1]):
                    return i
            return None

        def constant(register, before):
            i = writer(register, before)
            if i is None: return None
            instruction = instructions[i]
            operands = instruction.operands
            if instruction.id in (C.ARM64_INS_ADR, C.ARM64_INS_ADRP): return operands[1].imm
            if (instruction.id == C.ARM64_INS_ADD and len(operands) == 3 and
                    operands[2].type == C.ARM64_OP_IMM and operands[2].shift.value in (0, 12)):
                value = constant(reg(operands[1].reg), i)
                if value is not None:
                    value = value + (operands[2].imm << operands[2].shift.value)
                    return value & (0xffffffff if self.table_disasm.reg_name(operands[0].reg).startswith('w') else 0xffffffffffffffff)
            return None

        add_at = writer(bits(self.word(br_pc), 5, 5), len(instructions))
        if add_at is None: return None
        add = instructions[add_at]
        operands = add.operands
        if (add.id != C.ARM64_INS_ADD or len(operands) != 3 or
                any(o.type != C.ARM64_OP_REG for o in operands) or
                operands[2].ext not in (0, C.ARM64_EXT_UXTW, C.ARM64_EXT_SXTW) or
                operands[2].shift.type not in (0, C.ARM64_SFT_LSL) or operands[2].shift.value > 3):
            return None
        case_base = constant(reg(operands[1].reg), add_at)
        load_at = writer(reg(operands[2].reg), add_at)
        if case_base is None or load_at is None: return None
        load = instructions[load_at]
        widths = {C.ARM64_INS_LDRB: 1, C.ARM64_INS_LDRH: 2, C.ARM64_INS_LDRSW: 4}
        width = widths.get(load.id)
        if not width or load.writeback or len(load.operands) != 2: return None
        memory = load.operands[1]
        if (memory.type != C.ARM64_OP_MEM or not memory.mem.index or memory.mem.disp or
                memory.shift.type not in (0, C.ARM64_SFT_LSL) or 1 << memory.shift.value != width):
            return None
        indexed = reg(memory.mem.index)
        mask_instructions = []
        if br_pc - low <= 4 * 32768:
            mask_instructions = list(self.table_disasm.disasm(bytes(self.nso.image[low - self.base:br_pc - self.base]), low))
        mask_load_at = (load.address - low) // 4
        if len(mask_instructions) * 4 != br_pc - low: mask_instructions = []
        def mask_count(instruction):
            args = instruction.operands
            if (instruction.id == C.ARM64_INS_AND and len(args) == 3 and reg(args[0].reg) == indexed and
                    args[2].type == C.ARM64_OP_IMM and 0 < args[2].imm < 4096 and
                    not (args[2].imm & (args[2].imm + 1))):
                return args[2].imm + 1
            return None

        masks = {i: count for i, instruction in enumerate(mask_instructions[:mask_load_at])
                 if (count := mask_count(instruction)) is not None}
        local_mask = writer(indexed, load_at)
        masked_count = mask_count(instructions[local_mask]) if local_mask is not None else None
        state_indices, flow_tables = None, None
        if mask_instructions and (masks or constant(reg(memory.mem.base), load_at) is None):
            # A sibling case can end in BR between the mask and this table's direct predecessor.
            positions = {instruction.address: i for i, instruction in enumerate(mask_instructions)}
            predecessors = [set() for _ in mask_instructions]
            for i, instruction in enumerate(mask_instructions):
                if i + 1 < len(mask_instructions) and self.lift(instruction.address).kind not in ('b', 'br', 'ret', 'trap'):
                    predecessors[i + 1].add(i)
                if instruction.group(capstone.CS_GRP_JUMP) and instruction.operands[-1].type == C.ARM64_OP_IMM:
                    target = positions.get(instruction.operands[-1].imm)
                    if target is not None: predecessors[target].add(i)
            successors = [set() for _ in mask_instructions]
            for at, incoming in enumerate(predecessors):
                for previous in incoming: successors[previous].add(at)
            pending, reachable = [0], set()
            while pending:
                at = pending.pop()
                if at in reachable: continue
                reachable.add(at)
                pending.extend(successors[at] - reachable)
            for incoming in predecessors: incoming.intersection_update(reachable)
            def reaching_definitions(before, register=indexed):
                pending, seen, definitions = [before], set(), set()
                if before not in reachable: return None
                while pending:
                    at = pending.pop()
                    if at in seen: continue
                    seen.add(at)
                    if not predecessors[at]: return None
                    for previous in predecessors[at]:
                        instruction = mask_instructions[previous]
                        if instruction.group(capstone.CS_GRP_CALL): return None
                        if register in (reg(r) for r in self._register_access(instruction)[1]): definitions.add(previous)
                        else: pending.append(previous)
                return definitions or None

            values_cache = {}
            def address_values(register, before, seen=frozenset()):
                key = (register, before)
                if key in values_cache: return values_cache[key]
                if register is None or key in seen or len(seen) >= 64: return None
                definitions = reaching_definitions(before, register)
                if not definitions: return None
                values = set()
                for at in definitions:
                    instruction = mask_instructions[at]
                    args = instruction.operands
                    if instruction.id in (C.ARM64_INS_ADR, C.ARM64_INS_ADRP):
                        values.add(args[1].imm)
                    elif (instruction.id == C.ARM64_INS_ADD and len(args) == 3 and
                          args[2].type == C.ARM64_OP_IMM and args[2].shift.value in (0, 12)):
                        source = address_values(reg(args[1].reg), at, seen | {key})
                        if source is None: return None
                        mask = 0xffffffff if self.table_disasm.reg_name(args[0].reg).startswith('w') else 0xffffffffffffffff
                        values.update((v + (args[2].imm << args[2].shift.value)) & mask for v in source)
                    elif instruction.id == C.ARM64_INS_MOV and len(args) == 2 and args[1].type == C.ARM64_OP_REG:
                        source = address_values(reg(args[1].reg), at, seen | {key})
                        if source is None: return None
                        mask = 0xffffffff if self.table_disasm.reg_name(args[0].reg).startswith('w') else 0xffffffffffffffff
                        values.update(v & mask for v in source)
                    else: return None
                    if len(values) > 64: return None
                values_cache[key] = values
                return values

            flow_tables = address_values(reg(memory.mem.base), mask_load_at)
            definitions = reaching_definitions(mask_load_at)
            domain, complete = set(), bool(definitions)
            for at in definitions or ():
                if at in masks:
                    domain.update(range(masks[at]))
                    continue
                instruction = mask_instructions[at]
                args = instruction.operands
                if (instruction.id in (C.ARM64_INS_ADD, C.ARM64_INS_SUB) and len(args) == 3 and
                        args[1].type == C.ARM64_OP_REG and reg(args[1].reg) == indexed and args[2].type == C.ARM64_OP_IMM):
                    sources = reaching_definitions(at)
                    if sources and sources <= masks.keys():
                        offset = args[2].imm << args[2].shift.value
                        if instruction.id == C.ARM64_INS_SUB: offset = -offset
                        bits_ = 32 if self.table_disasm.reg_name(args[0].reg).startswith('w') else 64
                        domain.update((value + offset) & ((1 << bits_) - 1)
                                      for source in sources for value in range(masks[source]))
                        continue
                complete = False
                break
            if complete: state_indices = domain
            counts = {masks[at] for at in definitions or () if at in masks}
            if definitions and definitions <= masks.keys() and len(counts) == 1:
                masked_count = counts.pop()
        table_register = reg(memory.mem.base)
        table = constant(table_register, load_at)
        if table is None and flow_tables and len(flow_tables) == 1:
            table = next(iter(flow_tables))
        if table is None and not flow_tables and table_register is not None and br_pc - low <= 4 * 4096:
            try:
                mu = self._table_setup(low, load.address)
                table = mu.reg_read(self._gpr(table_register))
            except uc.UcError:
                return None
        ro_start = self.base + self.nso.rodata[0]
        ro_end = ro_start + self.nso.rodata[1]
        tables = {table} if table is not None else flow_tables or set()
        if any(not ro_start <= t < ro_end or t % width for t in tables): return None

        def bounded_targets(count):
            if not tables: return None
            targets = set()
            for table in tables:
                if table + count * width > ro_end: return None
                for i in range(count):
                    at = table + i * width - self.base
                    offset = int.from_bytes(self.nso.image[at:at + width], 'little', signed=width == 4)
                    target = case_base + (offset << operands[2].shift.value)
                    if not self.in_text(target): return None
                    targets.add(target)
            return sorted(targets)

        if masked_count:
            targets = bounded_targets(masked_count) if tables else self._emulate_table(
                br_pc, low, high, load.address, indexed, masked_count, load.address,
                readonly_table=(table_register, width))
            if targets is not None:
                self.stats['masked_tables'] += 1
                return targets

        def state_targets():
            if not tables: return None
            targets = set()
            for table in tables:
                candidates = set()
                for i in range(4096):
                    at = table + i * width
                    if at + width > ro_end: break
                    offset = int.from_bytes(self.nso.image[at - self.base:at - self.base + width], 'little', signed=width == 4)
                    target = case_base + (offset << operands[2].shift.value)
                    if not low <= target < high or not self.in_text(target): break
                    if state_indices is None or i in state_indices: candidates.add(target)
                else:
                    return None
                if len(candidates) < 2: return None
                targets.update(candidates)
            self.stats['state_tables'] += 1
            return sorted(targets)

        if guards:
            guard_at = guards[-1]
            guard_word = self.word(instructions[guard_at].address)
            condition = bits(guard_word, 0, 4)
            destination = instructions[guard_at].address + sext(bits(guard_word, 5, 19), 19) * 4
            if (condition not in (2, 8) or guard_at >= load_at or
                    instructions[guard_at].address < destination <= br_pc or
                    any(i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or i.group(capstone.CS_GRP_RET)
                        for i in instructions[guard_at + 1:])):
                return None
            comparison = self._guard_comparison(instructions[guard_at].address, start)
            if comparison is None: return None
            compare_pc, compared, count = comparison
            compare_at = (compare_pc - start) // 4
            indexed = reg(memory.mem.index)
            if indexed is None: return None
            required_mask = (1 << (count - 1).bit_length()) - 1

            def copy_source(at):
                copy = instructions[at]
                args = copy.operands
                if copy.id == C.ARM64_INS_MOV and len(args) == 2 and args[1].type == C.ARM64_OP_REG:
                    return reg(args[1].reg)
                if (copy.id == C.ARM64_INS_AND and len(args) == 3 and args[2].type == C.ARM64_OP_IMM and
                        args[2].imm & required_mask == required_mask):
                    return reg(args[1].reg)
                return None

            def saved_alias(register, seen):
                if register == compared: return True
                if register is None or register in seen: return False
                at = writer(register, compare_at)
                if at is None: return False
                source = copy_source(at)
                if source is None or any(source in (reg(r) for r in self._register_access(i)[1])
                                         for i in instructions[at + 1:compare_at]): return False
                return saved_alias(source, seen | {register})

            before, value = load_at, indexed
            resolved = False
            while value is not None:
                writes = [i for i in range(compare_at + 1, before)
                          if value in (reg(r) for r in self._register_access(instructions[i])[1])]
                if not writes:
                    resolved = saved_alias(value, set())
                    break
                before = writes[-1]
                value = copy_source(before)
            if not resolved:
                if indexed != compared and not any(indexed in (reg(r) for r in self._register_access(i)[1])
                                                   for i in instructions[compare_at + 1:load_at]):
                    return state_targets()
                return None
            if not tables:
                if br_pc - low > 4 * 4096: return None
                return self._emulate_table(br_pc, low, high, load.address, reg(memory.mem.index), count,
                                           load.address, readonly_table=(reg(memory.mem.base), width))
            targets = bounded_targets(count)
            if targets is None: return None
            self.stats['static_guard_tables'] += 1
            return targets
        return state_targets()

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    @staticmethod
    def _gpr(register):
        if 0 <= register < 29: return A.UC_ARM64_REG_X0 + register
        if register == 29: return A.UC_ARM64_REG_X29
        if register == 30: return A.UC_ARM64_REG_X30
        raise ValueError('invalid general register')

    def _table_setup(self, entry, before):
        mu = self.emu()
        self.table_probe[:] = [False, False]
        for register in range(31): mu.reg_write(self._gpr(register), SCRATCH_MEM + 0x80000)
        mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
        mu.reg_write(A.UC_ARM64_REG_NZCV, 0)
        for pc in range(entry, before, 4):
            word = self.word(pc)
            if (word & 0x1F000000) == 0x10000000 or (word & 0xFF800000) == 0x91000000:
                mu.emu_start(pc, 0, count=1)
        return mu

    def _set_index(self, mu, regs, i):
        for r in regs: mu.reg_write(self._gpr(r), i)

    def _table_reads(self, br_pc, start):
        """Memory reads contributing to the branch target, excluding unrelated scheduled loads."""
        def register(value):
            name = self.table_disasm.reg_name(value)
            if name in ('fp', 'lr', 'wsp'): return {'fp': 'x29', 'lr': 'x30', 'wsp': 'sp'}[name]
            return 'x' + name[1:] if re.fullmatch('w[0-9]+', name or '') else name
        needed, reads = {f'x{bits(self.word(br_pc), 5, 5)}'}, set()
        instructions = list(self.table_disasm.disasm(bytes(self.nso.image[start - self.base:br_pc - self.base]), start))
        for instruction in reversed(instructions):
            inputs, outputs = self._register_access(instruction)
            written = {register(r) for r in outputs}
            if needed & written:
                needed.difference_update(written)
                needed.update(register(r) for r in inputs)
                if any(o.type == C.ARM64_OP_MEM for o in instruction.operands): reads.add(instruction.address)
        return reads

    def _emulate_table(self, br_pc, entry, end, start, idx, count, set_at, readonly_table=None):
        """Run start..br for each index value (set at address set_at), collect targets."""
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)
        if rn >= 31: return None

        def step_to(frm, to):
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                if self.table_probe[1]: return False
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        table_reads = self._table_reads(br_pc, set_at)
        targets = []
        try:
            for i in range(count):
                mu = self._table_setup(entry, start)
                if set_at != start and not step_to(start, set_at):
                    return None
                self._set_index(mu, [idx], i)
                if readonly_table is not None:
                    register, width = readonly_table
                    if register is None or register >= 31: return None
                    table = mu.reg_read(self._gpr(register))
                    ro_start = self.base + self.nso.rodata[0]
                    if table % width or not ro_start <= table <= ro_start + self.nso.rodata[1] - count * width:
                        return None
                self.table_probe[0] = table_reads
                if not step_to(set_at, br_pc):
                    return None
                t = mu.reg_read(self._gpr(rn))
                if not self.in_text(t):
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        finally:
            self.table_probe[0] = False
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, entry, end):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(entry, end, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            comparison = self._guard_comparison(g, entry)
            if comparison is None: continue
            _, index, count = comparison
            return self._emulate_table(br_pc, entry, end, s, index, count, s)
        return None

    def _jump_table_fallthrough(self, br_pc, entry, end):
        rn = bits(self.word(br_pc), 5, 5)
        if rn >= 31: return None
        # find the guarding b.hi / b.hs and its cmp, scanning backwards
        a = br_pc - 4
        guard = cmp = None
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
                return None
            a -= 4
        if guard is None:
            return None
        comparison = self._guard_comparison(guard, entry)
        if comparison is None: return None
        cmp, idx, count = comparison
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        idx_regs = self._index_regs(start, cmp, idx)
        mu = self.emu()
        table_reads = self._table_reads(br_pc, cmp)
        targets = []

        def step_to(frm, to):
            # single-step: Unicorn's `until` is unreliable mid translation-block
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                if self.table_probe[1]: return False
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        try:
            for i in range(count):
                mu = self._table_setup(entry, start)
                if not step_to(start, cmp):
                    return None
                self._set_index(mu, idx_regs, i)
                self.table_probe[0] = table_reads
                if not step_to(cmp, br_pc):
                    return None
                t = mu.reg_read(self._gpr(rn))
                if not self.in_text(t):
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        finally:
            self.table_probe[0] = False
        return sorted(set(targets))

    # ------------------------------------------------------------ functions

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            instruction = next(self.table_disasm.disasm(bytes(self.nso.image[a - self.base:a - self.base + 4]), a), None)
            if instruction is None: return False
            names = (f'x{rn}', f'w{rn}', {29: 'fp', 30: 'lr'}.get(rn))
            if any(self.table_disasm.reg_name(r) in names for r in self._register_access(instruction)[1]):
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    labels.update(t for t in jt if entry <= t < end)
                    ext.update(t for t in jt if not entry <= t < end)
                    self.stats['jumptables'] += 1
                elif self.computed_target(a, entry):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
                    self.stats['br_all_labels'] += 1
                else:
                    self.stats['br_dispatch'] += 1
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);']
        if k == 'br':
            return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle):
        out = [f'void {fname(entry)}(Cpu *c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out
        out.append(f'    FN_ENTER({hx(entry)});')
        batch = []
        refs = set()

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        # Functions with jump tables also get the all-instruction fallback: a table whose emulation
        # missed some cases then still reaches the right instruction instead of aborting.
        all_labels = bool(tables)
        if all_labels:
            labels = set(range(entry, end, 4))
        rando_here = [a for a in range(entry, end, 4) if a - self.base in self.rando_code]
        if rando_here:
            labels = set(labels)
            for a in rando_here:
                for w in self.rando_code[a - self.base]:
                    v = self.lifter.lift(w, a)
                    if v.kind in ('b', 'bcond') and isinstance(v.target, int) and entry <= v.target < end:
                        labels.add(v.target)
        hooks_here = [a for a in range(entry, end, 4) if a in self.hooks]
        if hooks_here:  # (a hook's jump targets need their labels)
            labels = set(labels)
            for a in hooks_here:
                for t in self.hooks[a][1]:
                    if not entry <= t < end:
                        raise ValueError(f'hook {a:#x}: target {t:#x} is outside its function')
                    labels.add(t)
        last = None
        fused_tail = None
        for a in range(entry, end, 4):
            if a == fused_tail:
                continue
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a - self.base in self.rando_code:  # randomizer patch site: the active seed picks the instruction
                flush()
                out.append(f'    {{ extern uint32_t rando_word(uint64_t); uint32_t rw = rando_word({hx(a)});')
                first = True
                for w in self.rando_code[a - self.base]:
                    v = self.lifter.lift(w, a)
                    out.append(f'    {"if" if first else "else if"} (rw == {hx(w)}) {{ ' + ' '.join(self.render_single(v, a, jump, refs)) + ' }')
                    first = False
                out.append('    else { ' + ' '.join(self.render_single(ins, a, jump, refs)) + ' } }')
                last = 'op'
                continue
            if a in self.hooks:  # hook site (tools/patches.py HOOKS): the runtime decides, the instruction stays as it is
                flush()
                word, targets, _ = self.hooks[a]
                w = struct.unpack_from('<I', self.nso.image, a - self.base)[0]
                if w != word:
                    raise ValueError(f'hook {a:#x}: instruction {w:#010x}, expected {word:#010x}')
                body = ' '.join(self.render_single(ins, a, jump, refs))
                jumps = ' '.join(f'else if (h_ == {k + 2}) goto L_{t:x};' for k, t in enumerate(targets))
                out.append(f'    {{ int h_ = rt_hook(c, {hx(a)}); if (h_ == 0) {{ {body} }} {jumps} }}'
                           f'  /* {a:#x}: hook (runtime/hle/hooks.c) */')
                last = 'op'
                continue
            if a in self.patches:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + self.patches[a](self, a))
                last = 'op'
                continue
            if (a + 4 < end and a + 4 not in labels and a + 4 not in self.patches
                    and a + 4 not in self.hooks and a + 4 - self.base not in self.rando_code):
                transaction = exclusive_pair_transaction(self.word(a), self.word(a + 4), a)
                if transaction:
                    flush()
                    out.append('    ' + transaction)
                    self.unsupported.difference_update((a, a + 4))
                    self.stats['exclusive_pair_transactions'] += 1
                    fused_tail, last = a + 4, 'op'
                    continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                if self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);')
            elif k == 'br':
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {hx(t)}: {jump(t)}' for t in jt)
                    out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {hx(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        self._refs = refs
        return out


def load_game_patches():
    from patches import HOOKS, PATCHES
    import json
    variants = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rando', 'patch_variants.json')
    rando = {}
    if os.path.exists(variants):
        with open(variants) as f:
            rando = {int(a, 16): [int(w, 16) for w in words] for a, words in json.load(f)['code'].items()}
    return PATCHES, HOOKS, rando


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--no-game-patches', action='store_true',
                    help='disable Link\'s Awakening patches, hooks and randomizer variants')
    ap.add_argument('--cache-limit', type=int, default=0,
                    help='maximum cached lifted instructions (0: unlimited)')
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    args = ap.parse_args()

    if args.cache_limit < 0:
        ap.error('--cache-limit must be nonnegative')
    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd, not args.no_game_patches, args.cache_limit)
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            if m.in_text(pc + imm):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{time.time() - t0:.0f}s', file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    order = sorted(entries)
    if args.limit:
        order = order[:args.limit]
    nchunks = 0
    ci = 0
    pending = None  # (address, lines, refs) of a big function that starts the next file
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < args.chunk and size < args.chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, labels, tables = entries[a]
                lines = m.emit_function(a, end, labels, tables, hle)
                r = getattr(m, '_refs', set())
                m._refs = set()
            n = sum(len(l) + 1 for l in lines)
            if n > args.big_fn and fns:  # (a big function gets a file of its own: they compile in parallel)
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            refs |= r
            if n > args.big_fn:
                break
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')]
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(args.outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(args.name) + r'_(\d{3,})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total; '
          f'exclusive pair transactions {m.stats["exclusive_pair_transactions"]}, '
          f'unsupported remaining {len(m.unsupported)}', file=sys.stderr)


if __name__ == '__main__':
    main()

enum { Z_PROGRAM_PREPARE_DRAWS = 160 };
// Keep preparation within the draw prefix the following PSO prediction pass can visit.
static void prog_prepare_rest(fz_cursor cursor, const prog_t *program) {
    shmap_load();
    const prog_t *seen[64]; unsigned count = 0, draws = 0;
    NSMutableOrderedSet<NSString *> *needed = [NSMutableOrderedSet orderedSet];
    for (unsigned guard = 0; guard < 200000 && draws < Z_PROGRAM_PREPARE_DRAWS && count < 64; guard++) {
        const uint8_t *position; op_hdr header; const void *arguments;
        if (!fz_next(&cursor, &position, &header, &arguments)) break;
        if (header.op == OP_BIND_PROGRAM) program = ((const p_bind_program *)arguments)->prog;
        if (header.op != OP_DRAW && header.op != OP_DRAW_IDX) continue;
        draws++;
        if (!program) continue;
        unsigned i = 0; while (i < count && seen[i] != program) i++;
        if (i != count) continue;
        seen[count++] = program;
        if (program->serial && serial_get(program->serial)) continue;
        prog_collect_missing(program, needed);
    }
    prog_prepare_missing(needed);
}

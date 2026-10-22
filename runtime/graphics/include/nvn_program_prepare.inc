static NSString *prog_stage_key(const NVNshaderData *sd) {
    uint8_t hash[32]; const void *control = sd->control, *code = (const void *)(uintptr_t)sd->data;
    if (!zelda_shader_hash(control, code, hash)) {
        if (control && code) MLOG("SHADER_LAYOUT magic=%x major=%u version=%u length=%u program=%u constants=%u c1=%u total=%u offset=%u stage=%u code_magic=%x",
            shader_word(control, 0), shader_word(control, 4), shader_word(control, 8), shader_word(control, 0x20),
            shader_word(control, 0x6f8), shader_word(control, 0x6fc), shader_word(control, 0x700), shader_word(control, 0x704),
            shader_word(control, 0x708), shader_word(control, 0x714), shader_word(code, 0));
        MDIE("Zelda shader data layout is unsupported");
    }
    char key[17]; for (unsigned i = 0; i < 8; i++) snprintf(key + i * 2, 3, "%02x", hash[i]);
    return @(key);
}

static NSArray<NSString *> *prog_collect_missing(const prog_t *program, NSMutableOrderedSet<NSString *> *needed) {
    if (program->nstage > 6) MDIE("Invalid shader stage count");
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    for (unsigned i = 0; i < program->nstage; i++) {
        const NVNshaderData *sd = &program->sd[i]; NSString *key = prog_stage_key(sd);
        [keys addObject:key];
        NSString *library = [g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@"_b.metallib"]];
        if ((g_shmap[key] && [NSFileManager.defaultManager fileExistsAtPath:library]) || [needed containsObject:key]) continue;
        NSMutableData *record = [NSMutableData dataWithBytes:sd->control length:shader_word(sd->control, 0x20)];
        [record appendBytes:(const void *)(uintptr_t)sd->data length:shader_word(sd->control, 0x704)];
        NSString *path = [NSString stringWithFormat:@"work/shaders/source/runtime-%@.nvn", key];
        if (![record writeToFile:path options:NSDataWritingAtomic error:NULL]) MDIE("Cannot capture runtime shader");
        [needed addObject:key];
    }
    return keys;
}

static void prog_prepare_missing(NSMutableOrderedSet<NSString *> *needed) {
    if (!needed.count) return;
    uint64_t start = bp_now();
    zelda_compile_shaders(needed.array);
    g_shmap = nil; shmap_load();
    MLOG("SHADER_BATCH keys=%lu preparation_ms=%.1f", (unsigned long)needed.count, (bp_now() - start) / 1e6);
}

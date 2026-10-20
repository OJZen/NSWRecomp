#include <math.h>
// A sampler clamped to another mip cannot read the attachment being cleared or drawn. Keep the whole-texture
// dependency for texel reads, images, unknown source, different serials and any overlapping filtered mip range.
static NSDictionary<NSNumber *, NSNumber *> *zelda_sample_mip_bindings(NSString *source) {
    NSMutableDictionary *bindings = [NSMutableDictionary dictionary];
    if (!source) return bindings;
    NSRegularExpression *texture = [NSRegularExpression regularExpressionWithPattern:
        @"^    texture2d<float> (\\w+) \\[\\[id\\((\\d+)\\)\\]\\];$" options:NSRegularExpressionAnchorsMatchLines error:nil];
    for (NSTextCheckingResult *t in [texture matchesInString:source options:0 range:NSMakeRange(0, source.length)]) {
        NSString *name = [source substringWithRange:[t rangeAtIndex:1]];
        NSRegularExpression *use = [NSRegularExpression regularExpressionWithPattern:
            [NSString stringWithFormat:@"\\b%@\\b", name] options:0 error:nil];
        NSRegularExpression *call = [NSRegularExpression regularExpressionWithPattern:zelda_fetch_call_pattern(name) options:0 error:nil];
        NSUInteger calls = [call numberOfMatchesInString:source options:0 range:NSMakeRange(0, source.length)];
        if (!calls || [use numberOfMatchesInString:source options:0 range:NSMakeRange(0, source.length)] != calls + 1) continue;
        NSRegularExpression *sampler = [NSRegularExpression regularExpressionWithPattern:
            [NSString stringWithFormat:@"^    sampler %@Smplr \\[\\[id\\((\\d+)\\)\\]\\];$", name]
            options:NSRegularExpressionAnchorsMatchLines error:nil];
        NSArray<NSTextCheckingResult *> *matches = [sampler matchesInString:source options:0 range:NSMakeRange(0, source.length)];
        if (matches.count == 1) bindings[@([[source substringWithRange:[t rangeAtIndex:2]] intValue])] =
            @([[source substringWithRange:[matches[0] rangeAtIndex:1]] intValue]);
    }
    return bindings;
}
static int zelda_mip_disjoint(int levels, int base, int count, int target, float lo, float hi) {
    if (levels < 1 || base < 0 || base >= levels || count < 1 || count > levels - base || target < 0 || target >= levels ||
        !isfinite(lo) || !isfinite(hi) || hi < 0 || hi < fmaxf(lo, 0)) return 0;
    // Include both neighbours for fractional trilinear LODs; clamp before converting to an integer.
    int first = base + (int)floorf(fminf(fmaxf(lo, 0), count - 1));
    int last = base + (int)ceilf(fminf(hi, count - 1));
    return target < first || target > last;
}

#ifndef ZELDA_SAMPLE_RANGE_ANALYSIS_ONLY
// Called with g_rmu held, like the existing storage-overlap check.
static int zelda_sample_mip_disjoint(NvShader *shader, const nvn_rbind *binding, int tid, uint64_t handle,
        const tex_t *target, const view_t *view, int has_view) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("ZELDA_SAMPLE_MIP_RANGE"); enabled = !e || strcmp(e, "0"); }
    if (!enabled || binding->kind != NVN_RB_TEX) return 0;
    const treg_t *sample = &g_treg[tid];
    if (!target->serial || sample->t.serial != target->serial || target->d.target != NVN_TEXTURE_TARGET_2D ||
        sample->t.d.target != NVN_TEXTURE_TARGET_2D || target->d.samples > 1 || sample->t.d.samples > 1 ||
        target->d.levels != sample->t.d.levels ||
        (sample->has_v && sample->v.set_target && sample->v.target != NVN_TEXTURE_TARGET_2D) ||
        (has_view && view->set_target && view->target != NVN_TEXTURE_TARGET_2D)) return 0;
    int sid = (int)(handle >> 20);
    if (sid <= 0 || sid >= g_nsreg || !g_sreg_ok[sid] || g_sreg[sid].minf < 2 || g_sreg[sid].minf > 5) return 0;
    static NSMutableDictionary<NSString *, NSDictionary *> *cache;
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSString *key = @(shader->key); NSDictionary *bindings = cache[key];
    if (!bindings) cache[key] = bindings = zelda_sample_mip_bindings(zelda_fetch_shader_text(shader));
    NSNumber *sampler = bindings[@(binding->aid)];
    if (!sampler) return 0;
    int linked = 0;
    for (int i = 0; i < shader->rf.nb; i++) {
        const nvn_rbind *b = &shader->rf.b[i];
        linked |= b->kind == NVN_RB_SAMPT && b->aid == sampler.intValue && b->reg == binding->reg;
    }
    if (!linked) return 0;
    int base = sample->has_v && sample->v.set_levels ? sample->v.base_level : 0;
    int count = sample->has_v && sample->v.set_levels && sample->v.nlevels ? sample->v.nlevels : sample->t.d.levels - base;
    int level = has_view && view->set_levels ? view->base_level : 0;
    return zelda_mip_disjoint(target->d.levels, base, count, level, g_sreg[sid].lod_min, g_sreg[sid].lod_max);
}
#endif

static NvProg *zelda_line_program(NvProg *program) {
    if (program->z_line) return program->z_line;
    NvShader *shader = zelda_line_shader(program->sh[0], program->sh[1]);
    if (!shader) return nil;
    NvProg *result = [NvProg new]; for (int i = 0; i < 6; i++) result->sh[i] = program->sh[i]; result->sh[0] = shader;
    program->z_line = result; return result;
}

static NvProg *zelda_line_prepare(NvProg *program, const p_draw *dw, const p_draw_idx *di, zelda_line_params *p) {
    int primitive = dw ? dw->prim : di->prim;
    if (S.line_width == 1 || !zelda_uses_line_width(primitive, S.poly.mode)) return program;
    NvProg *expanded = zelda_line_program(program);
    if (!expanded || (primitive != NVN_DRAW_PRIMITIVE_LINES && primitive != NVN_DRAW_PRIMITIVE_LINE_STRIP) ||
        (di && S.restart && primitive != NVN_DRAW_PRIMITIVE_LINE_STRIP))
        MDIE("Unsupported wide-line draw width=%g primitive=%d polygon=%d vs=%s fs=%s", S.line_width, primitive, S.poly.mode,
            program->sh[0] ? program->sh[0]->key : "-", program->sh[1] ? program->sh[1]->key : "-");
    if (program->sh[0]->rf.nv != 1 || program->sh[0]->rf.v[0].nvn != 0)
        MDIE("Wide-line vertex interface changed");
    va_st attribute = S.attr[0]; unsigned stream = attribute.stream;
    MTLVertexFormat format = vfmt(attribute.fmt);
    unsigned vertex_size = format == MTLVertexFormatUCharNormalized || format == MTLVertexFormatUChar2Normalized ||
        format == MTLVertexFormatUChar3Normalized || format == MTLVertexFormatUChar4Normalized ? 1 : 4;
    if (stream >= 16 || S.stream[stream].divisor ||
        (vertex_size != 1 && format != MTLVertexFormatFloat && format != MTLVertexFormatFloat2 && format != MTLVertexFormatFloat3 && format != MTLVertexFormatFloat4) ||
        ((S.stream[stream].stride | attribute.offset | S.vbuf[stream].address) & (vertex_size - 1)))
        MDIE("Unsupported wide-line vertex layout format=%u stream=%u", attribute.fmt, stream);
    *p = (zelda_line_params){.vertex_bytes = S.vbuf[stream].size, .stride = S.stream[stream].stride, .offset = attribute.offset,
        .first = dw ? dw->first : 0, .count = dw ? dw->count : di->count, .base_vertex = di ? di->base_vertex : 0,
        .primitive = primitive, .restart = di && S.restart, .restart_index = S.restart_index, .vertex_size = vertex_size};
    double width = fmax(1, floor((double)S.line_width + 0.5));
    if (!isfinite(width) || width > UINT32_MAX / 2 || (dw && dw->first < 0)) MDIE("Invalid wide-line dimensions");
    p->width = (uint32_t)width;
    void *buffer; uint64_t offset;
    if (p->vertex_bytes < vertex_size || !mtl_gpu_addr(S.vbuf[stream].address, p->vertex_bytes, &buffer, &offset, &p->vertices))
        MDIE("Wide-line vertex buffer is outside its memory pool");
    if (di) {
        if (di->itype < 0 || di->itype > 2) MDIE("Unsupported wide-line index type %d", di->itype);
        p->index_size = 1u << di->itype;
        if ((di->ibuf & (p->index_size - 1)) || !mtl_gpu_addr(di->ibuf, (uint64_t)p->count * p->index_size, &buffer, &offset, &p->indices))
            MDIE("Wide-line index buffer is outside its memory pool");
        use_rec("buf", di->ibuf, (uint64_t)p->count * p->index_size, 0, NULL);
    }
    static int once;
    if (!once++) MLOG("Wide-line GPU expansion width=%g pixels=%u primitive=%d vertices=%u index=%u format=%u stride=%u", S.line_width, p->width, primitive, p->count, p->index_size, attribute.fmt, p->stride);
    return expanded;
}

static void zelda_line_draw(id encoder, zelda_line_params *p, unsigned instances, float width, float height) {
    if (p->count < 2) return;
    if (!(width > 0 && height > 0)) MDIE("Invalid wide-line viewport");
    p->viewport[0] = width; p->viewport[1] = height;
    uint64_t segments = p->primitive == NVN_DRAW_PRIMITIVE_LINES ? p->count / 2 : p->count - 1;
    if (segments * instances > UINT32_MAX) MDIE("Wide-line instance count overflow");
    id<MTLBuffer> buffer; size_t offset = arena_alloc(sizeof *p, &buffer);
    memcpy((uint8_t *)buffer.contents + offset, p, sizeof *p);
    enc_r_vbuf(encoder, 14, buffer.gpuAddress + offset);
    enc_r_draw_instanced(encoder, MTLPrimitiveTypeLine, 0, 2 * p->width, segments * instances, 0);
}

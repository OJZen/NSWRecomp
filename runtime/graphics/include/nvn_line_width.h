// Line width does not affect filled triangles or points, even when recorded alongside their other raster state.
static inline int zelda_uses_line_width(int primitive, int polygon_mode) {
    return primitive == NVN_DRAW_PRIMITIVE_LINES || primitive == NVN_DRAW_PRIMITIVE_LINE_LOOP ||
        primitive == NVN_DRAW_PRIMITIVE_LINE_STRIP ||
        (primitive >= NVN_DRAW_PRIMITIVE_TRIANGLES && polygon_mode == NVN_POLYGON_MODE_LINE);
}

/*
 * Font loading and text rendering on top of FreeType. Each cg_font owns an
 * FT_Face plus a hash table of rendered glyph bitmaps keyed by
 * (glyph index, pixel size). Pen positions are kept in 26.6 fixed point and
 * snapped to whole pixels per glyph.
 */
#include "cg_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdlib.h>
#include <string.h>

typedef struct glyph {
    uint32_t gi;
    int32_t size26;     /* 0 = empty slot */
    int16_t w, h, left, top;
    int32_t adv26;
    bool rendered;     /* measurements need only the advance, not a bitmap */
    uint8_t *bmp;
} glyph;

struct cg_font {
    FT_Face face;
    char *path;
    int index;
    bool symbol;        /* MS Symbol charmap: codepoints live at U+F0xx */
    int32_t cur_size26;
    glyph *tab;
    int cap, count;
    size_t bytes;
    uint32_t ascii[128];
    bool ascii_ok[128];
};

static FT_Library g_ft;
static int g_ft_users;

static bool ft_init(void)
{
    if (g_ft) return true;
    return FT_Init_FreeType(&g_ft) == 0;
}

/* ---- UTF-8 ---------------------------------------------------------- */

int utf8_decode(const char *str, int n, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    if (n <= 0) { *cp = 0; return 1; }
    unsigned c = s[0];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (len == 0 || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_prev(const char *s, int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

int utf8_next(const char *s, int len, int pos)
{
    if (pos >= len) return len;
    uint32_t cp;
    return pos + utf8_decode(s + pos, len - pos, &cp);
}

/* ---- loading ---------------------------------------------------------- */

cg_font *cg_font_load(const char *path, int face_index)
{
    if (!path || !ft_init()) return NULL;
    FT_Face face;
    if (FT_New_Face(g_ft, path, face_index, &face) != 0) return NULL;
    cg_font *f = (cg_font *)calloc(1, sizeof *f);
    f->face = face;
    f->path = strdup(path);
    f->index = face_index;
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        if (FT_Select_Charmap(face, FT_ENCODING_MS_SYMBOL) == 0) f->symbol = true;
    }
    f->cap = 256;
    f->tab = (glyph *)calloc((size_t)f->cap, sizeof(glyph));
    g_ft_users++;
    return f;
}

static void cache_clear(cg_font *f)
{
    for (int i = 0; i < f->cap; i++) free(f->tab[i].bmp);
    memset(f->tab, 0, sizeof(glyph) * (size_t)f->cap);
    f->count = 0;
    f->bytes = 0;
}

void cg_font_free(cg_font *f)
{
    if (!f) return;
    cache_clear(f);
    free(f->tab);
    FT_Done_Face(f->face);
    free(f->path);
    free(f);
    if (--g_ft_users == 0 && g_ft) {
        FT_Done_FreeType(g_ft);
        g_ft = NULL;
    }
}

const char *cg_font_path(const cg_font *f) { return f ? f->path : NULL; }

int cg_font_codepoints(cg_font *f, uint32_t *out, int max)
{
    if (!f) return 0;
    int n = 0;
    FT_UInt gi;
    FT_ULong c = FT_Get_First_Char(f->face, &gi);
    while (gi != 0) {
        uint32_t cp = (uint32_t)c;
        /* Symbol fonts map their glyphs at U+F0xx; char_index() folds
         * U+00xx onto those, so report the plain codepoint. */
        if (f->symbol && cp >= 0xF000 && cp < 0xF100) cp -= 0xF000;
        if (out && n < max) out[n] = cp;
        n++;
        c = FT_Get_Next_Char(f->face, c, &gi);
    }
    return n;
}

static void set_size(cg_font *f, int32_t size26)
{
    if (f->cur_size26 != size26) {
        FT_Set_Char_Size(f->face, 0, size26, 72, 72);
        f->cur_size26 = size26;
    }
}

static uint32_t char_index(cg_font *f, uint32_t cp)
{
    if (cp < 128 && f->ascii_ok[cp]) return f->ascii[cp];
    uint32_t gi = FT_Get_Char_Index(f->face, cp);
    if (!gi && f->symbol && cp < 0x100) gi = FT_Get_Char_Index(f->face, 0xF000 + cp);
    if (cp < 128) {
        f->ascii[cp] = gi;
        f->ascii_ok[cp] = true;
    }
    return gi;
}

static inline uint32_t hash2(uint32_t gi, int32_t size26)
{
    return (gi * 2654435761u) ^ ((uint32_t)size26 * 40503u);
}

static void cache_grow(cg_font *f)
{
    glyph *old = f->tab;
    int oldcap = f->cap;
    f->cap *= 2;
    f->tab = (glyph *)calloc((size_t)f->cap, sizeof(glyph));
    for (int i = 0; i < oldcap; i++) {
        if (!old[i].size26) continue;
        uint32_t h = hash2(old[i].gi, old[i].size26) & (uint32_t)(f->cap - 1);
        while (f->tab[h].size26) h = (h + 1) & (uint32_t)(f->cap - 1);
        f->tab[h] = old[i];
    }
    free(old);
}

static void render_glyph(cg_font *f, glyph *g, bool loaded)
{
    set_size(f, g->size26);
    if ((!loaded && FT_Load_Glyph(f->face, g->gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) != 0) ||
        FT_Render_Glyph(f->face->glyph, FT_RENDER_MODE_LIGHT) != 0) return;
    FT_GlyphSlot slot = f->face->glyph;
    FT_Bitmap *bm = &slot->bitmap;
    g->left = (int16_t)slot->bitmap_left;
    g->top = (int16_t)slot->bitmap_top;
    if (bm->width > 0 && bm->rows > 0 && bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
        if (bm->width > INT16_MAX || bm->rows > INT16_MAX) return;
        size_t bytes = (size_t)bm->width * bm->rows;
        g->bmp = (uint8_t *)malloc(bytes);
        if (!g->bmp) return;
        g->w = (int16_t)bm->width;
        g->h = (int16_t)bm->rows;
        for (int y = 0; y < g->h; y++)
            memcpy(g->bmp + (size_t)y * g->w, bm->buffer + (ptrdiff_t)y * bm->pitch, (size_t)g->w);
        f->bytes += bytes;
    }
    g->rendered = true;
}

static const glyph *get_glyph(cg_font *f, uint32_t gi, int32_t size26, bool draw)
{
    /* Bound metadata as well as bitmap storage, including measure-only runs. */
    if (f->bytes > (16u << 20) || f->count >= 65536) cache_clear(f);
    uint32_t mask = (uint32_t)f->cap - 1;
    uint32_t h = hash2(gi, size26) & mask;
    while (f->tab[h].size26) {
        if (f->tab[h].gi == gi && f->tab[h].size26 == size26) {
            if (draw && !f->tab[h].rendered) render_glyph(f, &f->tab[h], false);
            return &f->tab[h];
        }
        h = (h + 1) & mask;
    }
    /* Cache metrics now; rasterize only when a caller draws the glyph. */
    if ((f->count + 1) * 10 > f->cap * 7) {
        cache_grow(f);
        mask = (uint32_t)f->cap - 1;
        h = hash2(gi, size26) & mask;
        while (f->tab[h].size26) h = (h + 1) & mask;
    }
    glyph g;
    memset(&g, 0, sizeof g);
    g.gi = gi;
    g.size26 = size26;
    set_size(f, size26);
    FT_Face face = f->face;
    if (FT_Load_Glyph(face, gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) == 0) {
        g.adv26 = (int32_t)face->glyph->advance.x;
        if (draw) render_glyph(f, &g, true);
    }
    f->tab[h] = g;
    f->count++;
    return &f->tab[h];
}

static int32_t to26(float px) { return (int32_t)(px * 64.f + 0.5f); }

/* ---- runs, measuring, drawing ----------------------------------------- */

void font_run_begin(font_run *run, cg_font *font, float px_size)
{
    run->font = font;
    run->size26 = to26(px_size < 1.f ? 1.f : px_size);
    run->prev_gi = 0;
    run->pen26 = 0;
}

static int32_t run_glyph(font_run *run, uint32_t cp, const glyph **out)
{
    cg_font *f = run->font;
    if (cp == '\t') cp = ' ';
    uint32_t gi = char_index(f, cp);
    if (run->prev_gi && gi && FT_HAS_KERNING(f->face)) {
        FT_Vector k;
        set_size(f, run->size26);
        if (FT_Get_Kerning(f->face, run->prev_gi, gi, FT_KERNING_DEFAULT, &k) == 0)
            run->pen26 += (int32_t)k.x;
    }
    const glyph *g = get_glyph(f, gi, run->size26, out != NULL);
    int32_t origin = run->pen26;
    run->pen26 += g->adv26;
    run->prev_gi = gi;
    if (out) *out = g;
    return origin;
}

int32_t font_run_step(font_run *run, uint32_t cp)
{
    if (!run->font) return run->pen26;
    run_glyph(run, cp, NULL);
    return run->pen26;
}

float font_measure(cg_font *font, float px_size, const char *s, int n)
{
    if (!font || !s) return 0;
    if (n < 0) n = (int)strlen(s);
    font_run run;
    font_run_begin(&run, font, px_size);
    for (int i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        font_run_step(&run, cp);
    }
    return run.pen26 / 64.f;
}

float font_draw(cg_canvas *cv, cg_font *font, float px_size, float x, float baseline,
                const char *s, int n, pm_color c)
{
    if (!font || !s) return x;
    if (n < 0) n = (int)strlen(s);
    font_run run;
    font_run_begin(&run, font, px_size);
    int by = (int)floorf(baseline + 0.5f);
    for (int i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        if (cp == '\n' || cp == '\r') continue;
        const glyph *g;
        int32_t origin = run_glyph(&run, cp, &g);
        if (g->bmp) {
            int gx = (int)floorf(x + origin / 64.f + 0.5f) + g->left;
            int gy = by - g->top;
            if (gx < cv->cx1 && gx + g->w > cv->cx0)
                canvas_blit_a8(cv, gx, gy, g->bmp, g->w, g->h, g->w, c);
        }
        if (x + run.pen26 / 64.f > cv->cx1 + px_size * 2) break; /* rest is clipped */
    }
    return x + run.pen26 / 64.f;
}

void font_metrics(cg_font *font, float px_size, float *ascent, float *descent, float *line_h)
{
    float a = px_size * 0.8f, d = px_size * 0.2f, lh = px_size * 1.2f;
    if (font) {
        set_size(font, to26(px_size < 1.f ? 1.f : px_size));
        FT_Size_Metrics *m = &font->face->size->metrics;
        float fa = m->ascender / 64.f, fd = -m->descender / 64.f, fh = m->height / 64.f;
        if (fa > 0 && fa < px_size * 4) {
            a = ceilf(fa);
            d = ceilf(fd > 0 ? fd : 0);
            lh = fmaxf(a + d, ceilf(fh));
            if (lh > (a + d) * 1.5f) lh = a + d;  /* ignore absurd line gaps */
        }
    }
    if (ascent) *ascent = a;
    if (descent) *descent = d;
    if (line_h) *line_h = lh;
}

/* ---- public, logical-unit wrappers ----------------------------------- */

float cg_draw_text(cg_window *w, cg_font *font, float size, float x, float y,
                   const char *s, int n, cg_color c)
{
    if (!font) return x;
    float px = size * w->scale, asc;
    font_metrics(font, px, &asc, NULL, NULL);
    float end = font_draw(&w->cv, font, px, x * w->scale, y * w->scale + asc, s, n, pm_from(c, 1.f));
    return end / w->scale;
}

float cg_text_width(cg_window *w, cg_font *font, float size, const char *s, int n)
{
    return font_measure(font, size * w->scale, s, n) / w->scale;
}

void cg_font_metrics(cg_window *w, cg_font *font, float size, float *ascent, float *descent,
                     float *line_height)
{
    float a, d, lh;
    font_metrics(font, size * w->scale, &a, &d, &lh);
    if (ascent) *ascent = a / w->scale;
    if (descent) *descent = d / w->scale;
    if (line_height) *line_height = lh / w->scale;
}

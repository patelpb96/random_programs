/* Include the implementation to inspect cache storage without a public debug API. */
#include "../src/font.c"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
    int index = 0;
    const char *path = fontdb_default_ui_font_path(&index);
    cg_font *f = cg_font_load(path, index);
    CHECK(f != NULL);
    const char *sample = "AVATAR To Wa 0123456789";
    float width = font_measure(f, 24, sample, -1);
    CHECK(width > 0 && f->count > 0 && f->bytes == 0);

    /* Compare advances and kerning against the original eager-render path. */
    FT_UInt prev = 0;
    FT_Pos expected = 0;
    for (const char *s = sample; *s; s++) {
        FT_UInt gi = FT_Get_Char_Index(f->face, (unsigned char)*s);
        FT_Vector kern = {0, 0};
        if (prev && gi && FT_HAS_KERNING(f->face))
            CHECK(FT_Get_Kerning(f->face, prev, gi, FT_KERNING_DEFAULT, &kern) == 0);
        CHECK(FT_Load_Glyph(f->face, gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) == 0);
        CHECK(FT_Render_Glyph(f->face->glyph, FT_RENDER_MODE_LIGHT) == 0);
        expected += kern.x + f->face->glyph->advance.x;
        prev = gi;
    }
    CHECK(width == expected / 64.f);
    cg_canvas cv = {0};
    canvas_resize(&cv, 800, 80);
    canvas_clear(&cv, 0);
    pm_color white = {255, 255, 255, 255};
    CHECK(font_draw(&cv, f, 24, 0, 40, sample, -1, white) == width);
    CHECK(f->bytes > 0);
    size_t bytes = f->bytes;
    CHECK(font_measure(f, 24, sample, -1) == width);
    CHECK(font_draw(&cv, f, 24, 0, 40, sample, -1, white) == width);
    CHECK(f->bytes == bytes);
    printf("Measured without bitmaps; drawing allocated %zu bytes; widths match FreeType.\n", bytes);

    cache_clear(f);
    /* Invalid indices exercise metadata growth cheaply, without bitmap storage. */
    for (int i = 1; i <= 70000; i++)
        get_glyph(f, (uint32_t)f->face->num_glyphs + 1, i, false);
    CHECK(f->count <= 65536 && f->bytes == 0);
    CHECK(font_measure(f, 24, sample, -1) == width);
    CHECK(font_draw(&cv, f, 24, 0, 40, sample, -1, white) == width);
    printf("Cache remains usable after metadata limit and rehashing.\n");
    canvas_free(&cv);
    cg_font_free(f);
    return 0;
}

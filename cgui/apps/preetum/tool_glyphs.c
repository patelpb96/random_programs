/* Glyph Map: every character a font contains, in a scrollable grid, with an
 * inspector showing the selected glyph large (with its metrics) and its
 * codes, plus buttons to copy it. */
#include "preetum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int encode_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = 0;
    return 4;
}

/* Skip whitespace and invisible control/format characters. */
static bool visible(uint32_t cp)
{
    return cp > 0x20 && !(cp >= 0x7F && cp <= 0xA0) && cp != 0xAD && !(cp >= 0xD800 && cp <= 0xDFFF) &&
           !(cp >= 0x200B && cp <= 0x200F) && !(cp >= 0x2028 && cp <= 0x202F) && cp != 0xFEFF;
}

static void refresh_codepoints(preetum *a)
{
    if (a->cps_family == a->loaded_family && a->cps_style == a->loaded_style) return;
    free(a->cps);
    a->cps = NULL;
    a->ncps = 0;
    a->cps_family = a->loaded_family;
    a->cps_style = a->loaded_style;
    a->glyph_scroll = 0;
    a->glyph_sel = 0;
    if (!a->font) return;
    int n = cg_font_codepoints(a->font, NULL, 0);
    uint32_t *all = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
    cg_font_codepoints(a->font, all, n);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (visible(all[i]) && (m == 0 || all[i] != all[m - 1])) all[m++] = all[i];
    a->cps = all;
    a->ncps = m;
    /* Start on 'A' when the font has it. */
    for (int i = 0; i < m; i++)
        if (all[i] == 'A') a->glyph_sel = i;
}

void glyphs_init(preetum *a)
{
    a->cell = 56;
    a->cps_family = a->cps_style = -2;
}

void glyphs_free(preetum *a) { free(a->cps); }

void glyphs_icon(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    float s = fminf(r.w, r.h) / 3.f, x0 = r.x + (r.w - 3 * s) * 0.5f, y0 = r.y + (r.h - 3 * s) * 0.5f;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            cg_rect c = cg_inset(cg_rect_make(x0 + i * s, y0 + j * s, s, s), 2);
            if (i == 1 && j == 1) cg_fill_rrect(win, c, 3, th->accent);
            else cg_stroke_rrect(win, c, 3, 1.2f, th->text_dim);
        }
    cg_font *f = a->font ? a->font : cg_ui_font(win);
    float size = s * 0.7f, lh, w = cg_text_width(win, f, size, "&", 1);
    cg_font_metrics(win, f, size, NULL, NULL, &lh);
    cg_draw_text(win, f, size, x0 + s + (s - w) * 0.5f, y0 + s + (s - lh) * 0.5f, "&", 1, th->accent_text);
}

static void centered_glyph(cg_window *win, cg_font *f, float size, cg_rect r, const char *s, cg_color c)
{
    float lh, w = cg_text_width(win, f, size, s, -1);
    cg_font_metrics(win, f, size, NULL, NULL, &lh);
    cg_draw_text(win, f, size, floorf(r.x + (r.w - w) * 0.5f), floorf(r.y + (r.h - lh) * 0.5f), s, -1, c);
}

static void inspector(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    cg_font *ui = cg_ui_font(win);
    cg_fill_rrect(win, r, 8, th->panel_bg);
    cg_stroke_rrect(win, r, 8, 1, th->panel_border);
    r = cg_inset(r, 12);
    if (a->ncps == 0) {
        cg_label(win, cg_cut_top(&r, 24), "No visible glyphs", CG_ALIGN_LEFT, th->text_dim);
        return;
    }
    uint32_t cp = a->cps[a->glyph_sel];
    char ch[8];
    int nbytes = encode_utf8(cp, ch);

    /* Big glyph with ascender / baseline / descender guides and the advance. */
    cg_rect box = cg_cut_top(&r, fminf(r.w, r.h * 0.55f));
    cg_fill_rrect(win, box, 6, cg_color_mix(th->panel_bg, th->body_bg, 0.5f));
    float size = box.h * 0.5f, asc, desc, lh;
    cg_font_metrics(win, a->font, size, &asc, &desc, &lh);
    float adv = cg_text_width(win, a->font, size, ch, -1);
    float top = floorf(box.y + (box.h - lh) * 0.5f), base = top + asc;
    float gx = floorf(box.x + (box.w - adv) * 0.5f);
    cg_color guide = cg_color_alpha(th->text_dim, 0.45f);
    cg_fill_rect(win, cg_rect_make(box.x + 6, top, box.w - 12, 1), guide);
    cg_fill_rect(win, cg_rect_make(box.x + 6, base, box.w - 12, 1), cg_color_alpha(th->accent, 0.7f));
    cg_fill_rect(win, cg_rect_make(box.x + 6, base + desc, box.w - 12, 1), guide);
    cg_fill_rect(win, cg_rect_make(gx, top - 4, 1, lh + 8), guide);
    cg_fill_rect(win, cg_rect_make(gx + adv, top - 4, 1, lh + 8), guide);
    cg_push_clip(win, box);
    cg_draw_text(win, a->font, size, gx, top, ch, -1, th->text);
    cg_pop_clip(win);
    cg_cut_top(&r, 12);

    /* Codes. */
    char buf[64];
    snprintf(buf, sizeof buf, "U+%04X", (unsigned)cp);
    float lh_big;
    cg_font_metrics(win, ui, 22, NULL, NULL, &lh_big);
    cg_rect line = cg_cut_top(&r, lh_big + 4);
    cg_draw_text(win, ui, 22, line.x, line.y, buf, -1, th->text);

    char utf8_hex[32] = "";
    for (int i = 0; i < nbytes; i++) {
        char b[4];
        snprintf(b, sizeof b, i ? " %02X" : "%02X", (unsigned char)ch[i]);
        strcat(utf8_hex, b);
    }
    char dec[24], html[24];
    snprintf(dec, sizeof dec, "%u", (unsigned)cp);
    snprintf(html, sizeof html, "&#%u;", (unsigned)cp);
    const char *keys[3] = { "Decimal", "UTF-8", "HTML" };
    const char *vals[3] = { dec, utf8_hex, html };
    for (int i = 0; i < 3; i++) {
        cg_rect row = cg_cut_top(&r, 22);
        cg_label(win, cg_cut_left(&row, 70), keys[i], CG_ALIGN_LEFT, th->text_dim);
        cg_label(win, row, vals[i], CG_ALIGN_LEFT, th->text);
    }
    cg_cut_top(&r, 10);

    cg_rect btns = cg_cut_top(&r, 30);
    float bw = (btns.w - 8) * 0.5f;
    if (cg_button(win, cg_id_str("glyph.copy"), cg_cut_left(&btns, bw), "Copy glyph")) cg_clipboard_set(win, ch);
    cg_cut_left(&btns, 8);
    if (cg_button(win, cg_id_str("glyph.copycode"), btns, "Copy U+code")) cg_clipboard_set(win, buf);

    cg_rect tip = cg_cut_bottom(&r, 18);
    cg_label(win, tip, "Arrow keys move · wheel scrolls", CG_ALIGN_LEFT, th->text_dim);
}

void glyphs_frame(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    refresh_codepoints(a);

    cg_rect head = cg_cut_top(&r, 30);
    cg_cut_top(&r, 8);
    cg_spinbox(win, cg_id_str("glyph.cell"), cg_cut_right(&head, 110), &a->cell, 32, 128, 4, "%.0f px");
    cg_cut_right(&head, 6);
    cg_label(win, cg_cut_right(&head, 34), "Cell", CG_ALIGN_RIGHT, th->text_dim);
    cg_cut_right(&head, 14);
    char count[48];
    snprintf(count, sizeof count, "%d glyphs", a->ncps);
    cg_label(win, cg_cut_right(&head, 100), count, CG_ALIGN_RIGHT, th->text_dim);
    cg_cut_right(&head, 12);
    fonts_picker(win, head, a);
    refresh_codepoints(a); /* the picker may have switched fonts */
    if (!a->font) return;

    cg_rect insp = cg_cut_right(&r, 260);
    cg_cut_right(&r, 12);

    cg_fill_rrect(win, r, 8, th->panel_bg);
    cg_stroke_rrect(win, r, 8, 1, th->panel_border);
    cg_rect area = cg_inset(r, 8);
    cg_rect bar = cg_cut_right(&area, 8);
    cg_cut_right(&area, 4);

    int cols = (int)(area.w / a->cell);
    if (cols < 1) cols = 1;
    float cw = area.w / cols, ch = a->cell;
    int rows = (a->ncps + cols - 1) / cols;
    float content = rows * ch;

    /* Keyboard navigation, unless a text box or dropdown has the keys. */
    int sel = a->glyph_sel, page = cols * (int)fmaxf(1, area.h / ch - 1);
    if (cg_focused(win) == 0 && a->ncps > 0) {
        if (cg_key_pressed(win, CG_KEY_LEFT, 0)) sel--;
        if (cg_key_pressed(win, CG_KEY_RIGHT, 0)) sel++;
        if (cg_key_pressed(win, CG_KEY_UP, 0)) sel -= cols;
        if (cg_key_pressed(win, CG_KEY_DOWN, 0)) sel += cols;
        if (cg_key_pressed(win, CG_KEY_PAGE_UP, 0)) sel -= page;
        if (cg_key_pressed(win, CG_KEY_PAGE_DOWN, 0)) sel += page;
        if (cg_key_pressed(win, CG_KEY_HOME, 0)) sel = 0;
        if (cg_key_pressed(win, CG_KEY_END, 0)) sel = a->ncps - 1;
        if (sel < 0) sel = 0;
        if (sel >= a->ncps) sel = a->ncps - 1;
        if (sel != a->glyph_sel) {
            float y = (sel / cols) * ch;
            if (y < a->glyph_scroll) a->glyph_scroll = y;
            if (y + ch > a->glyph_scroll + area.h) a->glyph_scroll = y + ch - area.h;
            a->glyph_sel = sel;
        }
    }

    a->glyph_scroll -= cg_wheel(win, area) * ch * 1.5f;
    a->glyph_scroll = fmaxf(0, fminf(a->glyph_scroll, content - area.h));
    cg_scrollbar(win, cg_id_str("glyph.bar"), bar, &a->glyph_scroll, content, area.h);

    /* Mouse: hover and click-to-select. */
    cg_interaction it = cg_interact(win, cg_id_str("glyph.grid"), area);
    int hovered = -1;
    if (it.hover) {
        float mx, my;
        cg_mouse_pos(win, &mx, &my);
        int col = (int)((mx - area.x) / cw), row = (int)((my - area.y + a->glyph_scroll) / ch);
        int idx = row * cols + col;
        if (col >= 0 && col < cols && idx >= 0 && idx < a->ncps) hovered = idx;
    }
    if (hovered >= 0) {
        cg_set_cursor(win, CG_CURSOR_HAND);
        if (it.pressed) a->glyph_sel = hovered;
    }

    /* Draw only the visible rows. */
    cg_push_clip(win, area);
    float gsize = ch * 0.46f;
    bool labels = ch >= 44;
    int first = (int)(a->glyph_scroll / ch);
    for (int row = first; row < rows; row++) {
        float y = area.y + row * ch - a->glyph_scroll;
        if (y > area.y + area.h) break;
        for (int col = 0; col < cols; col++) {
            int idx = row * cols + col;
            if (idx >= a->ncps) break;
            cg_rect cell = cg_inset(cg_rect_make(area.x + col * cw, y, cw, ch), 2);
            if (idx == a->glyph_sel) {
                cg_fill_rrect(win, cell, 6, cg_color_alpha(th->accent, 0.22f));
                cg_stroke_rrect(win, cell, 6, 1.5f, th->accent);
            } else if (idx == hovered) {
                cg_fill_rrect(win, cell, 6, th->control_hover);
            }
            char s[8];
            encode_utf8(a->cps[idx], s);
            cg_rect gr = cell;
            if (labels) gr.h -= 12;
            centered_glyph(win, a->font, gsize, gr, s, th->text);
            if (labels) {
                char hex[12];
                snprintf(hex, sizeof hex, "%04X", (unsigned)a->cps[idx]);
                centered_glyph(win, cg_ui_font(win), 9.5f, cg_rect_make(cell.x, cell.y + cell.h - 16, cell.w, 14),
                               hex, th->text_dim);
            }
        }
    }
    cg_pop_clip(win);

    inspector(win, insp, a);
}

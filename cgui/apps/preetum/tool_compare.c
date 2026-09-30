/* Font Compare: type on the left, see it in any installed font on the right,
 * each side with its own size. */
#include "preetum.h"

static const char *sample_text =
    "The quick brown fox jumps over the lazy dog.\n"
    "Sphinx of black quartz, judge my vow!\n"
    "\n"
    "0123456789  ({[ ]})  @#$%&*  \"quotes\" 'apostrophes'\n"
    "Àçcéntëd lettérs: façade, naïve, Ångström, Straße\n"
    "\n"
    "Type on the left; pick a font on the right.";

void compare_init(preetum *a)
{
    a->in_size = 16;
    a->out_size = 32;
    cg_textbuf_init(&a->input, sample_text);
    a->input.placeholder = "Type something…";
    cg_textbuf_init(&a->output, sample_text);
    a->synced_version = a->input.version;
}

void compare_free(preetum *a)
{
    cg_textbuf_free(&a->input);
    cg_textbuf_free(&a->output);
}

void compare_icon(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    float size = r.h * 0.62f, lh;
    cg_font *ui = cg_ui_font(win);
    cg_font *f = a->font ? a->font : ui;
    cg_font_metrics(win, ui, size, NULL, NULL, &lh);
    float y = r.y + (r.h - lh) * 0.5f;
    float wa = cg_text_width(win, ui, size, "A", 1), wb = cg_text_width(win, f, size, "a", 1);
    float x = r.x + (r.w - wa - wb - 2) * 0.5f;
    x = cg_draw_text(win, ui, size, x, y, "A", 1, th->text) + 2;
    cg_draw_text(win, f, size, x, y, "a", 1, th->accent);
}

void compare_frame(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    float gap = 14;
    float half = (r.w - gap) * 0.5f;
    cg_rect left = cg_cut_left(&r, half);
    cg_cut_left(&r, gap);
    cg_rect right = r;

    /* Left: the editor, in the UI font. */
    cg_rect lhead = cg_cut_top(&left, 30);
    cg_cut_top(&left, 8);
    cg_label(win, cg_cut_left(&lhead, 120), "Input", CG_ALIGN_LEFT, th->text);
    cg_spinbox(win, cg_id_str("in.size"), cg_cut_right(&lhead, 110), &a->in_size, 8, 72, 1, "%.0f px");
    cg_cut_right(&lhead, 6);
    cg_label(win, cg_cut_right(&lhead, 34), "Size", CG_ALIGN_RIGHT, th->text_dim);
    cg_textedit(win, cg_id_str("input"), left, &a->input, NULL, a->in_size, 0);

    if (a->input.version != a->synced_version) {
        cg_textbuf_set(&a->output, a->input.data);
        a->synced_version = a->input.version;
    }

    /* Right: font picker + preview. */
    cg_rect rhead = cg_cut_top(&right, 30);
    cg_cut_top(&right, 8);
    cg_spinbox(win, cg_id_str("out.size"), cg_cut_right(&rhead, 110), &a->out_size, 6, 200, 1, "%.0f px");
    cg_cut_right(&rhead, 8);
    fonts_picker(win, rhead, a);

    cg_rect status = cg_cut_bottom(&right, 22);
    cg_cut_bottom(&right, 4);
    cg_push_clip(win, status);
    cg_draw_text(win, cg_ui_font(win), 12, status.x + 2, status.y + 5,
                 a->font ? a->font_desc : "No fonts found on this system", -1, th->text_dim);
    cg_pop_clip(win);
    cg_textedit(win, cg_id_str("output"), right, &a->output, a->font, a->out_size, CG_TEXT_READONLY);
}

/*
 * fontview - cgui test program.
 *
 * Left half: a text editor. Right half: the same text rendered in any font
 * installed on the system, picked from a searchable dropdown. Both sides
 * have their own size control. The bottom bar edits the window's
 * appearance live (theme, frame/body transparency, rounded corners, accent).
 */
#include "cgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_CACHE 48

typedef struct app {
    cg_window *win;
    cg_textbuf input, output;
    float in_size, out_size;
    int family, style;
    cg_font *font;
    int loaded_family, loaded_style;
    unsigned synced_version;
    char status[1024];

    /* Small LRU of fonts used for the per-row previews in the dropdown. */
    cg_font *preview[PREVIEW_CACHE];
    int preview_family[PREVIEW_CACHE];
    unsigned preview_used[PREVIEW_CACHE];
    unsigned tick;

    int theme, accent;
    float frame_opacity, body_opacity, radius;
    bool rounded;
} app;

static const cg_color accents[] = {
    { 0x5b, 0x8c, 0xff, 255 }, { 0x2f, 0xc2, 0x8b, 255 }, { 0xf5, 0x9e, 0x0b, 255 },
    { 0xef, 0x44, 0x6f, 255 }, { 0xa7, 0x7b, 0xf3, 255 }, { 0x14, 0xb8, 0xc6, 255 },
};
#define NACCENTS (int)(sizeof accents / sizeof *accents)

static const char *sample_text =
    "The quick brown fox jumps over the lazy dog.\n"
    "Sphinx of black quartz, judge my vow!\n"
    "\n"
    "0123456789  ({[ ]})  @#$%&*  \"quotes\" 'apostrophes'\n"
    "Àçcéntëd lettérs: façade, naïve, Ångström, Straße\n"
    "\n"
    "Type on the left; pick a font on the right.";

/* ---- list sources ------------------------------------------------ */

static const char *family_label(void *user, int i)
{
    (void)user;
    return cg_fontdb_family(i)->name;
}

static cg_font *family_preview(void *user, int i)
{
    app *a = (app *)user;
    a->tick++;
    int victim = 0;
    for (int k = 0; k < PREVIEW_CACHE; k++) {
        if (a->preview[k] && a->preview_family[k] == i) {
            a->preview_used[k] = a->tick;
            return a->preview[k];
        }
        if (a->preview_used[k] < a->preview_used[victim]) victim = k;
    }
    const cg_font_family *fam = cg_fontdb_family(i);
    const cg_font_face *face = &fam->faces[fam->regular];
    cg_font_free(a->preview[victim]);
    a->preview[victim] = cg_font_load(face->path, face->index);
    a->preview_family[victim] = i;
    a->preview_used[victim] = a->tick;
    return a->preview[victim];
}

static const char *style_label(void *user, int i)
{
    app *a = (app *)user;
    return cg_fontdb_family(a->family)->faces[i].style;
}

static const char *theme_label(void *user, int i)
{
    (void)user;
    return cg_theme_get(i)->name;
}

/* ---- helpers ------------------------------------------------------ */

static void load_selected_font(app *a)
{
    if (a->family == a->loaded_family && a->style == a->loaded_style) return;
    const cg_font_family *fam = cg_fontdb_family(a->family);
    if (!fam) return;
    if (a->style < 0 || a->style >= fam->face_count) a->style = fam->regular;
    const cg_font_face *face = &fam->faces[a->style];
    cg_font *f = cg_font_load(face->path, face->index);
    if (f) {
        cg_font_free(a->font);
        a->font = f;
        snprintf(a->status, sizeof a->status, "%s %s  ·  %s", face->family, face->style, face->path);
    } else {
        snprintf(a->status, sizeof a->status, "Could not load %s", face->path);
    }
    a->loaded_family = a->family;
    a->loaded_style = a->style;
}

static void apply_style(app *a)
{
    cg_style *st = cg_window_style(a->win);
    st->theme = *cg_theme_get(a->theme);
    if (a->accent >= 0) st->theme.accent = accents[a->accent];
    st->frame_opacity = a->frame_opacity;
    st->body_opacity = a->body_opacity;
    st->rounded = a->rounded;
    st->corner_radius = a->radius;
}

static void caption(cg_window *win, cg_rect *row, const char *text, float width)
{
    cg_rect r = cg_cut_left(row, width);
    cg_label(win, r, text, CG_ALIGN_LEFT, cg_window_style(win)->theme.text_dim);
}

/* ---- the frame ---------------------------------------------------- */

static void frame(cg_window *win, cg_rect content, void *user)
{
    app *a = (app *)user;
    apply_style(a);
    const cg_theme *th = &cg_window_style(win)->theme;

    cg_rect r = cg_inset(content, 12);
    cg_rect bar = cg_cut_bottom(&r, 32);
    cg_cut_bottom(&r, 10);
    float gap = 14;
    float half = (r.w - gap) * 0.5f;
    cg_rect left = cg_cut_left(&r, half);
    cg_cut_left(&r, gap);
    cg_rect right = r;

    /* Left: input. */
    cg_rect lhead = cg_cut_top(&left, 30);
    cg_cut_top(&left, 8);
    cg_label(win, cg_cut_left(&lhead, 120), "Input", CG_ALIGN_LEFT, th->text);
    cg_spinbox(win, cg_id_str("in.size"), cg_cut_right(&lhead, 110), &a->in_size, 8, 72, 1, "%.0f px");
    cg_cut_right(&lhead, 6);
    cg_label(win, cg_cut_right(&lhead, 34), "Size", CG_ALIGN_RIGHT, th->text_dim);
    cg_textedit(win, cg_id_str("input"), left, &a->input, NULL, a->in_size, 0);

    /* Keep the preview's text in sync with the editor. */
    if (a->input.version != a->synced_version) {
        cg_textbuf_set(&a->output, a->input.data);
        a->synced_version = a->input.version;
    }

    /* Right: font picker + preview. */
    cg_rect rhead = cg_cut_top(&right, 30);
    cg_cut_top(&right, 8);
    cg_spinbox(win, cg_id_str("out.size"), cg_cut_right(&rhead, 110), &a->out_size, 6, 200, 1, "%.0f px");
    cg_cut_right(&rhead, 8);
    const cg_font_family *fam = cg_fontdb_family(a->family);
    if (fam) {
        cg_list_source styles = { fam->face_count, style_label, NULL, NULL, false, a };
        if (cg_dropdown(win, cg_id_str("style"), cg_cut_right(&rhead, 130), &styles, &a->style))
            load_selected_font(a);
        cg_cut_right(&rhead, 8);
    }
    cg_list_source families = { cg_fontdb_family_count(), family_label, family_preview, "Aa Gg 123", true, a };
    int prev_family = a->family;
    if (cg_dropdown(win, cg_id_str("family"), rhead, &families, &a->family) && a->family != prev_family) {
        a->style = cg_fontdb_family(a->family)->regular;
        load_selected_font(a);
    }

    cg_rect status = cg_cut_bottom(&right, 22);
    cg_cut_bottom(&right, 4);
    cg_push_clip(win, status);
    cg_draw_text(win, cg_ui_font(win), 12, status.x + 2, status.y + 5,
                 a->font ? a->status : "No fonts found on this system", -1, th->text_dim);
    cg_pop_clip(win);
    cg_textedit(win, cg_id_str("output"), right, &a->output, a->font, a->out_size, CG_TEXT_READONLY);

    /* Bottom bar: window appearance. */
    cg_fill_rrect(win, bar, 8, cg_rgba(th->control_bg.r, th->control_bg.g, th->control_bg.b, 110));
    cg_rect row = cg_inset(bar, 4);
    row.x += 6;
    row.w -= 6;
    caption(win, &row, "Theme", 54);
    cg_list_source themes = { cg_theme_count(), theme_label, NULL, NULL, false, NULL };
    cg_dropdown(win, cg_id_str("theme"), cg_cut_left(&row, 120), &themes, &a->theme);
    cg_cut_left(&row, 16);
    caption(win, &row, "Frame", 50);
    cg_slider(win, cg_id_str("frame.op"), cg_cut_left(&row, 80), &a->frame_opacity, 0.15f, 1.f);
    cg_cut_left(&row, 14);
    caption(win, &row, "Body", 42);
    cg_slider(win, cg_id_str("body.op"), cg_cut_left(&row, 80), &a->body_opacity, 0.15f, 1.f);
    cg_cut_left(&row, 16);
    cg_checkbox(win, cg_id_str("rounded"), cg_cut_left(&row, 84), "Rounded", &a->rounded);
    cg_slider(win, cg_id_str("radius"), cg_cut_left(&row, 64), &a->radius, 0.f, 28.f);
    cg_cut_left(&row, 16);
    for (int i = -1; i < NACCENTS; i++) {
        cg_color c = i < 0 ? cg_theme_get(a->theme)->accent : accents[i];
        if (cg_swatch(win, cg_id_str("accent") + (cg_id)(i + 2), cg_cut_left(&row, 22), c, a->accent == i))
            a->accent = i;
    }
}

/* ---- main ---------------------------------------------------------- */

int main(void)
{
    int nfam = cg_fontdb_scan();
    app a;
    memset(&a, 0, sizeof a);
    a.in_size = 16;
    a.out_size = 32;
    a.loaded_family = a.loaded_style = -1;
    a.accent = -1;
    a.frame_opacity = 0.92f;
    a.body_opacity = 0.97f;
    a.rounded = true;
    a.radius = 12;
    for (int k = 0; k < PREVIEW_CACHE; k++) a.preview_family[k] = -1;

    a.win = cg_window_create("cgui · Font Viewer", 1100, 700);
    if (!a.win) return 1;
    cg_window_set_min_size(a.win, 820, 460);

    cg_textbuf_init(&a.input, sample_text);
    a.input.placeholder = "Type something…";
    cg_textbuf_init(&a.output, sample_text);
    a.synced_version = a.input.version;

    static const char *prefs[] = { "DejaVu Serif", "Georgia", "Noto Serif", "Times New Roman",
                                   "Liberation Serif", "FreeSerif" };
    a.family = 0;
    for (size_t i = 0; i < sizeof prefs / sizeof *prefs; i++) {
        int f = cg_fontdb_find(prefs[i]);
        if (f >= 0) {
            a.family = f;
            break;
        }
    }
    if (nfam > 0) {
        a.style = cg_fontdb_family(a.family)->regular;
        load_selected_font(&a);
    }

    cg_run(a.win, frame, &a);

    cg_textbuf_free(&a.input);
    cg_textbuf_free(&a.output);
    for (int k = 0; k < PREVIEW_CACHE; k++) cg_font_free(a.preview[k]);
    cg_font_free(a.font);
    cg_window_destroy(a.win);
    return 0;
}

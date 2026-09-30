/*
 * preetum: the main menu, navigation between tools, and the appearance bar
 * shown at the bottom of every screen.
 */
#include "preetum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tool registry. Order = menu order = number-key shortcut. */
static const tool tools[] = {
    { "Font Compare", { "Type on one side, see it in any", "installed font on the other." },
      NEEDS_FONTS, NULL, NULL, compare_icon, compare_frame },
    { "Glyph Map", { "Browse every glyph in a font,", "inspect it, and copy it." },
      NEEDS_FONTS, NULL, glyphs_leave, glyphs_icon, glyphs_frame },
};
#define NTOOLS (int)(sizeof tools / sizeof *tools)

static const cg_color accents[] = {
    { 0x5b, 0x8c, 0xff, 255 }, { 0x2f, 0xc2, 0x8b, 255 }, { 0xf5, 0x9e, 0x0b, 255 },
    { 0xef, 0x44, 0x6f, 255 }, { 0xa7, 0x7b, 0xf3, 255 }, { 0x14, 0xb8, 0xc6, 255 },
};
#define NACCENTS (int)(sizeof accents / sizeof *accents)

/* Switches screens, running the lifecycle: the old tool's leave hook, then
 * the services only it needed are closed; services only the new tool needs
 * are opened, then its enter hook runs. A screen that needs no fonts (the
 * menu) therefore holds no font data; see docs/PREETUM.md. */
static void set_screen(preetum *a, int screen)
{
    if (screen == a->screen) return;
    unsigned had = a->screen >= 0 ? tools[a->screen].needs : 0;
    unsigned want = screen >= 0 ? tools[screen].needs : 0;
    if (a->screen >= 0 && tools[a->screen].leave) tools[a->screen].leave(a);
    if ((had & ~want) & NEEDS_FONTS) fonts_close(a);
    a->screen = screen;
    if ((want & ~had) & NEEDS_FONTS) fonts_open(a);
    if (screen >= 0 && tools[screen].enter) tools[screen].enter(a);
    char title[128];
    if (screen < 0) snprintf(title, sizeof title, "preetum");
    else snprintf(title, sizeof title, "preetum  ·  %s", tools[screen].name);
    cg_window_set_title(a->win, title);
}

static const char *theme_label(void *user, int i)
{
    (void)user;
    return cg_theme_get(i)->name;
}

static void apply_style(preetum *a)
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
    cg_label(win, cg_cut_left(row, width), text, CG_ALIGN_LEFT, cg_window_style(win)->theme.text_dim);
}

static void appearance_bar(cg_window *win, cg_rect bar, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    cg_fill_rrect(win, bar, 8, cg_color_alpha(th->control_bg, 0.45f));
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
    cg_checkbox(win, cg_id_str("rounded"), cg_cut_left(&row, 90), "Rounded", &a->rounded);
    cg_slider(win, cg_id_str("radius"), cg_cut_left(&row, 64), &a->radius, 0.f, 28.f);
    cg_cut_left(&row, 16);
    for (int i = -1; i < NACCENTS; i++) {
        cg_color c = i < 0 ? cg_theme_get(a->theme)->accent : accents[i];
        if (cg_swatch(win, cg_id_str("accent") + (cg_id)(i + 2), cg_cut_left(&row, 22), c, a->accent == i))
            a->accent = i;
    }
}

/* ---- main menu ------------------------------------------------------ */

static void centered_text(cg_window *win, float size, float cx, float y, const char *s, cg_color c)
{
    cg_font *f = cg_ui_font(win);
    float w = cg_text_width(win, f, size, s, -1);
    cg_draw_text(win, f, size, floorf(cx - w * 0.5f), y, s, -1, c);
}

static void menu_card(cg_window *win, cg_rect r, preetum *a, int index)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    cg_font *ui = cg_ui_font(win);
    bool real = index < NTOOLS;
    cg_interaction it = real ? cg_interact(win, cg_id_str(tools[index].name), r) : (cg_interaction){ 0 };

    if (real) {
        cg_color bg = it.hover ? cg_color_mix(th->panel_bg, th->accent, it.down ? 0.16f : 0.08f) : th->panel_bg;
        cg_fill_rrect(win, r, 12, bg);
        cg_stroke_rrect(win, r, 12, it.hover ? 1.5f : 1.f, it.hover ? th->accent : th->panel_border);
    } else {
        cg_stroke_rrect(win, r, 12, 1.f, cg_color_alpha(th->text_dim, 0.35f));
    }

    cg_rect box = cg_rect_make(r.x + 18, r.y + (r.h - 64) * 0.5f, 64, 64);
    if (real) {
        cg_fill_rrect(win, box, 10, th->control_bg);
        tools[index].icon(win, cg_inset(box, 8), a);
    } else {
        float cx = box.x + 32, cy = box.y + 32;
        cg_color c = cg_color_alpha(th->text_dim, 0.6f);
        cg_line(win, cx - 10, cy, cx + 10, cy, 2, c);
        cg_line(win, cx, cy - 10, cx, cy + 10, 2, c);
    }

    float tx = box.x + box.w + 18, lh;
    cg_font_metrics(win, ui, 17, NULL, NULL, &lh);
    const char *name = real ? tools[index].name : "More soon";
    cg_draw_text(win, ui, 17, tx, r.y + 24, name, -1, real ? th->text : th->text_dim);
    const char *l1 = real ? tools[index].blurb[0] : "New tools will show up here.";
    const char *l2 = real ? tools[index].blurb[1] : "";
    cg_push_clip(win, cg_rect_make(tx, r.y, r.x + r.w - tx - 10, r.h));
    cg_draw_text(win, ui, 13, tx, r.y + 30 + lh, l1, -1, th->text_dim);
    cg_draw_text(win, ui, 13, tx, r.y + 48 + lh, l2, -1, th->text_dim);
    cg_pop_clip(win);

    if (real) {
        char key[16];
        snprintf(key, sizeof key, "%d", index + 1);
        cg_draw_text(win, ui, 12, r.x + r.w - 20, r.y + 10, key, -1, cg_color_alpha(th->text_dim, 0.7f));
        if (it.hover) cg_set_cursor(win, CG_CURSOR_HAND);
        if (it.clicked) set_screen(a, index);
    }
}

static void main_menu(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    const float card_w = 300, card_h = 112, gap = 16;
    int ncards = NTOOLS + 1;
    int per_row = (int)((r.w + gap) / (card_w + gap));
    if (per_row < 1) per_row = 1;
    if (per_row > ncards) per_row = ncards;
    int nrows = (ncards + per_row - 1) / per_row;

    float logo_px = 7, logo = LOGO_W * logo_px;
    float hero_h = logo + 12 + 56 + 24;
    float cards_h = nrows * card_h + (nrows - 1) * gap;
    float total = hero_h + 34 + cards_h;
    float y = r.y + fmaxf(0, (r.h - total) * 0.45f);
    float cx = r.x + r.w * 0.5f;

    /* Soft glow behind the logo, then the logo itself (pixel-exact scale). */
    for (int i = 5; i >= 1; i--)
        cg_fill_circle(win, cx, y + logo * 0.5f, logo * (0.35f + i * 0.09f), cg_color_alpha(th->accent, 0.035f));
    cg_draw_image(win, cg_rect_make(floorf(cx - logo * 0.5f), y, logo, logo), a->logo, LOGO_W, LOGO_H);
    y += logo + 12;
    centered_text(win, 46, cx, y, "preetum", th->text);
    y += 56;
    centered_text(win, 14, cx, y, "a growing box of small tools", th->text_dim);
    y += 24 + 34;

    for (int row = 0; row < nrows; row++) {
        int n = row == nrows - 1 ? ncards - row * per_row : per_row;
        float x = cx - (n * card_w + (n - 1) * gap) * 0.5f;
        for (int k = 0; k < n; k++)
            menu_card(win, cg_rect_make(x + k * (card_w + gap), y, card_w, card_h), a, row * per_row + k);
        y += card_h + gap;
    }

    for (int i = 0; i < NTOOLS && i < 9; i++)
        if (cg_key_pressed(win, '1' + i, 0)) set_screen(a, i);
}

/* ---- frame ----------------------------------------------------------- */

static void frame(cg_window *win, cg_rect content, void *user)
{
    preetum *a = (preetum *)user;
    apply_style(a);
    const cg_theme *th = &cg_window_style(win)->theme;

    cg_rect r = cg_inset(content, 12);
    appearance_bar(win, cg_cut_bottom(&r, 32), a);
    cg_cut_bottom(&r, 10);

    if (a->screen < 0) {
        main_menu(win, r, a);
        return;
    }

    /* Tool screens: a back button and the tool's name above its content. */
    cg_rect nav = cg_cut_top(&r, 30);
    cg_cut_top(&r, 10);
    bool back = cg_button(win, cg_id_str("nav.back"), cg_cut_left(&nav, 90), "‹  Menu");
    if (cg_focused(win) == 0 && cg_key_pressed(win, CG_KEY_ESCAPE, 0)) back = true;
    cg_cut_left(&nav, 14);
    const tool *t = &tools[a->screen];
    t->icon(win, cg_cut_left(&nav, 30), a);
    cg_cut_left(&nav, 8);
    cg_label(win, nav, t->name, CG_ALIGN_LEFT, th->text);
    t->frame(win, r, a);
    if (back) set_screen(a, -1);
}

int main(void)
{
    /* Note: no font scanning here. The menu only needs the UI font, which
     * cg_window_create finds on its own; the fonts service loads the rest
     * when a tool that needs it is opened. */
    static preetum a;
    a.screen = -1;
    a.accent = -1;
    a.frame_opacity = 0.92f;
    a.body_opacity = 0.97f;
    a.rounded = true;
    a.radius = 12;
    logo_build(a.logo);

    a.win = cg_window_create("preetum", 1120, 720);
    if (!a.win) return 1;
    cg_window_set_min_size(a.win, 880, 540);
    cg_window_set_icon(a.win, a.logo, LOGO_W, LOGO_H);

    compare_init(&a);
    glyphs_init(&a);

    /* PREETUM_TOOL=n opens tool n (1-based) directly. */
    const char *start = getenv("PREETUM_TOOL");
    if (start && atoi(start) >= 1 && atoi(start) <= NTOOLS) set_screen(&a, atoi(start) - 1);

    cg_run(a.win, frame, &a);

    set_screen(&a, -1); /* runs the open tool's leave hook and closes services */
    compare_free(&a);
    cg_window_destroy(a.win);
    return 0;
}

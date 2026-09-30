/*
 * preetum - a growing collection of small tools built on cgui.
 *
 * main.c owns the window, the main menu and the appearance bar; each tool
 * is a frame function that draws into the area it is given. To add a tool,
 * write a tool_*.c with a frame function and a card icon, then list it in
 * the `tools` table in main.c.
 */
#ifndef PREETUM_H
#define PREETUM_H

#include "cgui.h"

#define PREVIEW_CACHE 48
#define LOGO_W 16
#define LOGO_H 16

typedef struct preetum preetum;

typedef struct tool {
    const char *name;
    const char *blurb[2];  /* two short lines for the menu card */
    void (*icon)(cg_window *win, cg_rect r, preetum *app);
    void (*frame)(cg_window *win, cg_rect r, preetum *app);
} tool;

struct preetum {
    cg_window *win;
    int screen;  /* -1 = main menu, otherwise an index into the tool table */
    uint32_t logo[LOGO_W * LOGO_H];

    /* Font selection, shared by every tool. */
    int family, style;
    int loaded_family, loaded_style;
    cg_font *font;
    char font_desc[1024];

    /* Small LRU of fonts for the per-row previews in the family dropdown. */
    cg_font *preview[PREVIEW_CACHE];
    int preview_family[PREVIEW_CACHE];
    unsigned preview_used[PREVIEW_CACHE];
    unsigned tick;

    /* Appearance bar. */
    int theme, accent;
    float frame_opacity, body_opacity, radius;
    bool rounded;

    /* Font Compare. */
    cg_textbuf input, output;
    float in_size, out_size;
    unsigned synced_version;

    /* Glyph Map. */
    uint32_t *cps;
    int ncps;
    int cps_family, cps_style;  /* which font `cps` was built for */
    float cell;
    float glyph_scroll;
    int glyph_sel;
};

/* logo.c */
void logo_build(uint32_t out[LOGO_W * LOGO_H]);

/* fonts.c */
void fonts_pick_default(preetum *app);
void fonts_load_selected(preetum *app);
/* Family + style dropdowns, laid out right-to-left inside `row`. */
void fonts_picker(cg_window *win, cg_rect row, preetum *app);
void fonts_free(preetum *app);

/* tools */
void compare_init(preetum *app);
void compare_free(preetum *app);
void compare_icon(cg_window *win, cg_rect r, preetum *app);
void compare_frame(cg_window *win, cg_rect r, preetum *app);

void glyphs_init(preetum *app);
void glyphs_free(preetum *app);
void glyphs_icon(cg_window *win, cg_rect r, preetum *app);
void glyphs_frame(cg_window *win, cg_rect r, preetum *app);

#endif

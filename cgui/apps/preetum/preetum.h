/*
 * preetum - a growing collection of small tools built on cgui.
 *
 * main.c owns the window, the main menu and the appearance bar; each tool
 * is a frame function that draws into the area it is given. Shared,
 * expensive resources are "services" (fonts is the first): a tool lists the
 * services it needs, main.c opens them when the tool is entered and closes
 * them when it is left, so nothing heavy is loaded while it is not in use.
 * See docs/PREETUM.md for how to add a tool or a service.
 */
#ifndef PREETUM_H
#define PREETUM_H

#include "cgui.h"

#define PREVIEW_CACHE 48
#define LOGO_W 16
#define LOGO_H 16

typedef struct preetum preetum;

/* Services a tool can ask for (bit flags for tool.needs). */
enum { NEEDS_FONTS = 1 };

typedef struct tool {
    const char *name;
    const char *blurb[2];  /* two short lines for the menu card */
    unsigned needs;        /* NEEDS_* services held while the tool is open */
    /* Optional. enter runs after the tool's services are opened; leave runs
     * before they are closed and should free whatever the tool loaded. */
    void (*enter)(preetum *app);
    void (*leave)(preetum *app);
    /* Draws the menu-card icon. Runs on the main menu, where no services
     * are open, so it must not rely on them (app->font may be NULL). */
    void (*icon)(cg_window *win, cg_rect r, preetum *app);
    void (*frame)(cg_window *win, cg_rect r, preetum *app);
} tool;

enum { FONTS_CLOSED, FONTS_LOADING, FONTS_READY };

struct preetum {
    cg_window *win;
    int screen;  /* -1 = main menu, otherwise an index into the tool table */
    uint32_t logo[LOGO_W * LOGO_H];

    /* Fonts service (fonts.c). Only populated while a NEEDS_FONTS tool is
     * open; the selection survives closing by name, not by index. */
    int fonts_state;
    bool fonts_splash_shown;
    char keep_family[256], keep_style[256];
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

/* fonts.c: the fonts service. */
void fonts_open(preetum *app);   /* marks the service wanted; loading is deferred */
void fonts_close(preetum *app);  /* frees the database, fonts and previews */
/* Call at the top of a NEEDS_FONTS tool's frame; returns false (after
 * drawing a "Loading fonts…" placeholder into r) until fonts are ready. */
bool fonts_ready(cg_window *win, cg_rect r, preetum *app);
void fonts_load_selected(preetum *app);
/* Family + style dropdowns, laid out right-to-left inside `row`. */
void fonts_picker(cg_window *win, cg_rect row, preetum *app);

/* tools */
void compare_init(preetum *app);
void compare_free(preetum *app);
void compare_icon(cg_window *win, cg_rect r, preetum *app);
void compare_frame(cg_window *win, cg_rect r, preetum *app);

void glyphs_init(preetum *app);
void glyphs_leave(preetum *app);
void glyphs_icon(cg_window *win, cg_rect r, preetum *app);
void glyphs_frame(cg_window *win, cg_rect r, preetum *app);

#endif

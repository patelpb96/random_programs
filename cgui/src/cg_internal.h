#ifndef CGUI_INTERNAL_H
#define CGUI_INTERNAL_H

#include "cgui.h"
#include "platform.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/* Software canvas (render.c). Physical pixel coordinates.             */
/* ------------------------------------------------------------------ */

typedef struct cg_canvas {
    uint32_t *px;      /* premultiplied 0xAARRGGBB, stride == w */
    int w, h, cap;
    int cx0, cy0, cx1, cy1;  /* clip rectangle, half-open */
} cg_canvas;

typedef struct pm_color { uint32_t r, g, b, a; } pm_color;  /* premultiplied */

pm_color pm_from(cg_color c, float opacity);
void canvas_resize(cg_canvas *cv, int w, int h);
void canvas_free(cg_canvas *cv);
void canvas_clear(cg_canvas *cv, uint32_t value);
void canvas_set_clip(cg_canvas *cv, int x0, int y0, int x1, int y1);
void canvas_fill_rect(cg_canvas *cv, float x0, float y0, float x1, float y1, pm_color c);
/* radii order: top-left, top-right, bottom-right, bottom-left */
void canvas_fill_rrect(cg_canvas *cv, float x, float y, float w, float h, const float r[4], pm_color c);
void canvas_stroke_rrect(cg_canvas *cv, float x, float y, float w, float h, const float r[4],
                         float t, pm_color c);
void canvas_fill_circle(cg_canvas *cv, float cx, float cy, float r, pm_color c);
void canvas_stroke_circle(cg_canvas *cv, float cx, float cy, float r, float t, pm_color c);
void canvas_line(cg_canvas *cv, float x0, float y0, float x1, float y1, float t, pm_color c);
/* Thick arc with round caps: centre, radius, angles in degrees (0 = +x,
 * counter-clockwise with y pointing up), half-width. */
void canvas_arc(cg_canvas *cv, float cx, float cy, float radius, float a0, float a1,
                float half_width, pm_color c);
/* Nearest-neighbour blit of a straight-alpha ARGB image into a rect. */
void canvas_blit_image(cg_canvas *cv, float x, float y, float w, float h, const uint32_t *argb, int iw,
                       int ih);
void canvas_blit_a8(cg_canvas *cv, int x, int y, const uint8_t *a8, int w, int h, int pitch,
                    pm_color c);

/* ------------------------------------------------------------------ */
/* Fonts (font.c). Sizes here are physical pixels.                     */
/* ------------------------------------------------------------------ */

typedef struct font_run {
    cg_font *font;
    int32_t size26;
    uint32_t prev_gi;
    int32_t pen26;
} font_run;

void font_run_begin(font_run *run, cg_font *font, float px_size);
/* Advances the pen by one codepoint (kerning + advance); returns new pen. */
int32_t font_run_step(font_run *run, uint32_t cp);
float font_draw(cg_canvas *cv, cg_font *font, float px_size, float x, float baseline,
                const char *s, int n, pm_color c);
float font_measure(cg_font *font, float px_size, const char *s, int n);
void font_metrics(cg_font *font, float px_size, float *ascent, float *descent, float *line_h);

int utf8_decode(const char *s, int n, uint32_t *cp);  /* returns bytes used (>=1) */
int utf8_encode(uint32_t cp, char *out);
int utf8_prev(const char *s, int pos);
int utf8_next(const char *s, int len, int pos);

/* ------------------------------------------------------------------ */
/* Window state                                                        */
/* ------------------------------------------------------------------ */

#define CG_MAX_KEYS 64
#define CG_CLIP_STACK 32

typedef struct cg_input {
    float mx, my;
    bool down[3], pressed[3], released[3];
    int clicks;            /* 1 = single, 2 = double, 3 = triple click */
    float wheel_x, wheel_y;
    int mods;
    int keys[CG_MAX_KEYS], key_mods[CG_MAX_KEYS], nkeys;
    char text[256];
    int text_len;
    bool inside;
} cg_input;

typedef struct cg_popup {
    cg_id id;
    bool open, registered;
    cg_rect anchor, rect;
    cg_list_source src;
    int selected, highlight;
    cg_id result_id;
    int result;
    float scroll;
    char filter[128];
    int filter_len;
    int *matches;
    int nmatches, cap;
    bool reveal;           /* scroll the highlighted row into view */
    bool dragging_thumb;
    float drag_offset;
} cg_popup;

struct cg_window {
    plat_window *pw;
    cg_canvas cv;
    float scale;
    int pw_w, pw_h;        /* physical size */
    float w, h;            /* logical size */
    cg_style style;
    char *title;
    int min_w, min_h;

    bool running, dirty, redraw, in_frame;
    double wake_at;
    cg_frame_fn frame_fn;
    void *user;

    cg_input in;
    double last_click_time;
    float last_click_x, last_click_y;
    int click_count;

    /* widget state */
    cg_id hot, active, focus;
    bool active_seen, focus_taken, press_taken;
    bool block_mouse;       /* mouse is over the resize border */
    cg_rect overlay;        /* area covered by the open popup (last frame) */
    bool overlay_valid;
    cg_popup popup;
    int cursor, cursor_shown;
    float drag_anchor, drag_value;   /* shared by slider/spinbox drags */
    double repeat_at;

    cg_font *ui_font;
    cg_rect clips[CG_CLIP_STACK];
    int nclips;

    /* window chrome */
    bool maximized;
    bool alpha_ok;          /* translucency is actually visible */
    bool debug_dump;        /* CGUI_SCREENSHOT is set */
    uint32_t *icon;         /* title-bar icon, straight ARGB */
    int icon_w, icon_h;
    int pending_drag;       /* -1 = none, else edge flags (0 = move) */
    int armed_drag;         /* press on title/edge waiting for movement, -1 = none */
    float armed_x, armed_y;
    int pending_op;
    unsigned shape_serial;
    float shape_key[6];
    int hover_edge;
};

enum { OP_NONE, OP_MINIMIZE, OP_MAXIMIZE, OP_CLOSE };

/* ui.c */
void ui_begin_frame(cg_window *w);
void ui_end_frame(cg_window *w);
bool ui_hover(cg_window *w, cg_rect r);
void ui_popup_end(cg_window *w);

typedef struct ui_behavior { bool hover, pressed, down, released, clicked; } ui_behavior;
ui_behavior ui_behave(cg_window *w, cg_id id, cg_rect r, bool focusable);
void ui_text_in(cg_window *w, cg_rect r, const char *s, int align, cg_color c, float size);

/* window.c */
void chrome_draw(cg_window *w);
cg_rect chrome_content_rect(cg_window *w);
int chrome_hit_edges(cg_window *w, float x, float y);

/* fontdb.c */
const char *fontdb_default_ui_font_path(int *index);

static inline float cg_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int cg_mini(int a, int b) { return a < b ? a : b; }
static inline int cg_maxi(int a, int b) { return a > b ? a : b; }

#endif

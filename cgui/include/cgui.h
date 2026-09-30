/*
 * cgui - a small cross-platform, custom-drawn GUI toolkit in C.
 *
 * Everything inside the window (frame, title bar, window buttons, widgets,
 * text) is rendered by cgui's own software rasterizer into a premultiplied
 * ARGB buffer, which the platform layer (X11, Win32, Cocoa) hands to the OS
 * as a per-pixel-alpha window. That is what makes transparent frames,
 * rounded corners and the curved window-button corner possible.
 *
 * The widget API is immediate mode: you describe the UI every frame from a
 * callback passed to cg_run(), and widgets report interactions through
 * their return values. All coordinates are logical pixels (they are
 * multiplied by the window's DPI scale internally).
 */
#ifndef CGUI_H
#define CGUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Basic types                                                         */
/* ------------------------------------------------------------------ */

typedef struct cg_rect { float x, y, w, h; } cg_rect;
typedef struct cg_color { uint8_t r, g, b, a; } cg_color;
typedef uint64_t cg_id;
typedef struct cg_window cg_window;
typedef struct cg_font cg_font;

static inline cg_color cg_rgba(int r, int g, int b, int a)
{
    cg_color c = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    return c;
}
static inline cg_color cg_rgb(int r, int g, int b) { return cg_rgba(r, g, b, 255); }
/* Multiply a colour's alpha by `a`; mix two colours (t = 0 gives `a`). */
cg_color cg_color_alpha(cg_color c, float a);
cg_color cg_color_mix(cg_color a, cg_color b, float t);

static inline cg_rect cg_rect_make(float x, float y, float w, float h)
{
    cg_rect r = { x, y, w, h };
    return r;
}

/* ------------------------------------------------------------------ */
/* Themes and window style                                             */
/* ------------------------------------------------------------------ */

typedef struct cg_theme {
    const char *name;
    cg_color frame_bg;      /* title bar and border fill */
    cg_color frame_border;  /* 1px outline around the window */
    cg_color title_text;
    cg_color body_bg;       /* window background below the title bar */
    cg_color panel_bg;      /* text areas, popups */
    cg_color panel_border;
    cg_color text;
    cg_color text_dim;
    cg_color accent;
    cg_color accent_text;   /* text drawn on top of the accent colour */
    cg_color control_bg;    /* buttons, dropdowns, spin boxes */
    cg_color control_hover;
    cg_color control_active;
    cg_color selection;     /* text selection highlight */
    cg_color scrollbar;
    cg_color button_track;  /* curved strip behind the window buttons */
    cg_color button_idle;   /* window buttons when not hovered */
    cg_color button_icon;
    cg_color minimize;      /* hover colours of the window buttons */
    cg_color maximize;
    cg_color close;
} cg_theme;

int cg_theme_count(void);
const cg_theme *cg_theme_get(int index);

typedef struct cg_style {
    cg_theme theme;
    float frame_opacity;  /* 0..1, alpha multiplier for title bar + border */
    float body_opacity;   /* 0..1, alpha multiplier for the window body    */
    bool rounded;         /* rounded window corners on/off                  */
    float corner_radius;  /* radius of the top-left and bottom corners      */
    float title_height;   /* title bar height; also the radius of the
                             top-right corner that holds the window buttons */
    float border_width;
    float resize_margin;  /* width of the invisible resize grab zone        */
    float font_size;      /* default UI font size                           */
} cg_style;

/* ------------------------------------------------------------------ */
/* Window and main loop                                                */
/* ------------------------------------------------------------------ */

/* Called once per frame. `content` is the area below the title bar. */
typedef void (*cg_frame_fn)(cg_window *win, cg_rect content, void *user);

cg_window *cg_window_create(const char *title, int width, int height);
void cg_window_destroy(cg_window *win);
void cg_window_set_title(cg_window *win, const char *title);
void cg_window_set_min_size(cg_window *win, int width, int height);
cg_style *cg_window_style(cg_window *win);   /* edit freely, applies next frame */
float cg_window_scale(cg_window *win);
bool cg_window_maximized(cg_window *win);
void cg_window_size(cg_window *win, float *w, float *h);
/* Window icon as straight (non-premultiplied) 0xAARRGGBB pixels, row-major.
 * It is drawn at the left of the title bar with nearest-neighbour scaling
 * (so pixel art stays crisp) and handed to the OS for the taskbar / dock.
 * The pixels are copied. */
void cg_window_set_icon(cg_window *win, const uint32_t *argb, int w, int h);

int cg_run(cg_window *win, cg_frame_fn frame, void *user);
void cg_quit(cg_window *win);
void cg_request_redraw(cg_window *win);
void cg_request_wakeup(cg_window *win, double seconds_from_now);
double cg_time(void);

/* Clipboard (UTF-8). cg_clipboard_get returns a malloc'd string or NULL. */
void cg_clipboard_set(cg_window *win, const char *text);
char *cg_clipboard_get(cg_window *win);

/* ------------------------------------------------------------------ */
/* Input (for writing your own widgets)                                */
/* ------------------------------------------------------------------ */

enum { CG_MOUSE_LEFT = 0, CG_MOUSE_RIGHT = 1, CG_MOUSE_MIDDLE = 2 };

enum {
    CG_MOD_SHIFT = 1,
    CG_MOD_CTRL = 2,
    CG_MOD_ALT = 4,
    CG_MOD_SUPER = 8,  /* Cmd on macOS, Windows key elsewhere */
};
/* The "shortcut" modifier: Cmd on macOS, Ctrl elsewhere. */
#ifdef __APPLE__
#define CG_MOD_SHORTCUT CG_MOD_SUPER
#else
#define CG_MOD_SHORTCUT CG_MOD_CTRL
#endif

/* Letter keys use their upper-case ASCII value ('A'..'Z'), digits '0'..'9'. */
enum {
    CG_KEY_NONE = 0,
    CG_KEY_BACKSPACE = 256,
    CG_KEY_DELETE,
    CG_KEY_ENTER,
    CG_KEY_TAB,
    CG_KEY_ESCAPE,
    CG_KEY_LEFT,
    CG_KEY_RIGHT,
    CG_KEY_UP,
    CG_KEY_DOWN,
    CG_KEY_HOME,
    CG_KEY_END,
    CG_KEY_PAGE_UP,
    CG_KEY_PAGE_DOWN,
};

void cg_mouse_pos(cg_window *win, float *x, float *y);
bool cg_mouse_down(cg_window *win, int button);
bool cg_mouse_pressed(cg_window *win, int button);
bool cg_mouse_released(cg_window *win, int button);
bool cg_mouse_in(cg_window *win, cg_rect r); /* false when covered by a popup */
bool cg_key_pressed(cg_window *win, int key, int mods); /* exact modifier match */

/* Building blocks for custom widgets: hover/press/click tracking for a
 * rectangle, and the mouse cursor to show this frame. */
typedef struct cg_interaction { bool hover, pressed, down, clicked; } cg_interaction;
cg_interaction cg_interact(cg_window *win, cg_id id, cg_rect r);
/* Vertical wheel movement (in notches, + = up) while the mouse is over `r`;
 * the movement is consumed so enclosing areas don't scroll too. */
float cg_wheel(cg_window *win, cg_rect r);
/* Widget holding keyboard focus (a text editor, an open dropdown), or 0. */
cg_id cg_focused(cg_window *win);

enum {
    CG_CURSOR_ARROW,
    CG_CURSOR_IBEAM,
    CG_CURSOR_HAND,
    CG_CURSOR_RESIZE_EW,
    CG_CURSOR_RESIZE_NS,
};
void cg_set_cursor(cg_window *win, int cursor);

/* ------------------------------------------------------------------ */
/* Drawing (logical coordinates)                                       */
/* ------------------------------------------------------------------ */

void cg_fill_rect(cg_window *win, cg_rect r, cg_color c);
void cg_fill_rrect(cg_window *win, cg_rect r, float radius, cg_color c);
void cg_stroke_rrect(cg_window *win, cg_rect r, float radius, float thickness, cg_color c);
void cg_fill_circle(cg_window *win, float cx, float cy, float radius, cg_color c);
void cg_stroke_circle(cg_window *win, float cx, float cy, float radius, float thickness, cg_color c);
void cg_line(cg_window *win, float x0, float y0, float x1, float y1, float thickness, cg_color c);
/* Straight 0xAARRGGBB image scaled into `r` with nearest-neighbour sampling. */
void cg_draw_image(cg_window *win, cg_rect r, const uint32_t *argb, int w, int h);
void cg_push_clip(cg_window *win, cg_rect r); /* intersects with the current clip */
void cg_pop_clip(cg_window *win);

/* ------------------------------------------------------------------ */
/* Fonts and text                                                      */
/* ------------------------------------------------------------------ */

cg_font *cg_font_load(const char *path, int face_index);
void cg_font_free(cg_font *font);
cg_font *cg_ui_font(cg_window *win);
const char *cg_font_path(const cg_font *font);
/* Unicode codepoints the font has glyphs for, ascending. With out == NULL
 * it only counts them. Returns the total count. */
int cg_font_codepoints(cg_font *font, uint32_t *out, int max);

/* `n` < 0 means NUL-terminated. `y` is the top of the line box.
 * Returns the x coordinate where the text ends. */
float cg_draw_text(cg_window *win, cg_font *font, float size, float x, float y,
                   const char *s, int n, cg_color c);
float cg_text_width(cg_window *win, cg_font *font, float size, const char *s, int n);
void cg_font_metrics(cg_window *win, cg_font *font, float size,
                     float *ascent, float *descent, float *line_height);

/* System font database: scans the platform's font directories. */
typedef struct cg_font_face {
    char *family;
    char *style;
    char *path;
    int index;    /* face index for FreeType (collections, variable instances) */
    int weight;   /* 100..900 */
    bool italic;
} cg_font_face;

typedef struct cg_font_family {
    char *name;
    cg_font_face *faces;
    int face_count;
    int regular;  /* index into faces of the most "regular" style */
} cg_font_family;

/* Scanning reads every font file's header, which can take a noticeable
 * fraction of a second on systems with thousands of fonts, so call it only
 * when something actually needs the list. Creating a window does not scan
 * (the UI font is found at a well-known path when possible). */
int cg_fontdb_scan(void);  /* returns the family count; cached after first call */
/* Frees the database. Every cg_font_family / cg_font_face pointer and index
 * obtained before becomes invalid; cg_fontdb_scan() rebuilds it. Loaded
 * cg_font objects are independent and stay valid. */
void cg_fontdb_release(void);
bool cg_fontdb_loaded(void);
int cg_fontdb_family_count(void);
const cg_font_family *cg_fontdb_family(int index);
int cg_fontdb_find(const char *family_name); /* case-insensitive, -1 if missing */

/* ------------------------------------------------------------------ */
/* Layout helpers ("rect cut")                                         */
/* ------------------------------------------------------------------ */

cg_rect cg_cut_left(cg_rect *r, float a);
cg_rect cg_cut_right(cg_rect *r, float a);
cg_rect cg_cut_top(cg_rect *r, float a);
cg_rect cg_cut_bottom(cg_rect *r, float a);
cg_rect cg_inset(cg_rect r, float d);
bool cg_rect_contains(cg_rect r, float x, float y);

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

cg_id cg_id_str(const char *s);
cg_id cg_id_ptr(const void *p);

enum { CG_ALIGN_LEFT, CG_ALIGN_CENTER, CG_ALIGN_RIGHT };

/* Colour with alpha 0 means "use the theme's text colour". */
void cg_label(cg_window *win, cg_rect r, const char *text, int align, cg_color c);
bool cg_button(cg_window *win, cg_id id, cg_rect r, const char *label);
bool cg_checkbox(cg_window *win, cg_id id, cg_rect r, const char *label, bool *value);
bool cg_slider(cg_window *win, cg_id id, cg_rect r, float *value, float min, float max);
bool cg_spinbox(cg_window *win, cg_id id, cg_rect r, float *value,
                float min, float max, float step, const char *fmt);
bool cg_swatch(cg_window *win, cg_id id, cg_rect r, cg_color c, bool selected);
/* Vertical scrollbar for a view of height `view` over `content`. Handles
 * dragging the thumb and clicking the track; returns true when *scroll
 * changed. Draws nothing when everything fits. */
bool cg_scrollbar(cg_window *win, cg_id id, cg_rect track, float *scroll, float content, float view);

/* Items for dropdowns are pulled through callbacks, so large lists (like
 * every font on the system) need no copying. `preview_font` is optional:
 * when set, each row also shows `preview_text` drawn in that font. The
 * struct is copied, but the callbacks' data must stay valid for the frame. */
typedef struct cg_list_source {
    int count;
    const char *(*label)(void *user, int index);
    cg_font *(*preview_font)(void *user, int index);
    const char *preview_text;
    bool filterable;  /* show a type-to-filter search field */
    void *user;
} cg_list_source;

bool cg_dropdown(cg_window *win, cg_id id, cg_rect r, const cg_list_source *src, int *selected);

/* Multi-line, word-wrapping text editor. */
typedef struct cg_textbuf {
    char *data;           /* always NUL-terminated UTF-8 */
    int len, cap;
    int caret, anchor;    /* byte offsets; selection is [min, max) */
    float scroll;         /* vertical scroll offset (logical px) */
    unsigned version;     /* bumped on every change */
    const char *placeholder;
    struct cg_textbuf_priv *priv;
} cg_textbuf;

enum { CG_TEXT_READONLY = 1 };

void cg_textbuf_init(cg_textbuf *tb, const char *text);
void cg_textbuf_set(cg_textbuf *tb, const char *text);
void cg_textbuf_free(cg_textbuf *tb);
/* Returns true when the text was changed by the user this frame. */
bool cg_textedit(cg_window *win, cg_id id, cg_rect r, cg_textbuf *tb,
                 cg_font *font, float size, int flags);

#ifdef __cplusplus
}
#endif

#endif /* CGUI_H */

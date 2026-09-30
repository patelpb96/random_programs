/*
 * The platform layer: the only OS-specific part of cgui. Each backend
 * (platform_x11.c, platform_win32.c, platform_cocoa.m) implements these
 * functions. All sizes and coordinates here are physical pixels.
 */
#ifndef CGUI_PLATFORM_H
#define CGUI_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct plat_window plat_window;

enum {
    PE_NONE,
    PE_MOUSE_MOVE,   /* x, y */
    PE_MOUSE_DOWN,   /* x, y, button */
    PE_MOUSE_UP,     /* x, y, button */
    PE_MOUSE_LEAVE,
    PE_WHEEL,        /* wheel_x, wheel_y in "notches" (+y = scroll up) */
    PE_KEY_DOWN,     /* key, mods */
    PE_TEXT,         /* text (UTF-8) */
    PE_RESIZE,       /* w, h */
    PE_SCALE,        /* DPI scale changed */
    PE_CLOSE,
    PE_FOCUS,
    PE_UNFOCUS,
    PE_EXPOSE,
    PE_STATE,        /* maximized / minimized state changed */
};

typedef struct plat_event {
    int type;
    int x, y;
    int button;
    int key;
    int mods;
    float wheel_x, wheel_y;
    char text[16];
    int w, h;
} plat_event;

/* Resize/move edges, combined as bit flags. 0 = move the whole window. */
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };

enum {
    CURSOR_ARROW,
    CURSOR_IBEAM,
    CURSOR_HAND,
    CURSOR_RESIZE_EW,
    CURSOR_RESIZE_NS,
    CURSOR_RESIZE_NWSE,
    CURSOR_RESIZE_NESW,
    CURSOR_COUNT
};

bool plat_init(void);
void plat_shutdown(void);

plat_window *plat_window_create(const char *title, int w, int h);
void plat_window_destroy(plat_window *pw);
void plat_window_set_title(plat_window *pw, const char *title);
void plat_window_set_min_size(plat_window *pw, int w, int h);
void plat_window_size(plat_window *pw, int *w, int *h);
float plat_window_scale(plat_window *pw);
/* True when translucent pixels really show what is behind the window
 * (on X11 this needs an ARGB visual and a running compositor). */
bool plat_window_has_alpha(plat_window *pw);

/* Show a premultiplied 0xAARRGGBB buffer (stride = w). `shape_serial`
 * changes whenever the window outline (size, corner rounding) changed,
 * so backends without a compositor can update a shape mask cheaply. */
void plat_window_present(plat_window *pw, const uint32_t *pixels, int w, int h,
                         unsigned shape_serial);

/* Interactive move (edges == 0) or resize. Called with the left button held.
 * May block in a modal loop; during it the backend calls the refresh callback
 * whenever the window must be redrawn at a new size. */
void plat_window_begin_drag(plat_window *pw, int edges);
void plat_window_minimize(plat_window *pw);
void plat_window_toggle_maximize(plat_window *pw);
bool plat_window_is_maximized(plat_window *pw);
void plat_window_set_cursor(plat_window *pw, int cursor);
void plat_window_set_refresh(plat_window *pw, void (*fn)(void *), void *ctx);

bool plat_poll_event(plat_window *pw, plat_event *ev);
void plat_wait_events(plat_window *pw, int timeout_ms); /* <0 = forever */

void plat_clipboard_set(plat_window *pw, const char *text);
char *plat_clipboard_get(plat_window *pw);

double plat_time(void);

/* Font directories to scan; returns a count and fills `out` with malloc'd
 * paths (caller frees). */
int plat_font_dirs(char **out, int max);

#endif
